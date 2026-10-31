/* 1.4.3 has a new compiler and frame-controller layout. Every adapted game-code
   site below is checked before any write; older versions use game_patches.c. */
#include "aspect_patch.h"
#include "world_patch.h"
#include "game_patches.h"
#include "game_version.h"
#include "world_survey.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <math.h>
#include <TargetConditionals.h>
#include <sys/sysctl.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#else
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

typedef struct { uint64_t off; uint32_t original, replacement; } Patch;
/* The text padding after main's text segment (1024 bytes on 1.4.3 before read-only data), as offsets from the stub base
   (text_end rounded up to 16). scripts/verify_v143_output.py and verify_v143_timing.py check the same layout.
     +0    frame-step producer veneer (16)          +16   output / display-config veneer (28)
     +48   aspect, 4 x 32 (aspect_patch.h)          +176  cloth/hair clock (19 words, to +252)
     +256  high-FPS trampoline bounce (20)          +288  field of view, 2 x 48 (aspect_patch.h)
     +384  render resolution + pick kick (32)       +416  world: time of day, render distance x 2 (3 x 32)
     +512  fog (48)                                 +560  shadow resolution (40), to +600
     +608  high-FPS fade minimum step, 2 x 40 (Mac only), to +688
     +688  noclip god mode, registerDamage (80, Mac only), to +768 */
enum { CLOTH_STUB_AT = 176, BOUNCE_STUB_AT = 256, FOV_STUB_AT = 288, RENDER_VENEER = 384, WORLD_STUBS = 416,
       FOG_STUB_AT = 512, SHADOW_STUB_AT = 560, FADE_STUB_AT = 608, STUBS_END = 768 };
static const Patch fps60[] = {
    {0x2db204, 0x6b08015f, 0x7100055f},
    {0x2db208, 0x1a89b10a, 0x1a9fa52a},
    {0x2db22c, 0xb940468a, 0xd503201f},
    {0x2db360, 0x6b0801bf, 0x710005bf},
    {0x2db36c, 0x1a88c1ac, 0x1a9fc5ac},
    {0x2db340, 0xb9400288, 0x52800028},
    {0x2db34c, 0x2a0a03f5, 0x1a9fa555},
};


static _Atomic uint64_t *frame_step_data;
static _Atomic uint32_t *frame_interval_data;
static _Atomic uint32_t pinned_step_us;
static uint64_t last_present_ns;
static double frame_samples[5];
static unsigned frame_sample_count,frame_sample_next;
static void frame_step_store(float seconds){
    float values[3]={seconds*30.0f,seconds,seconds*60.0f};uint64_t bits;uint32_t interval;
    memcpy(&bits,values,8);memcpy(&interval,values+2,4);
    atomic_store_explicit(frame_step_data,bits,memory_order_release);
    atomic_store_explicit(frame_interval_data,interval,memory_order_release);
}
void runtime_frame_step_pin(double ms){
    uint32_t us=ms>0?(uint32_t)(ms*1000.0+0.5):0;atomic_store(&pinned_step_us,us);
    if(frame_step_data&&us)frame_step_store((float)(us/1e6));
    fprintf(stderr,"[totk] 1.4.3 frame step %s (pin %.3f ms)\n",us?"pinned":"real time",us/1000.0);
}
static void render_latch(void);static void output_latch(uint64_t now_ns);
void runtime_frame_step_update(uint64_t now_ns){
    output_latch(now_ns);
    render_latch();   /* the present boundary: a live scene size takes effect between two frames */
    if(!frame_step_data||!now_ns)return;
    if(atomic_load_explicit(&pinned_step_us,memory_order_relaxed)){last_present_ns=now_ns;return;}
    if(!last_present_ns){last_present_ns=now_ns;return;}
    double dt=(double)(now_ns-last_present_ns)*1e-9;last_present_ns=now_ns;
    if(dt>0.25||dt<0.001){frame_sample_count=frame_sample_next=0;return;}
    /* No 1/120 s floor, as in TOTK 1.0-1.2.1 (game_patches.c) and BotW since 2338bb0: a present shorter than 1/120 s
       counted as 1/120 s made the game run fast above 120 fps (1.2x at 144, 2x at 240) and with jitter at the 120 cap
       (each short interval raised, each long one kept). A hitch is still cut at 1/15 s per interval, as before (upstream ce7f0b3). */
    if(dt>1.0/15.0)dt=1.0/15.0;
    frame_samples[frame_sample_next++%5]=dt;if(frame_sample_count<5)frame_sample_count++;
    double sum=0;for(unsigned i=0;i<frame_sample_count;i++)sum+=frame_samples[i];
    frame_step_store((float)(sum/frame_sample_count));
}
static uint32_t adrp(unsigned reg,uint64_t pc,uint64_t target){
    int64_t pages=(int64_t)(target>>12)-(int64_t)(pc>>12);
    if(pages<-(1<<20)||pages>=(1<<20))runtime_fail("1.4.3 data outside ADRP range");
    uint32_t imm=(uint32_t)pages&0x1fffff;
    return 0x90000000|((imm&3)<<29)|((imm>>2)<<5)|reg;
}
static uint32_t branch(uint64_t pc,uint64_t target){
    int64_t delta=(int64_t)target-(int64_t)pc;
    if((delta&3)||delta<-(1<<27)||delta>=(1<<27))runtime_fail("1.4.3 patch branch outside range");
    return 0x14000000|(((uint32_t)(delta>>2))&0x3ffffff);
}
/* The actor animation update has a second, unscaled clock for cloth/hair
 * (ordinary animation already receives FrameRateMgr's real-time step). Its
 * explicit time-factor path still constructs {factor / 30, factor} every
 * rendered frame. At 120 FPS that advances four seconds per real second.
 * Correct only that clock, retaining pauses, slow motion and the fallback to
 * the ordinary clock. 1.4.3 inlines the update in two actor paths; two other
 * callers share the out-of-line update at +0x2a0e40. Its third caller already
 * passes measured time and MUST NOT be scaled a second time.
 *
 * The inline veneers use only the original destination s register and x8,
 * which the next original load overwrites. At the two call boundaries x8/s4
 * are caller-saved scratch; the veneer tail-calls the original function with
 * its original LR. No stack, flags, ordinary-clock arguments or callee-saved
 * registers are changed. Stubs follow the four 32-byte aspect veneers. */
static void patch_secondary_animation(Module *m,uint64_t data,uint64_t stub){
    const char *enabled=getenv("TOTK_CLOTH_REALTIME");
    if(enabled&&!strcmp(enabled,"0"))return;   /* diagnostic comparison */
    const Patch sites[]={
        {0x246eac,0x1e200940,0}, /* fmul s0,s10,s0: inline clock */
        {0x345904,0x1e210801,0}, /* fmul s1,s0,s1: inline clock */
        {0x24944c,0x94015e7d,0}, /* bl 0x2a0e40: actor with transform override */
        {0x28bc98,0x9400546a,0}, /* bl 0x2a0e40: includes both transform branches */
    };
    enum { WORDS=19 };
    if(stub+WORDS*4>((m->text_end+0x3fff)&~0x3fffULL)||data+8>m->size)
        runtime_fail("1.4.3 secondary animation patch layout");
    uint32_t code[WORDS]={
        adrp(8,(uintptr_t)m->base+stub,(uintptr_t)m->base+data),
        0xbd400100,0x1e200940,0xbd0367e0, /* ratio * factor -> sp+0x364 */
        0xbd400500,0x1e200940,branch(stub+24,0x246eb0), /* seconds * factor */
        adrp(8,(uintptr_t)m->base+stub+28,(uintptr_t)m->base+data),
        0xbd400101,0x1e210801,0xbd0117e1, /* ratio * factor -> sp+0x114 */
        0xbd400501,0x1e210801,branch(stub+52,0x345908),
        adrp(8,(uintptr_t)m->base+stub+56,(uintptr_t)m->base+data),
        0xbd400104,0x1e240842,0x1e240863, /* s2/s3 *= measured 30 Hz ratio */
        branch(stub+72,0x2a0e40),
    };
    uint32_t jumps[]={branch(sites[0].off,stub),branch(sites[1].off,stub+28),
        branch(sites[2].off,stub+56)|0x80000000u,branch(sites[3].off,stub+56)|0x80000000u};
    for(unsigned i=0;i<sizeof sites/sizeof *sites;i++){
        if(sites[i].off+4>m->text_end)runtime_fail("1.4.3 secondary animation site outside text");
        uint32_t word;memcpy(&word,m->base+sites[i].off,4);
        if(word!=(TARGET_OS_IPHONE?jumps[i]:sites[i].original))
            runtime_fail("1.4.3 secondary animation instruction mismatch");
    }
#if TARGET_OS_IPHONE
    if(memcmp(m->base+stub,code,sizeof code))runtime_fail("1.4.3 embedded secondary animation veneer mismatch");
#else
    for(unsigned i=0;i<sizeof code;i++)if(m->base[stub+i])runtime_fail("1.4.3 secondary animation padding in use");
    memcpy(m->base+stub,code,sizeof code);
    for(unsigned i=0;i<sizeof sites/sizeof *sites;i++)memcpy(m->base+sites[i].off,jumps+i,4);
#endif
    fprintf(stderr,"[totk] 1.4.3 cloth/hair clock: real-time seconds and ratio at 4 guarded actor sites; ordinary animation and time factors retained\n");
}
static void patch_realtime(Module *m){
    const char *enabled=getenv("TOTK_REALTIME");if(enabled&&!strcmp(enabled,"0"))return;
    uint64_t data=(m->file_size+0x3fff)&~0x3fffULL;
    /* Keep executable veneers inside the text segment. iOS embeds x0 as a
     * signed RX segment, so these bytes must be emitted by export-ios rather
     * than written after the app starts. */
    uint64_t stub=(m->text_end+15)&~15ULL;
    if(data+0x4000!=m->size||stub+16>((m->text_end+0x3fff)&~0x3fffULL))runtime_fail("1.4.3 frame-step patch layout");
    const Patch guards[]={
        {0x3e5658,0x1e210808,0},{0x2daa54,0xbd425260,0},{0x2daa58,0x52a84e08,0},
        {0x2daa6c,0x5e21d800,0},{0x2daa70,0x1e201820,0},{0x2daa74,0x1e2c1001,0},{0x2daa78,0x1e210800,0}};
    for(unsigned i=0;i<sizeof guards/sizeof *guards;i++){
#if !TARGET_OS_IPHONE
        uint32_t word;memcpy(&word,m->base+guards[i].off,4);
        if(word!=guards[i].original)runtime_fail("1.4.3 real-time original instruction mismatch");
#endif
    }
#if !TARGET_OS_IPHONE
    for(unsigned i=0;i<16;i++)if(m->base[stub+i])runtime_fail("1.4.3 frame-step veneer padding not empty");
#endif
    /* Replace the nominal 60/FPS interval BEFORE the original producer applies
     * its 0.5/1.0 selector (sys+0x1da bit 0). Both FrameRateMgr call sites use
     * this s8 value. Hooking the callee entry instead would discard that scale
     * and affect any other intentional interval setter. Keep the original fmul
     * and all FrameRateMgr ratio/inverse/seconds/FPS arithmetic untouched.
     * x9 contains live task flags here: use x8, which the next ldxr overwrites.
     * s0 is dead until the call sites copy s8 into it again. */
    uint32_t veneer[]={adrp(8,(uintptr_t)m->base+stub,(uintptr_t)m->base+data),0xbd400900,0x1e210808,branch(stub+12,0x3e565c)};
 #if TARGET_OS_IPHONE
    if(memcmp(m->base+stub,veneer,sizeof veneer))runtime_fail("1.4.3 embedded frame-step veneer mismatch");
 #else
    memcpy(m->base+stub,veneer,sizeof veneer);
 #endif
    uint32_t words[]={branch(0x3e5658,stub),adrp(9,(uintptr_t)m->base+0x2daa54,(uintptr_t)m->base+data),0xbd400120,0xd503201f,0xd503201f,0xd503201f,0xd503201f};
 #if TARGET_OS_IPHONE
    for(unsigned i=0;i<sizeof guards/sizeof *guards;i++){uint32_t got;memcpy(&got,m->base+guards[i].off,4);if(got!=words[i])runtime_fail("1.4.3 embedded real-time patch mismatch");}
 #else
    for(unsigned i=0;i<sizeof guards/sizeof *guards;i++)memcpy(m->base+guards[i].off,words+i,4);
 #endif
    frame_step_data=(_Atomic uint64_t *)(void *)(m->base+data);
    frame_interval_data=(_Atomic uint32_t *)(void *)(m->base+data+8);frame_step_store(1.0f/60.0f);
    patch_secondary_animation(m,data,stub+CLOTH_STUB_AT);
    fprintf(stderr,"[totk] 1.4.3 real-time frame step: 7 guarded sites, producer veneer +%#llx; original half-step selector and ratio/inverse/seconds/FPS calculations retained\n",(unsigned long long)stub);
}
/* High frame rates (#61, sky-island trampolines): the contact code at main+0x562a64 scales the bounce vector by 0.1
   when X+0x28 > 100, X being the time object 0x3e58a4 feeds (X+0x28 = 30 / (30 Hz ratio * time scale), X+0x38 the
   ratio). The shipping game steps at 30 Hz (ratio 1), so the check means "time slowed below 0.3"; above 100 FPS it also
   fires with time running normally. The veneer compares X+0x28 * X+0x38 = 30 / time scale instead: the same value at
   30 Hz, independent of the frame rate. TOTK_HIGHFPS_FIX=0 keeps the original check. */
static void patch_slowmo_bounce(Module *m){
    const char *e=getenv("TOTK_HIGHFPS_FIX");if(e&&!strcmp(e,"0"))return;
    /* after the ultrawide veneers (+48, 4 x 32 bytes) and the cloth/hair clock (+176, 19 words); read-only data starts
       at the next 4 KiB page (the layout at the top of this file) */
    uint64_t stub=((m->text_end+15)&~15ULL)+BOUNCE_STUB_AT;
    if(stub+20>((m->text_end+0xfff)&~0xfffULL))runtime_fail("1.4.3 bounce veneer layout");
    uint32_t veneer[]={0xbd402920,   /* ldr s0, [x9, #0x28]: X+0x28 */
        0xbd403921,                  /* ldr s1, [x9, #0x38]: the 30 Hz ratio (s1 is set again by the next original instruction) */
        0x1e210800,                  /* fmul s0, s0, s1 */
        0x52a85909,                  /* mov w9, #0x42c80000: the original 100.0 */
        branch(stub+16,0x562a6c)};
    uint32_t word=branch(0x562a64,stub);
#if TARGET_OS_IPHONE
    uint32_t got;memcpy(&got,m->base+0x562a64,4);
    if(got!=word||memcmp(m->base+stub,veneer,sizeof veneer))runtime_fail("1.4.3 embedded bounce patch mismatch");
#else
    const Patch guards[]={{0x562a64,0xbd402920,0},{0x562a68,0x52a85909,0},{0x562a6c,0x1e270121,0}};
    for(unsigned i=0;i<sizeof guards/sizeof *guards;i++){uint32_t w;memcpy(&w,m->base+guards[i].off,4);if(w!=guards[i].original)runtime_fail("1.4.3 bounce check: original instruction mismatch");}
    for(unsigned i=0;i<sizeof veneer;i++)if(m->base[stub+i])runtime_fail("1.4.3 bounce veneer padding not empty");
    memcpy(m->base+stub,veneer,sizeof veneer);memcpy(m->base+0x562a64,&word,4);
#endif
    fprintf(stderr,"[totk] 1.4.3 high-FPS fix: trampoline slow-motion check uses the time scale, veneer +%#llx\n",(unsigned long long)stub);
}
#if !TARGET_OS_IPHONE
/* High-FPS fades (upstream #466, fix from open PR #1134; upstream 3e9e7f6 / #1112 forces 0 -> 1 always). Static placement
   and shared-record fades use alpha = counter / 20 and advance the counter by trunc(2.1 * 30 Hz ratio) each frame; above
   ~63 fps that is 0, so the counter never moves and rocks, lanterns and walls stay invisible or see-through. At the four
   conversion sites (fcvtzs w8 / w10, s0) a BL to a 40-byte veneer gives 1 where a finite positive step truncates to 0;
   zero (paused), negative and non-finite steps keep the original conversion, and 30 / 60 fps are unchanged. Fades then
   finish faster than at 30 Hz above ~63 fps (about 2x at 120 fps updated every frame); FrameRateMgr and game time are
   not touched. Placement sites (0xa7ac4, 0x95dd24) produce w8, shared-record sites (0x6155e4, 0x615660) w10. Both
   veneers use w11 / w12, which every path after the sites writes before reading, keep s0, NZCV and FPSR as the
   original, and return through x30, which each enclosing function reloads from its frame before it returns.
   Stubs at +608 and +648, after the shadow stub. Mac only: the iOS export (TOTK_EXPORT_IOS) leaves the four sites
   alone. TOTK_PREACTOR_FADE_FIX=0 keeps the original conversion at all four. */
static void patch_preactor_fade(Module *m){
    if(getenv("TOTK_EXPORT_IOS"))return;   /* the Mac host also builds the phone's code */
    const char *enabled=getenv("TOTK_PREACTOR_FADE_FIX");
    if(enabled&&!strcmp(enabled,"0"))return;
    uint64_t stub=((m->text_end+15)&~15ULL)+FADE_STUB_AT,stubs[]={stub,stub+40};
    const Patch guards[]={   /* each site and the add that consumes its result */
        {0xa7ac4,0x1e380008,0},{0xa7ac8,0x3942792a,0},    /* fcvtzs w8, s0 / ldrb w10, [x9, #0x9e] */
        {0x95dd24,0x1e380008,0},{0x95dd28,0x0b080128,0},  /* fcvtzs w8, s0 / add w8, w9, w8 */
        {0x6155e4,0x1e38000a,0},{0x6155e8,0x0b0a012b,0},  /* fcvtzs w10, s0 / add w11, w9, w10 */
        {0x615660,0x1e38000a,0},{0x615664,0x0b0a012b,0},
    };
    /* fcvtzs wD, s0; cbnz wD, ret; fmov w11, s0; cbz w11, ret (+0.0); tbnz w11, #31, ret (negative);
       ubfx w12, w11, #23, #8; add w12, w12, #1; tbnz w12, #8, ret (exponent 255: inf / NaN); mov wD, #1; ret */
    static const uint32_t code[2][10]={
        {0x1e380008,0x35000108,0x1e26000b,0x340000cb,0x37f800ab,0x5317796c,0x1100058c,0x3740004c,0x52800028,0xd65f03c0},
        {0x1e38000a,0x3500010a,0x1e26000b,0x340000cb,0x37f800ab,0x5317796c,0x1100058c,0x3740004c,0x5280002a,0xd65f03c0},
    };
    enum { SITES=4 };
    if(stubs[1]+sizeof code[1]>((m->text_end+15)&~15ULL)+STUBS_END||stubs[1]+sizeof code[1]>((m->text_end+0xfff)&~0xfffULL))
        runtime_fail("1.4.3 fade veneer layout");
    for(unsigned i=0;i<SITES*2;i++){
        uint32_t word;
        if(guards[i].off+4>m->text_end)runtime_fail("1.4.3 fade site outside text");
        memcpy(&word,m->base+guards[i].off,4);
        if(word!=guards[i].original)runtime_fail("1.4.3 fade: original instruction mismatch");
    }
    for(unsigned s=0;s<2;s++)for(unsigned i=0;i<sizeof code[s];i++)
        if(m->base[stubs[s]+i])runtime_fail("1.4.3 fade veneer padding not empty");
    uint32_t jumps[SITES];
    for(unsigned i=0;i<SITES;i++)jumps[i]=branch(guards[i*2].off,stubs[i/2])|0x80000000u;   /* bl */
    for(unsigned s=0;s<2;s++)memcpy(m->base+stubs[s],code[s],sizeof code[s]);
    for(unsigned i=0;i<SITES;i++)memcpy(m->base+guards[i*2].off,jumps+i,4);
    fprintf(stderr,"[totk] 1.4.3 high-FPS fade fix: a finite positive step counts at least 1 at four fade sites, veneers +%#llx\n",
            (unsigned long long)stub);
}
#endif
/* Noclip god mode (2026-10-30): no damage at all to Link while noclip's guard is on (world_survey.c noclip_guard: noclip on,
   and after it until he has stopped). Switching noclip off at 2103 m killed him on landing with the fall calculator reset
   and his life refilled every frame: a blow larger than all his hearts kills before the next refill.
   Every hit reaches the game through one function, InternalDamageCalculator::registerDamage (1.4.3 main+0x318f6c; the
   1.4.3 address afiaka87/totk-mods self-recall hooks, MIT, RecallMovement.cpp RegisterInternalDamageHook, from its 1.2.1
   0x9e4838): x0 the calculator (its actor at +0x8), x2 the damage packet (amount s32 +0x0, type u32 +0x14, mask u32
   +0x20). It returns at once when the type or the mask is 0, otherwise ORs 1 << type into calculator +0x14 and adds the
   amount to calculator +0x18, which the life update takes off later. Its callers in 1.4.3: 0x31408c (type 4, the fall:
   the amount from a virtual call on the packet, registered when at least 1), 0x301024 (type 13) on the player
   component's calculator at +0x1218 (the one noclip_guard resets the fall in: +0x8 is the actor there too), 0x1cd9480
   (type 7, the same calculator's 0x1cd9380) and 0x441398 (type 14). Its first word, mov x8, x2, becomes a branch to a
   veneer: while the data page +NOCLIP_DAMAGE_DATA (u64) holds an actor and the packet's calculator names that actor,
   with a non-zero type and mask (anything else the original drops itself), the packet is not registered (amount and
   type at +8 / +12, a count at +16 for the log); otherwise it runs mov x8, x2 and goes on at 0x318f70. x8, x9 and x10
   are scratch: the original writes all three before reading them. Flags are not live at a function entry; no stack.
   world_survey.c writes the player there while guarded and 0 when the guard ends. Enemies and every other actor take
   damage as before. Mac only (the iOS export has no noclip and leaves the word alone). TOTK_NOCLIP_NODAMAGE=0: off. */
#define NOCLIP_DAMAGE_DATA 0x200u
enum { NOCLIP_DAMAGE_STUB_AT = 688 };
_Atomic uint64_t *runtime_noclip_damage_block;
#if !TARGET_OS_IPHONE
static void patch_noclip_damage(Module *m){
    if(getenv("TOTK_EXPORT_IOS"))return;   /* the Mac host also builds the phone's code */
    const char *e=getenv("TOTK_NOCLIP_NODAMAGE");if(e&&!strcmp(e,"0"))return;
    enum { SITE=0x318f6c, WORDS=20 };
    uint64_t data=(m->file_size+0x3fff)&~0x3fffULL,stub=((m->text_end+15)&~15ULL)+NOCLIP_DAMAGE_STUB_AT;
    if(stub+WORDS*4>((m->text_end+15)&~15ULL)+STUBS_END||stub+WORDS*4>((m->text_end+0xfff)&~0xfffULL)||
       data+NOCLIP_DAMAGE_DATA+24>m->size||SITE+8>m->text_end)
        runtime_fail("1.4.3 noclip damage veneer layout");
    uint32_t got[2];memcpy(got,m->base+SITE,8);
    if(got[0]!=0xaa0203e8||got[1]!=0xb8414d09)   /* mov x8, x2 / ldr w9, [x8, #0x14]! */
        runtime_fail("1.4.3 noclip damage: registerDamage does not match");
    for(unsigned i=0;i<WORDS*4;i++)if(m->base[stub+i])runtime_fail("1.4.3 noclip damage veneer padding not empty");
    uint32_t code[WORDS]={
        adrp(8,(uintptr_t)m->base+stub,(uintptr_t)m->base+data),
        0xf9410109,   /* ldr x9, [x8, #0x200]: the guarded actor, 0 = off */
        0xb4000209,   /* cbz x9, original */
        0xb940144a,   /* ldr w10, [x2, #0x14]: type */
        0x340001ca,   /* cbz w10, original */
        0xb940204a,   /* ldr w10, [x2, #0x20]: mask */
        0x3400018a,   /* cbz w10, original */
        0xf940040a,   /* ldr x10, [x0, #8]: the calculator's actor */
        0xeb09015f,   /* cmp x10, x9 */
        0x54000121,   /* b.ne original */
        0xb9400049,   /* ldr w9, [x2]: amount */
        0xb940144a,   /* ldr w10, [x2, #0x14] */
        0xb9020909,   /* str w9, [x8, #0x208] */
        0xb9020d0a,   /* str w10, [x8, #0x20c] */
        0xb9421109,   /* ldr w9, [x8, #0x210] */
        0x11000529,   /* add w9, w9, #1 */
        0xb9021109,   /* str w9, [x8, #0x210]: dropped */
        0xd65f03c0,   /* ret */
        0xaa0203e8,   /* original: mov x8, x2 */
        branch(stub+19*4,SITE+4),
    };
    memset(m->base+data+NOCLIP_DAMAGE_DATA,0,24);
    memcpy(m->base+stub,code,sizeof code);
    uint32_t jump=branch(SITE,stub);memcpy(m->base+SITE,&jump,4);
    runtime_noclip_damage_block=(_Atomic uint64_t *)(void *)(m->base+data+NOCLIP_DAMAGE_DATA);
    fprintf(stderr,"[noclip] god mode patch in: no damage to Link while guarded (registerDamage main+%#x, veneer +%#llx)\n",
            SITE,(unsigned long long)stub);
}
#endif
/* Benchmark camera (survey sessions only, 2026-10-20). The look-at camera update stores the frame's eye (doubles +0x60,
   floats +0x38), target (doubles +0x78, floats +0x44) and normalised up (+0x50) into the camera in x20 at
   main+0x6287b4..0x628824, then builds the view matrix from d1,d2,d0 / d3,d4,d5 / s7,s16,s17. The adrp at main+0x628828
   (the point UltraCam's "Camera Controller Universal" signature finds) branches to a stub that, while the block at
   data+0x100 is active, loads the benchmark camera into those registers and the camera's fields; the game's camera
   controller keeps its own state, so the view returns to the player when the block goes inactive. Block: u32 active,
   eye f64[3] +8, target f64[3] +32, up f32[3] +56, then the float copies of eye and target as at +0x38 (+72..+95), the
   vertical FOV in radians +96 (stored into the controller's +0x13c, which main+0x628c3c copies to the projection's
   fovy, as UltraCam's "Camera Controller Hook Fix 1.4.0+" does). UltraCam's time-of-day hook ("TimeHook") matches an
   unrelated function in 1.4.3, so the benchmark sets no time: the save's time runs on as in citron.
   The stub sits after the rewriter's patch area (main+file_size up to the data page, executable like the rest of that
   area and never written by the rewriter): main's text padding is taken by the veneers above, the aspect and Vert+ stubs. */
#define BENCH_CAM_DATA 0x100u
static void patch_bench_cam(Module *m){
#if TARGET_OS_IPHONE
    (void)m;
#else
    const char *survey=getenv("TOTK_SURVEY");if(!survey||strcmp(survey,"1"))return;
    uint64_t data=(m->file_size+0x3fff)&~0x3fffULL,stub=(m->file_size+15)&~15ULL;   /* the patch area's tail */
    static const uint32_t body[]={
        0x91040129, /* add x9, x9, #0x100 */
        0xb940012a, /* ldr w10, [x9]: active? */
        0x3400028a, /* cbz w10, restore (20 instructions on) */
        0x6d408921,0xfd400d20,0x6d421123,0xfd401925,   /* ldp d1,d2 / ldr d0 / ldp d3,d4 / ldr d5: eye and target */
        0x2d474127,0xbd404131,                         /* ldp s7,s16 / ldr s17: up */
        0x6d060a81,0x6d070e80,0x6d081684,              /* stp into the camera's doubles +0x60..+0x88 */
        0x2d0a4287,0xbd005a91,                         /* stp s7,s16,[x20,#0x50] / str s17,[x20,#0x58] */
        0xf940252a,0xf9001e8a,0xf940292a,0xf900228a,0xf9402d2a,0xf900268a,   /* float eye and target +0x38..+0x4f */
        0xb940612a,0xb9013e6a,                         /* ldr w10, [x9, #96] / str w10, [x19, #0x13c]: FOV */
        0xf84107ea}; /* restore: ldr x10, [sp], #16 */
    enum { N = 2 + sizeof body / 4 + 2 };
    uint32_t words[N];
    words[0]=0xf81f0fea;   /* str x10, [sp, #-16]! */
    words[1]=adrp(9,(uintptr_t)m->base+stub+4,(uintptr_t)m->base+data);
    memcpy(words+2,body,sizeof body);
    words[N-2]=adrp(9,(uintptr_t)m->base+stub+(N-2)*4,(uintptr_t)m->base+0x396f000);   /* the instruction it replaces */
    words[N-1]=branch(stub+(N-1)*4,0x62882c);
    uint32_t got;memcpy(&got,m->base+0x628828,4);
    if(got!=0xf0019a29)runtime_fail("1.4.3 benchmark camera: the camera code does not match");
    if(stub<m->patch_off||stub+sizeof words>data||data+BENCH_CAM_DATA+112>m->size)runtime_fail("1.4.3 benchmark camera layout");
    for(unsigned i=0;i<sizeof words;i++)if(m->base[stub+i])runtime_fail("1.4.3 benchmark camera padding not empty");
    memset(m->base+data+BENCH_CAM_DATA,0,112);
    memcpy(m->base+stub,words,sizeof words);
    uint32_t jump=branch(0x628828,stub);memcpy(m->base+0x628828,&jump,4);
    runtime_bench_cam=m->base+data+BENCH_CAM_DATA;
    fprintf(stderr,"[totk] 1.4.3 benchmark camera: look-at and FOV override at main+0x628828 (HUD not hidden yet), stub +%#llx\n",
            (unsigned long long)stub);
#endif
}
#if !TARGET_OS_IPHONE
/* High-frame-rate diagnostics (Mac, TOTK_INPUT_FILE commands). Such bugs sit in game code that compares the game's time
   values with constants tuned for 30 Hz (#61: X+0x28 > 100 above). The time values: the FrameRateMgr singleton
   (main+0x3ada848; +0x10 30 Hz ratio, +0x18 seconds, +0x1c integer FPS) and the time object X = [[main+0x3ad27c8]+0xc8]
   (+0x24 seconds, +0x28 FPS, +0x38 ratio), which 0x3e58a4 updates only when the ratio changes.
   - "frmwatch <seconds> [hex offset]" (FrameRateMgr, default +0x10) / "frmwatchx <seconds> <hex offset>" (X): a hardware
     read watchpoint in every thread, re-armed every 10 ms. A hit (loader.c crash() hands SIGTRAP to
     runtime_diag_watch_trap) records the reading instruction and its return address and disarms that thread, so the
     load runs again; the readers are printed at the end. A frequent reader hides the later ones on its thread until the
     next re-arm, and threshold code often runs only at the moment of contact: watch while the case happens.
   - "xpoke <hex offset> <value>": writes a float into X. With the frame step pinned (framestep) the value stays until
     the ratio changes, which tells which field a behaviour depends on. */
static Module *diag_main;
static uint64_t time_object(void){uint64_t x;memcpy(&x,diag_main->base+0x3ad27c8,8);memcpy(&x,(const void *)(uintptr_t)(x+0xc8),8);return x;}
static uint64_t watch_addr;
enum{WATCH_SLOTS=512};
static struct{_Atomic uint64_t pc;_Atomic uint64_t lr;_Atomic uint32_t n;} watch_hits[WATCH_SLOTS];
static void watch_set(thread_act_t t,int on){
    arm_debug_state64_t d;mach_msg_type_number_t c=ARM_DEBUG_STATE64_COUNT;
    if(thread_get_state(t,ARM_DEBUG_STATE64,(thread_state_t)&d,&c)!=KERN_SUCCESS)return;
    /* WCR: enable, EL0, load, byte address select for the 4 bytes at watch_addr */
    d.__wvr[0]=on?(watch_addr&~7ULL):0;d.__wcr[0]=on?((0xfULL<<((watch_addr&7)+5))|(1u<<3)|(2u<<1)|1u):0;
    thread_set_state(t,ARM_DEBUG_STATE64,(thread_state_t)&d,ARM_DEBUG_STATE64_COUNT);
}
static void watch_all(int on){
    thread_act_array_t list;mach_msg_type_number_t n;thread_act_t self=mach_thread_self();
    if(task_threads(mach_task_self(),&list,&n)!=KERN_SUCCESS)return;
    for(unsigned i=0;i<n;i++){if(list[i]!=self){thread_suspend(list[i]);watch_set(list[i],on);thread_resume(list[i]);}mach_port_deallocate(mach_task_self(),list[i]);}
    vm_deallocate(mach_task_self(),(vm_address_t)list,n*sizeof *list);mach_port_deallocate(mach_task_self(),self);
}
int runtime_diag_watch_trap(void *context){
    ucontext_t *u=context;uint64_t pc=u->uc_mcontext->__ss.__pc,lr=u->uc_mcontext->__ss.__lr;
    unsigned ec=(unsigned)(u->uc_mcontext->__es.__esr>>26);if(!watch_addr||(ec!=0x34&&ec!=0x35))return 0;   /* watchpoint */
    for(unsigned i=(unsigned)(pc>>2)%WATCH_SLOTS,k=0;k<WATCH_SLOTS;k++,i=(i+1)%WATCH_SLOTS){
        uint64_t z=0;if(atomic_compare_exchange_strong(&watch_hits[i].pc,&z,pc)||z==pc){atomic_store(&watch_hits[i].lr,lr);atomic_fetch_add(&watch_hits[i].n,1);break;}
    }
    thread_act_t self=mach_thread_self();watch_set(self,0);mach_port_deallocate(mach_task_self(),self);
    return 1;
}
static void *watch_run(void *arg){
    double secs=((double *)arg)[0];unsigned off=(unsigned)((double *)arg)[1];int in_x=((double *)arg)[2]!=0;free(arg);
    uint64_t obj;if(in_x)obj=time_object();else memcpy(&obj,diag_main->base+0x3ada848,8);
    for(unsigned i=0;i<WATCH_SLOTS;i++){atomic_store(&watch_hits[i].pc,0);atomic_store(&watch_hits[i].n,0);}
    watch_addr=obj+off;
    fprintf(stderr,"[totk] frmwatch: %s %#llx, watching +%#x for %.0f s\n",in_x?"X":"FrameRateMgr",(unsigned long long)obj,off,secs);
    for(double t=0;t<secs;t+=0.01){watch_all(1);usleep(10000);}
    watch_all(0);uint64_t base=(uintptr_t)diag_main->base;unsigned total=0;
    for(unsigned i=0;i<WATCH_SLOTS;i++){uint64_t pc=atomic_load(&watch_hits[i].pc);if(!pc)continue;uint32_t n=atomic_load(&watch_hits[i].n);total+=n;
        fprintf(stderr,"[totk] frmwatch hit main%+lld x%u lr main%+lld\n",(long long)(pc-base),n,(long long)(atomic_load(&watch_hits[i].lr)-base));}
    fprintf(stderr,"[totk] frmwatch done: %u hits\n",total);watch_addr=0;return NULL;
}
void runtime_diag_frm_watch(double secs,unsigned off,int in_x){
    if(watch_addr)runtime_fail("frmwatch: a watch is already running");
    double *a=malloc(3*sizeof *a);a[0]=secs;a[1]=off;a[2]=in_x;pthread_t t;pthread_create(&t,NULL,watch_run,a);pthread_detach(t);
}
void runtime_diag_xpoke(unsigned off,float value){
    uint64_t x=time_object();float old;
    memcpy(&old,(const void *)(uintptr_t)(x+off),4);memcpy((void *)(uintptr_t)(x+off),&value,4);
    fprintf(stderr,"[totk] xpoke X+%#x: %g -> %g\n",off,old,value);
}
#endif
static void patch_output_res(Module *m);
static void patch_render_res(Module *m);
static void patch_fixed_res(Module *m);
void runtime_game_patches(Module *m) {
    runtime_survey_init(m);
    if (strcmp(m->name, "main")) runtime_fail("1.4.3 game patches expect main");
    if (((m->text_end + 15) & ~15ULL) + STUBS_END > ((m->text_end + 0xfff) & ~0xfffULL))
        runtime_fail("1.4.3 patch stubs would overlap read-only data");   /* the layout at the top of this file */
    patch_realtime(m);
    patch_output_res(m);
    patch_render_res(m);
    patch_fixed_res(m);
    patch_slowmo_bounce(m);
#if !TARGET_OS_IPHONE
    patch_preactor_fade(m);
    patch_noclip_damage(m);
    diag_main=m;   /* frmwatch / xpoke */
#endif
    { static const uint64_t sites[] = {0x628c74, 0xb43460, 0x1ff04f4, 0x253fbb0};   /* camera update and setup, viewport x2 */
      /* No Vert+ stubs of upstream's own (vert_off 0): their 2 x 304 bytes do not fit beside the fork's stubs in the
         1024 bytes of padding, and their fovy loads (0x628c3c, 0xb4342c) carry the field of view's stubs below, whose
         multiplier gives Vert+ instead (aspect_patch.h aspect_fov_follow). */
      aspect_install(m, "1.4.3", sites, 4, 2, ((m->text_end + 15) & ~15ULL) + 48,   /* after the two veneers above */
                     0, (m->file_size + 0x3fff) & ~0x3fffULL); }
    /* Field of view (aspect_patch.h): the projection writers' fovy loads, 0x628c3c in the camera update (beside 0x628c74)
       and 0xb4342c in the camera setup (beside 0xb43460); each is followed by fmul s8, s0, s1 (the half angle), the fovy
       store at +0xa0 and the sin / cos / tan calls. Stubs at +288, after the bounce veneer; always in on the Mac and in the iOS
       export, the multiplier switching them on. */
    { static const uint64_t sites[] = {0x628c3c, 0xb4342c};
      static const uint32_t want[] = {0xbd413e60 /* ldr s0, [x19, #0x13c] */, 0xbd4066c0 /* ldr s0, [x22, #0x64] */};
      fov_install(m, "1.4.3", sites, want, 2, ((m->text_end + 15) & ~15ULL) + FOV_STUB_AT, (m->file_size + 0x3fff) & ~0x3fffULL); }
    /* Fog density and shadow resolution (aspect_patch.h): text padding from +512 (fog, one 48-byte stub; the shadow's 40-byte
       stub from +560, to +600), data page +80 (fog) and +84 (shadow). Always in on the Mac and in the iOS export. */
    { static const uint64_t sites[] = {0xd3050};
      static const uint32_t want[] = {0x1e211e49 /* fcsel s9, s18, s1, ne */};
      fog_install(m, "1.4.3", sites, want, 1, ((m->text_end + 15) & ~15ULL) + FOG_STUB_AT, (m->file_size + 0x3fff) & ~0x3fffULL); }
    shadow_install(m, "1.4.3", ((m->text_end + 15) & ~15ULL) + SHADOW_STUB_AT, (m->file_size + 0x3fff) & ~0x3fffULL);
    /* Time of day speed and render distance (world_patch.h): three 32-byte stubs after the render-resolution veneers
       (+416..+512), factors at the data page +72 / +76. Always in on the Mac and in the iOS export. */
    world_patches_install(m, "1.4.3", ((m->text_end + 15) & ~15ULL) + WORLD_STUBS, (m->file_size + 0x3fff) & ~0x3fffULL);
    patch_bench_cam(m);
    const char *enabled = getenv("TOTK_FPS60");
    if (enabled && !strcmp(enabled, "0")) {
        fprintf(stderr, "[totk] 1.4.3 shipping frame controller (TOTK_FPS60=0)\n");
        return;
    }
    for (unsigned i = 0; i < sizeof fps60 / sizeof *fps60; i++) {
#if !TARGET_OS_IPHONE
        uint32_t word;
        if (fps60[i].off + 4 > m->text_end) runtime_fail("1.4.3 FPS patch outside text");
        memcpy(&word, m->base + fps60[i].off, 4);
        if (word != fps60[i].original) runtime_fail("1.4.3 FPS patch original instruction mismatch");
#endif
    }
 #if TARGET_OS_IPHONE
    for (unsigned i = 0; i < sizeof fps60 / sizeof *fps60; i++) {
        uint32_t word; memcpy(&word,m->base+fps60[i].off,4);
        if(word!=fps60[i].replacement)runtime_fail("1.4.3 embedded FPS patch mismatch");
    }
 #else
    for (unsigned i = 0; i < sizeof fps60 / sizeof *fps60; i++)
        memcpy(m->base + fps60[i].off, &fps60[i].replacement, 4);
 #endif
    fprintf(stderr, "[totk] 1.4.3 60 FPS: seven verified controller instructions; original game frame-time calculation\n");
}

/* New display configuration object: +0x10 is display 0's canvas size, +0x508
 * its active crop, and +0x528 the floating display scale. The allocation path
 * multiplies the canvas by that scale (8d860..8d894). Publishing only +0x508
 * changes the final crop but leaves every UI/composition target at 1080p.
 * Pending output changes are bit 21 of +8. Keep the original rebuild path. */
static int output_full_at_start;
static unsigned output_w,output_h;
static _Atomic(uint8_t *) output_page;
static uint64_t pack_size(unsigned w,unsigned h){return (uint64_t)h<<32|w;}
uint64_t runtime_output_size_current(void){
    uint8_t *page=atomic_load(&output_page);
    return page?atomic_load((_Atomic uint64_t *)(void *)(page+32)):pack_size(1920,1080);
}
int runtime_output_res(unsigned *w,unsigned *h){
    static int decided,on;static unsigned cw,ch;
    if(!decided){
        const char *e=getenv("TOTK_OUTPUT_RES");
        /* iOS shows 720p only (the user, 2026-10-18): no larger swapchain, heaps or output there, whatever
           TOTK_OUTPUT_RES says. The iOS export runs these patches on the Mac to make the phone's embedded code
           (TOTK_EXPORT_IOS), so it follows the same rule: before, auto put the 2560x1440 swapchain into the phone's code,
           and the phone, which carries the heap overlay in its bundle, reserved 88 MiB more for a picture it never shows. */
        if(TARGET_OS_IPHONE||getenv("TOTK_EXPORT_IOS")){
            on=0;
            if(e&&*e&&strcmp(e,"0")&&strcmp(e,"auto"))fprintf(stderr,"[totk] TOTK_OUTPUT_RES=%s ignored: iOS shows 720p only\n",e);
        }else if(e&&!strcmp(e,"0"))on=0;
        else if(!e||!*e||!strcmp(e,"auto")){
            const char *dir=getenv("TOTK_ROMFS_PATCHES_OUT");char path[4096];
            snprintf(path,sizeof path,"%s/2560x1440/manifest.tsv",dir?dir:"extracted/v143/romfs-patches-out");
            FILE *f=fopen(path,"r");if(f){fclose(f);on=1;cw=2560;ch=1440;}
            /* Headroom (settings): a 4K output on a Mac with 16 GB or more when its overlay was built (build_app.py native 4K,
               on by default there), with a 2560x1440 scene reserve; a larger scene (Maximum render resolution, its own
               <canvas>-scene-<scene> overlay) needs a texture heap above 1 GiB, which one zero-copy GPU buffer now takes
               (nvn_mtl.m pool_range_check: no size limit since upstream b3bb2c08). */
            uint64_t mem=0;size_t len=sizeof mem;sysctlbyname("hw.memsize",&mem,&len,NULL,0);
            snprintf(path,sizeof path,"%s/3840x2160-scene-2560x1440/manifest.tsv",dir?dir:"extracted/v143/romfs-patches-out");
            if(mem>=(16ULL<<30)&&(f=fopen(path,"r"))){fclose(f);on=1;cw=3840;ch=2160;}
            output_full_at_start=0;
        }else{
            char tail;
            if(sscanf(e,"%ux%u%c",&cw,&ch,&tail)!=2||cw<640||ch<360||cw>7680||ch>4320)runtime_fail("TOTK_OUTPUT_RES must be auto, 0 or widthxheight");
            on=1;output_full_at_start=1;
        }
        decided=1;
    }
    if(on){*w=cw;*h=ch;}return on;
}
/* The settings menu's output size (settings.c display_apply): any 16:9 size up to the swapchain, live through the game's own
   display-config rebuild (as key 3). Asked before the patch installs, it is the size the game starts at. */
static _Atomic uint64_t output_pending,output_want,output_want_ns;
void runtime_output_max(unsigned *w,unsigned *h){if(!runtime_output_res(w,h)){*w=1920;*h=1080;}}
int runtime_output_request(unsigned w,unsigned h){
    unsigned mw,mh;if(!runtime_output_res(&mw,&mh))return w==1920&&h==1080?0:-1;
    if(w<640||h<360||w>mw||h>mh)return -1;
    uint8_t *page=atomic_load(&output_page);
    if(!page){atomic_store(&output_pending,pack_size(w,h));return 0;}
    /* Each size rebuilds the game's canvas targets, and old and new are both held for a moment: a size clicked through
       on the way to another (several in a row ran past the GPU memory budget, 2026-10-20) is never applied. The present
       thread takes the asked size once it has stayed the same for 0.4 s (output_latch). */
    atomic_store(&output_want,pack_size(w,h));atomic_store(&output_want_ns,0);
    return 0;
}
static void output_latch(uint64_t now_ns){
    uint8_t *page=atomic_load(&output_page);uint64_t want=atomic_load(&output_want);
    if(!page||!want||!now_ns)return;
    _Atomic uint64_t *size=(_Atomic uint64_t *)(void *)(page+32);
    if(atomic_load(size)==want){atomic_store(&output_want,0);return;}
    uint64_t since=atomic_load(&output_want_ns);
    if(!since){atomic_store(&output_want_ns,now_ns);return;}
    if(now_ns-since<400000000ULL)return;
    if(!atomic_compare_exchange_strong(&output_want,&want,0))return;   /* asked again meanwhile: wait for that one */
    atomic_store(size,want);
    uint64_t config=atomic_load((_Atomic uint64_t *)(void *)(page+40));
    if(config){__atomic_fetch_or((uint32_t *)(uintptr_t)(config+8),1u<<21,__ATOMIC_SEQ_CST);runtime_oe_push_message(30);}
    fprintf(stderr,"[totk] 1.4.3 output %ux%u (settings; display config %s)\n",(unsigned)want,(unsigned)(want>>32),config?"ready":"not published yet");
}
/* What the HeapDef overlay adds to DynamicTextureAllocatorMem and Graphics (and runtime/services.c to the application
 * pool): the canvas and the scene each grow the heap by their pixel ratio above the shipped 1920x1080 / 1600x900, the
 * scene reserve with a 1.6 margin (Awis13 f9808deb1c, measured 2026-10-19: a 3440x1440 canvas with a 2580x1080 scene
 * crashed in the allocator at +258 MiB and loaded at +400 MiB). scripts/make_output_res_overlay.py uses the same formula. */
static int render_reserve_heap(unsigned *w,unsigned *h);
/* Upstream's name for "a scene above 1600x900 that the heaps count" (romfs.c, game_patches.c / game_patches_botw.c give
   0): here the boot reserve when it needs the HeapDef overlay. */
int runtime_render_res(unsigned *w,unsigned *h){return render_reserve_heap(w,h);}
uint64_t runtime_output_res_extra_bytes(void){
    unsigned w,h,sw,sh;double grow=0;
    if(runtime_output_res(&w,&h)&&(double)w*h>1920.0*1080.0)grow+=(double)w*h/(1920.0*1080.0)-1;
    int scene=render_reserve_heap(&sw,&sh);if(scene)grow=(grow+(double)sw*sh/(1600.0*900.0)-1)*1.6;
    if(grow<=0)return 0;
    uint64_t kib=(uint64_t)ceil(113664.0*(1+grow)/1024.0)*1024-113664;return kib<<10;
}
/* Key 3's picture mode is kept between runs in the file TOTK_PICTURE_MODE names (the launcher: <data>/picture-mode.txt;
   unset: nothing is kept, as before). It holds one word: off, metalfx, native-2k, native-2k-metalfx, native-4k or 720p. The next start with
   automatic output sizes begins in that mode; a size larger than this app's swapchain falls back to the largest one it
   has (native-4k on a 2K app starts at native 2K). An explicit TOTK_OUTPUT_RES=<w>x<h> starts as it says. */
static void picture_mode_save(uint64_t size){
    const char *path=getenv("TOTK_PICTURE_MODE");if(!path||!*path)return;
    extern int mfxup_output_enabled(void);
    const char *mode=mfxup_output_enabled()?(size==pack_size(2560,1440)?"native-2k-metalfx":"metalfx"):size==pack_size(2560,1440)?"native-2k":
                     size==pack_size(3840,2160)?"native-4k":size==pack_size(1280,720)?"720p":"off";
    FILE *f=fopen(path,"w");
    if(!f||fprintf(f,"%s\n",mode)<0||fclose(f)){fprintf(stderr,"[totk] picture mode: cannot write %s\n",path);return;}
    fprintf(stderr,"[totk] picture mode %s kept for the next start\n",mode);
}
static void picture_mode_restore(uint8_t *page){
    const char *path=getenv("TOTK_PICTURE_MODE");if(!path||!*path||output_full_at_start)return;
    char mode[32]="";FILE *f=fopen(path,"r");if(!f)return;
    if(!fgets(mode,sizeof mode,f))mode[0]=0;
    fclose(f);mode[strcspn(mode,"\r\n")]=0;
    unsigned w=1920,h=1080;
    if(!strcmp(mode,"native-4k")&&output_w>=3840&&output_h>=2160){w=3840;h=2160;}
    else if((!strcmp(mode,"native-4k")||!strcmp(mode,"native-2k"))&&output_w>=2560&&output_h>=1440){w=2560;h=1440;}
    else if(!strcmp(mode,"native-2k-metalfx")&&output_w>=2560&&output_h>=1440){extern void dm_gfx_set_flag(const char *,int);
        w=2560;h=1440;dm_gfx_set_flag("mfx_output",1);dm_gfx_set_flag("mfx_upscale",1);}
    else if(!strcmp(mode,"metalfx")){extern void dm_gfx_set_flag(const char *,int);dm_gfx_set_flag("mfx_output",1);dm_gfx_set_flag("mfx_upscale",1);}
    else if(!strcmp(mode,"720p")){w=1280;h=720;}
    else if(strcmp(mode,"off")){fprintf(stderr,"[totk] picture mode: %s holds \"%s\", not off, metalfx, native-2k, native-2k-metalfx, native-4k or 720p; starting off\n",path,mode);return;}
    atomic_store((_Atomic uint64_t *)(void *)(page+32),pack_size(w,h));
    fprintf(stderr,"[totk] picture mode: starts in %s (kept from the last run, %s)\n",mode,path);
}
static void patch_output_res(Module *m){
    unsigned w,h;if(!runtime_output_res(&w,&h))return;
    uint64_t data=(m->file_size+0x3fff)&~0x3fffULL,stub=((m->text_end+15)&~15ULL)+16;
    if(data+0x4000!=m->size||stub+28>((m->text_end+0x3fff)&~0x3fffULL))runtime_fail("1.4.3 output patch layout");
    const Patch guards[]={
        {0xbdb578,0xd280f008,0},{0xbdb584,0xf2c08708,0},
        {0x3dcb28,0xd360fc64,0},{0x3dcb2c,0xf9028663,0},{0x3dcb30,0xd001ac08,0}};
    for(unsigned i=0;i<sizeof guards/sizeof *guards;i++){
#if !TARGET_OS_IPHONE
        uint32_t word;memcpy(&word,m->base+guards[i].off,4);
        if(word!=guards[i].original)runtime_fail("1.4.3 output original instruction mismatch");
#endif
    }
#if !TARGET_OS_IPHONE
    for(unsigned i=0;i<28;i++)if(m->base[stub+i])runtime_fail("1.4.3 output veneer padding not empty");
#endif
    /* x9 is dead at this publication point; the next original use initializes
     * it from the active display selector. x19 remains the display config. */
    uint32_t veneer[]={adrp(9,(uintptr_t)m->base+stub,(uintptr_t)m->base+data),
        0xf9001533, /* str x19,[x9,#40]: live display config */
        0xf9401123, /* ldr x3,[x9,#32]: requested output */
        0xf9000a63, /* str x3,[x19,#0x10]: display 0 canvas */
        0xd360fc64, /* lsr x4,x3,#32: crop height */
        0xf9028663, /* str x3,[x19,#0x508]: original active output write */
        branch(stub+24,0x3dcb30)};
 #if TARGET_OS_IPHONE
    if(memcmp(m->base+stub,veneer,sizeof veneer))runtime_fail("1.4.3 embedded output veneer mismatch");
 #else
    memcpy(m->base+stub,veneer,sizeof veneer);
 #endif
    uint32_t words[]={0xd2800008|(w<<5),0xf2c00008|(h<<5),branch(0x3dcb28,stub)};
 #if TARGET_OS_IPHONE
    for(unsigned i=0;i<3;i++){uint32_t got;memcpy(&got,m->base+guards[i].off,4);if(got!=words[i])runtime_fail("1.4.3 embedded output patch mismatch");}
 #else
    for(unsigned i=0;i<3;i++)memcpy(m->base+guards[i].off,words+i,4);
 #endif
    uint8_t *page=m->base+data;output_w=w;output_h=h;
    uint64_t pending=atomic_load(&output_pending);
    atomic_store((_Atomic uint64_t *)(void *)(page+32),pending?pending:output_full_at_start?pack_size(w,h):pack_size(1920,1080));
    atomic_store((_Atomic uint64_t *)(void *)(page+40),0);
    if(!pending)picture_mode_restore(page);   /* an output size the settings panel asked for wins over the kept picture mode */
    atomic_store(&output_page,page);   /* Publish initialized state to the HUD on the main thread. */
    uint64_t start=atomic_load((_Atomic uint64_t *)(void *)(page+32));
    fprintf(stderr,"[totk] 1.4.3 output: %ux%u swapchain; starts at %ux%u, key 3 cycles MetalFX / native / off; display-config veneer +%#llx\n",w,h,(unsigned)start,(unsigned)(start>>32),(unsigned long long)stub);
}
/* Scene render resolution - the 1.4.3 port of game_patches.c's TOTK_RENDER_RES patch, live since the settings menu.
   1.4.3 keeps the same graphics-parameter entry (+0x30 of the mode entry: handheld 1280x720, docked 1600x900) and the
   same renderer fields (+0xcc4 width, +0xcc8 height), but converts both through s0 and keeps the output size in w9:
     main+0x585e48  scvtf s0, w11             -> b veneer: adrp x3, <data page>; str x19, [x3, #24]; b main+0x585e4c
     main+0x585e50  str   s0, [x19, #0xcc4]   -> ldp  s0, s1, [x3, #16]
     main+0x585e54  scvtf s0, w10             -> str  s0, [x19, #0xcc4]
     main+0x585e60  str   s0, [x19, #0xcc8]   -> str  s1, [x19, #0xcc8]
   Every path that picks the render size converges through main+0x585e48 (the branch at 0x585f50 only reloads w11/w10
   and jumps back), so the four sites cover all of them. x3 is scratch and untouched by this function; x9 must NOT be
   used here because w9 still holds the output size for main+0x585e64. The size comes from the loader's data page +16
   (the page patch_realtime reserves), so it can change while the game runs.
   The four words are written always (Mac) and embedded always (iOS export, TOTK_EXPORT_IOS: the phone cannot write
   code), with the game's own size on the page until something asks for another: handheld 1280x720 (the phone, and
   TOTK_OPERATION_MODE=handheld), docked 1600x900. TOTK_RENDER_RES=<w>x<h> asks at boot; runtime_render_request asks
   live and the present thread latches it (render_latch, from runtime_frame_step_update; after
   ly05010419/MacNativeGames#42). The game reads the size only when it picks one (main+0x585da0), which it does when
   its mode index changes: rewriting the page alone changes nothing mid-game, and neither does nn::oe message 30 with
   the display config's pending bit, nor a mode index no mode has in the renderer's cached one (+0xcd0) (all tested
   2026-10-19, the scene stayed 1600x896). The per-frame graphics update (main+0x5808f0..) takes its mode-change path,
   main+0x580b28 -> main+0x585bd0(force 1), when the display state's flags at +8 have 0x280; a store there from the
   host is overwritten before it is read. So a second veneer at main+0x580920 ORs the data page +48 into the flags it
   loads (ldrh w8,[x23,#8]; x10 is dead there, every path calls before reading it). The latch writes the size, sets +48
   to 0x80 and the cached index to -1, and clears +48 once the cached index shows the pick ran (normally the next
   frame; given up after 600 frames). The pick reallocates the scene targets the game's own way, as on a mode change.
   The first veneer also keeps the renderer (x19) at +24 for that. Code from an older export without the patch keeps
   the game's own size and refuses live requests. TOTK_RENDER_RES=0 leaves main alone (Mac; live requests refused).
   The game scales its scene with the canvas (Awis13/MacNativeGames b899aa841c): it renders the patched size times the
   canvas width over 1920 on both axes (measured there: 2560x1440 asked on a 2560x1440 canvas rendered 3408x1920). An
   asked size is therefore written divided by that factor, and written again when the canvas changes (key 3); the
   game's own size is written as it is, so the game scales it exactly as it does unpatched.
   The scene's render targets come from DynamicTextureAllocatorMem, sized once at boot from the HeapDef (113664 KiB:
   a 1080p canvas plus the 1600x900 scene). The boot reserve (runtime_render_reserve) is the biggest scene the heaps
   hold: above 1600x900 it needs the HeapDef overlay <canvas>-scene-<w>x<h> (scripts/make_output_res_overlay.py
   --scene; runtime/romfs.c loads it) and grows the application pool by the same amount (runtime_output_res_extra_bytes,
   Awis13 f9808deb1c's formula). Live requests above the reserve are refused (-1: applies after a restart). */
static uint8_t *render_page;                 /* the data page once the four sites carry the patch */
static unsigned render_game_w,render_game_h; /* the game's own size for this mode */
static _Atomic uint64_t render_request;      /* pack_size of the asked size; 0 = the game's own */
static uint64_t render_latched;static unsigned render_canvas;   /* present thread only */
#define RENDER_STOCK_PIXELS (1600u*900u)     /* the scene the shipped heaps hold next to a 1080p canvas */
static int render_size_parse(const char *e,unsigned *w,unsigned *h){
    char rest;return sscanf(e,"%ux%u%c",w,h,&rest)==2&&*w>=64&&*h>=64&&*w<=4096&&*h<=4096;
}
/* Upstream (e8edbcd) turned this patch on by default (the Mac scene at the canvas size, the phone at 960x540). This fork
   keeps the game's own size until something asks for another (TOTK_RENDER_RES, or the settings panel's resolution model
   and graphics.dynamic_res, live through runtime_render_request). TOTK_RENDER_RES=<w>x<h> sets a size at boot. */
static int render_res(unsigned *w,unsigned *h){
    const char *e=getenv("TOTK_RENDER_RES");
    if(!e||!*e||!strcmp(e,"0"))return 0;
    if(!render_size_parse(e,w,h))runtime_fail("TOTK_RENDER_RES must be <width>x<height>");
    return 1;
}
/* The HeapDef overlay a scene reserve above the stock heaps needs: <canvas>-scene-<w>x<h> in TOTK_ROMFS_PATCHES_OUT. */
static void render_overlay_name(char *out,size_t n,unsigned sw,unsigned sh){
    unsigned w=1920,h=1080;runtime_output_res(&w,&h);snprintf(out,n,"%ux%u-scene-%ux%u",w,h,sw,sh);
}
static int render_overlay_exists(unsigned sw,unsigned sh){
    if((uint64_t)sw*sh<=RENDER_STOCK_PIXELS)return 1;
    char name[64],path[4096];render_overlay_name(name,sizeof name,sw,sh);const char *dir=getenv("TOTK_ROMFS_PATCHES_OUT");
    snprintf(path,sizeof path,"%s/%s/manifest.tsv",dir?dir:"extracted/v143/romfs-patches-out",name);
    FILE *f=fopen(path,"r");if(f)fclose(f);return f!=NULL;
}
void runtime_render_reserve(unsigned *w,unsigned *h){
    static int decided;static unsigned rw=1600,rh=900;
    if(!decided){
        const char *e=getenv("TOTK_RENDER_RESERVE");unsigned bw,bh;int boot=render_res(&bw,&bh);
        if(getenv("TOTK_EXPORT_IOS"))boot=0;   /* the export builds code only; the phone decides at its own launch */
        else if(e&&*e){
            if(!render_size_parse(e,&rw,&rh))runtime_fail("TOTK_RENDER_RESERVE must be <width>x<height>");
            if(!render_overlay_exists(rw,rh)){   /* e.g. the Maximum render resolution setting without the 4K maximum output:
                                                   the automatic choice below, never a failed start */
                char name[64];render_overlay_name(name,sizeof name,rw,rh);
                fprintf(stderr,"[totk] TOTK_RENDER_RESERVE=%s needs the HeapDef overlay %s (built for the 4K maximum output by build_app.py, or scripts/make_output_res_overlay.py <canvas> --scene %ux%u --version v143): the automatic reserve instead\n",e,name,rw,rh);
                rw=1600;rh=900;e=NULL;
            }
        }
        if(!getenv("TOTK_EXPORT_IOS")&&(!e||!*e)){
            /* By what the app may use at launch (iOS: its limit is far below the RAM) or the Mac's memory: the biggest
               candidate at or below that whose overlay was built (1600x900 and below need none). */
            static const unsigned cand[][2]={{2560,1440},{1920,1080},{1600,900},{1280,720}};
#if TARGET_OS_IPHONE
            uint64_t avail=os_proc_available_memory(),gib=1ULL<<30;
            unsigned first=avail>=10*gib?1:avail>=5*gib?2:3;
            fprintf(stderr,"[totk] render reserve: %.2f GiB available at launch\n",avail/(double)gib);
#else
            uint64_t mem=0;size_t len=sizeof mem;sysctlbyname("hw.memsize",&mem,&len,NULL,0);
            unsigned first=mem>=(16ULL<<30)?0:1;
#endif
            for(unsigned i=first;i<4;i++)if(render_overlay_exists(cand[i][0],cand[i][1])){rw=cand[i][0];rh=cand[i][1];break;}
        }
        if(boot&&(uint64_t)bw*bh>(uint64_t)rw*rh){
            if(render_overlay_exists(bw,bh)){rw=bw;rh=bh;}
            else fprintf(stderr,"[totk] TOTK_RENDER_RES=%ux%u is above the %ux%u reserve and has no HeapDef overlay: heaps sized for %ux%u\n",bw,bh,rw,rh,rw,rh);
        }
        decided=1;
        fprintf(stderr,"[totk] render reserve %ux%u: live scene sizes up to %u pixels (%s)\n",rw,rh,rw*rh,
                (uint64_t)rw*rh>RENDER_STOCK_PIXELS?"HeapDef overlay grows the texture heap":"the game's own heaps");
    }
    *w=rw;*h=rh;
}
static int render_reserve_heap(unsigned *w,unsigned *h){runtime_render_reserve(w,h);return (uint64_t)*w**h>RENDER_STOCK_PIXELS;}
int runtime_heap_overlay(char *out,size_t n){
    unsigned w=1920,h=1080,sw,sh;if(!runtime_output_res_extra_bytes())return 0;
    if(render_reserve_heap(&sw,&sh))render_overlay_name(out,n,sw,sh);else{runtime_output_res(&w,&h);snprintf(out,n,"%ux%u",w,h);}
    return 1;
}
static void render_write(unsigned canvas){
    uint64_t r=atomic_load(&render_request);float v[2];
    if(!r){v[0]=(float)render_game_w;v[1]=(float)render_game_h;}
    else{float k=canvas>1920?canvas/1920.0f:1.0f;v[0]=(float)(uint32_t)r/k;v[1]=(float)(uint32_t)(r>>32)/k;}
    uint64_t bits;memcpy(&bits,v,8);atomic_store((_Atomic uint64_t *)(void *)(render_page+16),bits);
    render_latched=r;render_canvas=canvas;
    fprintf(stderr,"[totk] 1.4.3 render resolution: scene %ux%u on a %u-wide canvas (data page %.1fx%.1f)%s\n",
            r?(unsigned)r:render_game_w,r?(unsigned)(r>>32):render_game_h,canvas,v[0],v[1],r?"":", the game's own");
}
static uint64_t render_forced;static unsigned render_waited;   /* the renderer whose pick was asked for, until it has picked (logged) */
static void render_latch(void){
    if(!render_page)return;
    if(render_forced&&atomic_load((_Atomic int8_t *)(uintptr_t)(render_forced+0xcd0))!=-1){
        float v[2];memcpy(v,(void *)(uintptr_t)(render_forced+0xcc4),8);render_forced=0;
        fprintf(stderr,"[totk] 1.4.3 render resolution: the game picked again, renderer size %.0fx%.0f\n",v[0],v[1]);
        atomic_store((_Atomic uint32_t *)(void *)(render_page+48),0);
    }else if(render_forced&&++render_waited==600){   /* ten seconds without a pick (paused, loading): stop asking every frame */
        atomic_store((_Atomic uint32_t *)(void *)(render_page+48),0);render_forced=0;
        fprintf(stderr,"[totk] 1.4.3 render resolution: no pick after 600 frames; the size applies at the game's next own pick\n");
    }
    uint64_t r=atomic_load_explicit(&render_request,memory_order_acquire);unsigned canvas=(unsigned)runtime_output_size_current();
    if(r==render_latched&&canvas==render_canvas)return;
    render_write(canvas);
    /* One forced pick through the per-frame update's mode-change path (the second veneer); the cached index set to -1
       tells when it ran (the pick stores the real one). */
    uint64_t renderer=atomic_load((_Atomic uint64_t *)(void *)(render_page+24));
    if(!renderer){fprintf(stderr,"[totk] 1.4.3 render resolution: the game has not picked a size yet; it takes the new one at its first\n");return;}
    atomic_store((_Atomic int8_t *)(uintptr_t)(renderer+0xcd0),(int8_t)-1);render_forced=renderer;render_waited=0;
    atomic_store((_Atomic uint32_t *)(void *)(render_page+48),0x80);   /* the per-frame update takes its mode-change path */
}
int runtime_render_request(unsigned w,unsigned h){
    if(!w&&!h){if(atomic_exchange(&render_request,0))fprintf(stderr,"[totk] render request: the game's own size\n");return 0;}
    unsigned rw,rh;runtime_render_reserve(&rw,&rh);
    int range=w>=64&&h>=64&&w<=4096&&h<=4096;
    if(!range||(uint64_t)w*h>(uint64_t)rw*rh){
        fprintf(stderr,"[totk] render request %ux%u refused: %s\n",w,h,!range?"out of range":"above the boot reserve, applies after a restart");
        return -1;
    }
    if(atomic_load(&render_request)==pack_size(w,h))return 0;
    atomic_store_explicit(&render_request,pack_size(w,h),memory_order_release);
    fprintf(stderr,"[totk] render request %ux%u (reserve %ux%u): latched at the next present\n",w,h,rw,rh);return 0;
}
static void patch_render_res(Module *m){
    int handheld=runtime_handheld_mode();render_game_w=handheld?1280:1600;render_game_h=handheld?720:900;
    unsigned w,h;int set=render_res(&w,&h);
#if !TARGET_OS_IPHONE
    const char *e=getenv("TOTK_RENDER_RES");
    if(e&&!strcmp(e,"0")){fprintf(stderr,"[totk] 1.4.3 render resolution: the game's own, not patched (TOTK_RENDER_RES=0)\n");return;}
#endif
    uint64_t data=(m->file_size+0x3fff)&~0x3fffULL,stub=((m->text_end+15)&~15ULL)+RENDER_VENEER;
    if(data+0x4000!=m->size||stub+32>((m->text_end+0x3fff)&~0x3fffULL))runtime_fail("1.4.3 render-resolution patch layout");
    const Patch sites[]={
        {0x585e48,0x1e220160,0},
        {0x585e50,0xbd0cc660,0x2d420460},
        {0x585e54,0x1e220140,0xbd0cc660},
        {0x585e60,0xbd0cca60,0xbd0cca61},
    };
    /* The veneer also keeps the renderer (x19) at data page +24, for render_latch to ask for a new pick. */
    uint32_t veneer[]={adrp(3,(uintptr_t)m->base+stub,(uintptr_t)m->base+data),
        0xf9000c73, /* str x19,[x3,#24] */
        branch(stub+8,0x585e4c)};   /* stub+12: veneer2 below */
    uint32_t want[4]={branch(sites[0].off,stub),sites[1].replacement,sites[2].replacement,sites[3].replacement};
    /* The per-frame graphics update takes its mode-change path (main+0x580b28: re-pick, forced) when the display state's
       flags at +8 have 0x280; the second veneer ORs the data page +48 into what it loads (x10 is dead there: every path
       calls before reading it), so render_latch can ask for one pick. */
    const Patch kick={0x580920,0x794012e8,0};
    uint32_t veneer2[]={kick.original,adrp(10,(uintptr_t)m->base+stub+16,(uintptr_t)m->base+data),
        0xb940314a, /* ldr w10,[x10,#48] */
        0x2a0a0108, /* orr w8,w8,w10 */
        branch(stub+28,0x580924)},kick_word=branch(kick.off,stub+12);
#if TARGET_OS_IPHONE
    uint32_t first;memcpy(&first,m->base+sites[0].off,4);
    if(first==sites[0].original){fprintf(stderr,"[totk] 1.4.3 render resolution: the game's own (this export did not embed the patch; no live sizes)\n");return;}
    if(memcmp(m->base+stub,veneer,sizeof veneer)||memcmp(m->base+stub+12,veneer2,sizeof veneer2)||memcmp(m->base+kick.off,&kick_word,4))
        runtime_fail("1.4.3 embedded render-resolution veneer mismatch");
    for(unsigned i=0;i<sizeof sites/sizeof *sites;i++){
        uint32_t got;memcpy(&got,m->base+sites[i].off,4);
        if(got!=want[i])runtime_fail("1.4.3 embedded render-resolution patch mismatch");
    }
#else
    for(unsigned i=0;i<sizeof sites/sizeof *sites;i++){
        uint32_t word;memcpy(&word,m->base+sites[i].off,4);
        if(word!=sites[i].original){
            fprintf(stderr,"[totk] render resolution patch main+%#llx: found %08x, expected %08x\n",
                    (unsigned long long)sites[i].off,word,sites[i].original);
            runtime_fail("render resolution patch: main does not match 1.4.3");
        }
    }
    {uint32_t word;memcpy(&word,m->base+kick.off,4);if(word!=kick.original)runtime_fail("render resolution patch: main does not match 1.4.3");}
    for(unsigned i=0;i<sizeof veneer+sizeof veneer2;i++)if(m->base[stub+i])runtime_fail("1.4.3 render-resolution veneer padding not empty");
    memcpy(m->base+stub,veneer,sizeof veneer);memcpy(m->base+stub+12,veneer2,sizeof veneer2);memcpy(m->base+kick.off,&kick_word,4);
    for(unsigned i=0;i<sizeof sites/sizeof *sites;i++)memcpy(m->base+sites[i].off,&want[i],4);
#endif
    atomic_store((_Atomic uint64_t *)(void *)(m->base+data+24),0);atomic_store((_Atomic uint32_t *)(void *)(m->base+data+48),0);
    if(set)atomic_store(&render_request,pack_size(w,h));
    render_page=m->base+data;render_write((unsigned)runtime_output_size_current());
    fprintf(stderr,"[totk] 1.4.3 render resolution patch: 4 instructions at main+0x585e48, veneer +%#llx, size on the data page (live: runtime_render_request)\n",(unsigned long long)stub);
}
/* TOTK_FIXED_RES=1: no dynamic resolution. The game's controller (main+0x584300, called from the renderer's per-frame update at
   main+0x58196c) keeps a window of 24 frame-time samples, predicts the next frame's cost and steps through resolution tiers
   (the scene came out at 3024x1888, 2712x1696, 2416x1512 on a 3024x1890 canvas: 100 %, 90 %, 80 %). It returns the tier's scale
   as a float in the low word of x0 (bit 33 set: the tier did not change); the caller stores cc4 * scale / cc0 as the renderer's
   scale at +0xccc. Every path leaves through one epilogue, so the single instruction that moves the scale into the result
   (main+0x5848a8, mov w9, w23) is replaced by a move of 1.0f: the controller still runs and picks tiers, but every tier is 100 %.
   The size the render-resolution patch above chooses (TOTK_RENDER_RES) stays; this only stops the dynamic part. */
static void patch_fixed_res(Module *m){
    const char *e=getenv("TOTK_FIXED_RES");
    if(!e||!*e||!strcmp(e,"0"))return;
#if TARGET_OS_IPHONE
    fprintf(stderr,"[totk] TOTK_FIXED_RES: not available in the embedded build\n");(void)m;
#else
    const uint64_t site=0x5848a8;const uint32_t original=0x2a1703e9,replacement=0x52a7f009;   /* mov w9, w23 -> mov w9, #0x3f800000 */
    uint32_t word;memcpy(&word,m->base+site,4);
    if(word!=original){
        fprintf(stderr,"[totk] fixed resolution patch main+%#llx: found %08x, expected %08x\n",(unsigned long long)site,word,original);
        runtime_fail("fixed resolution patch: main does not match 1.4.3");
    }
    memcpy(m->base+site,&replacement,4);
    fprintf(stderr,"[totk] 1.4.3 fixed resolution (TOTK_FIXED_RES): the dynamic resolution controller's scale is held at 100 %% (1 instruction at main+%#llx)\n",(unsigned long long)site);
#endif
}
int runtime_output_res_toggle(void){
    if(!output_page)return 0;
    _Atomic uint64_t *size=(_Atomic uint64_t *)(void *)(output_page+32);
    int full=atomic_load(size)!=pack_size(output_w,output_h);
    atomic_store(size,full?pack_size(output_w,output_h):pack_size(1920,1080));
    uint64_t now=atomic_load(size);   /* the scene size follows at the next present (render_latch sees the new canvas) */
    uint64_t config=atomic_load((_Atomic uint64_t *)(void *)(output_page+40));
    if(config){__atomic_fetch_or((uint32_t *)(uintptr_t)(config+8),1u<<21,__ATOMIC_SEQ_CST);runtime_oe_push_message(30);}
    fprintf(stderr,"[totk] 1.4.3 output requested %ux%u (display config %s)\n",(unsigned)now,(unsigned)(now>>32),config?"ready":"not published yet");
    return 1;
}
/* Upstream's resolution setters (runtime.h OUTPUT_MODE_*; its key-1 panel uses them, ours can): any of 720p, 1080p, 1080p
   with the MetalFX output scaler, the native canvas (the swapchain's size), the same with MetalFX up to the window.
   The size goes through the same display-config rebuild as key 3; the scene follows at the next present (render_latch). */
static void output_size_set(uint64_t want){
    _Atomic uint64_t *size=(_Atomic uint64_t *)(void *)(output_page+32);
    if(atomic_load(size)==want)return;
    atomic_store(size,want);
    uint64_t config=atomic_load((_Atomic uint64_t *)(void *)(output_page+40));
    if(config){__atomic_fetch_or((uint32_t *)(uintptr_t)(config+8),1u<<21,__ATOMIC_SEQ_CST);runtime_oe_push_message(30);}
    fprintf(stderr,"[totk] 1.4.3 output requested %ux%u (display config %s)\n",(unsigned)want,(unsigned)(want>>32),config?"ready":"not published yet");
}
int runtime_output_native(int mode,unsigned *w,unsigned *h){
    if(!output_page)return 0;
    if(mode==OUTPUT_MODE_NATIVE_4K){if(output_w<3840||output_h<2160)return 0;*w=3840;*h=2160;return 1;}   /* a native 4K app */
    if(mode!=OUTPUT_MODE_NATIVE&&mode!=OUTPUT_MODE_NATIVE_METALFX)return 0;
    if(output_w>2560){*w=2560;*h=1440;return 1;}   /* a 4K app's 2K stays 2560x1440 */
    *w=output_w;*h=output_h;return 1;
}
int runtime_output_modes(void){
    if(!output_page)return 0;
    return 1<<OUTPUT_MODE_OFF|1<<OUTPUT_MODE_METALFX|1<<OUTPUT_MODE_720P|1<<OUTPUT_MODE_NATIVE|1<<OUTPUT_MODE_NATIVE_METALFX|
           (output_w>=3840&&output_h>=2160?1<<OUTPUT_MODE_NATIVE_4K:0);
}
int runtime_output_mode(void){
    extern int mfxup_output_enabled(void);
    if(!output_page)return OUTPUT_MODE_OFF;
    uint64_t now=atomic_load((_Atomic uint64_t *)(void *)(output_page+32));
    if(now==pack_size(1280,720))return OUTPUT_MODE_720P;
    if(now==pack_size(3840,2160)&&!mfxup_output_enabled())return OUTPUT_MODE_NATIVE_4K;
    if((unsigned)now>1920)return mfxup_output_enabled()?OUTPUT_MODE_NATIVE_METALFX:OUTPUT_MODE_NATIVE;
    return mfxup_output_enabled()?OUTPUT_MODE_METALFX:OUTPUT_MODE_OFF;
}
int runtime_output_mode_set(int mode){
    extern void dm_gfx_set_flag(const char *,int);
    if(!output_page||mode<OUTPUT_MODE_OFF||mode>OUTPUT_MODE_NATIVE_4K||!(runtime_output_modes()&1<<mode))return 0;
    unsigned w=1920,h=1080;
    if(mode==OUTPUT_MODE_720P){w=1280;h=720;}else if(mode==OUTPUT_MODE_NATIVE||mode==OUTPUT_MODE_NATIVE_METALFX||mode==OUTPUT_MODE_NATIVE_4K)runtime_output_native(mode,&w,&h);
    output_size_set(pack_size(w,h));
    int mfx=mode==OUTPUT_MODE_METALFX||mode==OUTPUT_MODE_NATIVE_METALFX;
    dm_gfx_set_flag("mfx_output",mfx);dm_gfx_set_flag("mfx_upscale",mfx);
    fprintf(stderr,"[totk] picture: %s\n",mode==OUTPUT_MODE_NATIVE_4K?"native 4K":mode==OUTPUT_MODE_720P?"720p":mode==OUTPUT_MODE_NATIVE_METALFX?"native 2K, MetalFX to the window":
            mode==OUTPUT_MODE_NATIVE?"native":mode==OUTPUT_MODE_METALFX?"MetalFX 2K":"off (1080p)");
    picture_mode_save(atomic_load((_Atomic uint64_t *)(void *)(output_page+32)));
    return 1;
}
int runtime_output_mode_cycle(void){
    if(!output_page)return 0;
    extern int mfxup_output_enabled(void);extern void dm_gfx_set_flag(const char *,int);
    int full=atomic_load((_Atomic uint64_t *)(void *)(output_page+32))==pack_size(output_w,output_h);
    int native=0,mfx=0;const char *name;
    if(full)name="2K off";
    else if(mfxup_output_enabled()){native=1;name="native 2K";}
    else{mfx=1;name="MetalFX 2K";}
    if(full!=native)runtime_output_res_toggle();
    dm_gfx_set_flag("mfx_output",mfx);dm_gfx_set_flag("mfx_upscale",mfx);
    fprintf(stderr,"[totk] key 3: %s\n",name);
    picture_mode_save(atomic_load((_Atomic uint64_t *)(void *)(output_page+32)));
    return 1;
}
