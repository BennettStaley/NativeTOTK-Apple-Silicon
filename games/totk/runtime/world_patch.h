/* World settings (1.4.3; Mac and iPhone / iPad): the time of day's speed and the camera's render distance, both live.
   Each site is one instruction that leaves a float in an s register; it branches to a 32-byte stub in main's text padding
   that runs it, loads a float from the data page after main and, when that is non-zero, multiplies the register by it
   (0 = the game's own). Unlike aspect_patch.h's dpf stubs these keep nothing on the stack: each site names an x and an s
   register that are dead there (checked by reading the code that follows, noted per site below), which keeps the stub to
   8 words. iOS cannot write code at run time: the iOS export (TOTK_EXPORT_IOS) always puts the stubs into the phone's
   code, the phone checks them and only writes the factors (the data page is read-write there).

   Time speed. The world clock's per-frame update (main+0x68c098, called from main+0x68ac90 and main+0xb6c364) stores
   its step at +0x3c (twice the frame's step it is handed; zero while the clock is stopped by the game) and then, at
   main+0x68c15c, loads it into s9 and adds s9 to its three time-of-day accumulators (+0x28, +0x78, +0x90; each wrapped
   at the day length 86400.0, main+0x2d011a0, the day counters at +0x2c / +0x7c / +0x94), or, when the game moves the
   clock towards a set time (main+0x68c888), hands s9 to that step (main+0x21bdbf8, which takes at most the time left).
   The game holds the clock itself in places (the opening beneath Hyrule Castle: the update runs every frame with the
   time at 25200.0, 7:00, and a zero step, through its own stopped-clock path that skips this load), so there the
   setting has nothing to scale. Scaling s9 scales the time of day only: the game-minute counter at +0x10 / +0xc
   reloads +0x3c itself and keeps the game's own pace. Paused is the factor -0.0 (its bits are non-zero, so the stub multiplies; the product is -0.0 and
   adding it leaves every accumulator as it is).
     main+0x68c15c  ldr s9, [x19, #0x3c]  (bd403e69)   x8 and s1 dead: the fall-through writes s1 at +0x68c16c and w8 at
                                                       +0x68c184 before reading them; the 0x68c888 path calls first.
   Render distance. Both functions that write the whole sead PerspectiveProjection (aspect_patch.h: fovy +0xa0, aspect
   +0xb0) store the far clip plane at +0x9c (the near one at +0x98) from the camera's own value; the stub scales it there,
   so the projection, its frustum and everything culled against it follow. The game's own far plane is 25000 (25 km;
   measured in game, the projection's +0x9c then read 12500 at 50 % and 75000 at 300 %), more than the map is across:
   below 100 % distant land is cut off sooner. Above it is no longer offered: it broke the horizon seen from high in the sky
   (distance_clamp), and level of detail and streaming are separate anyway.
     main+0x628c30  ldr s0, [x19, #0x138] (bd413a60)   per-frame camera update; x8 and s2 dead: the next uses are a write
                                                       (+0x628c6c) or after the sin / cos / tan calls.
     main+0xb43420  ldr s0, [x22, #0x60]  (bd4062c0)   camera setup; x8 and s2 dead: the next use of x8 is the adrp at
                                                       +0xb43478, s2 is not read before the calls.
   Both sit before the field-of-view sites (+0x628c3c, +0xb4342c), whose stubs leave x8 and s2 alone. */
#pragma once
#include "runtime.h"
#include <TargetConditionals.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIME_SPEED_DATA_SLOT 72        /* the time-of-day factor on the data page after main; 0.0 = the game's own */
#define RENDER_DISTANCE_DATA_SLOT 76   /* the far-plane factor */
enum { WORLD_STUB = 32 };              /* bytes per stub */

typedef struct { uint64_t off; uint32_t want; unsigned x, s; } WorldSite;   /* the original word and two dead registers */

static inline uint32_t world_branch(uint64_t from, uint64_t to) {
    int64_t d = (int64_t)to - (int64_t)from;
    if ((d & 3) || d < -(1 << 27) || d >= (1 << 27)) runtime_fail("world patch: branch out of range");
    return 0x14000000u | ((uint32_t)(d >> 2) & 0x3ffffff);
}
/* The 8 words at stub offset s for a site: the instruction, then s<d> *= the data page float when it is non-zero. */
static inline void world_stub(uint32_t *stub, const WorldSite *site, uint64_t s, uint64_t data_off, unsigned slot) {
    int64_t pages = (int64_t)(data_off >> 12) - (int64_t)((s + 4) >> 12);   /* the module base is page aligned */
    if (pages < -(1 << 20) || pages >= (1 << 20)) runtime_fail("world patch: data page out of ADRP range");
    uint32_t imm = (uint32_t)pages & 0x1fffff, d = site->want & 31, x = site->x, t = site->s;
    stub[0] = site->want;                                                   /* the game's own value into s<d> */
    stub[1] = 0x90000000u | ((imm & 3) << 29) | ((imm >> 2) << 5) | x;      /* adrp x<x>, <data page> */
    stub[2] = 0xb9400000u | ((slot / 4) << 10) | (x << 5) | x;              /* ldr w<x>, [x<x>, #slot] */
    stub[3] = 0x34000060u | x;                                              /* cbz w<x>, +12: 0 = the game's own */
    stub[4] = 0x1e270000u | (x << 5) | t;                                   /* fmov s<t>, w<x> */
    stub[5] = 0x1e200800u | (t << 16) | (d << 5) | d;                       /* fmul s<d>, s<d>, s<t> */
    stub[6] = world_branch(s + 24, site->off + 4);                          /* b <after the instruction> */
    stub[7] = 0xd503201f;                                                   /* nop, never reached */
}
static inline void world_fail(const char *what, const char *why) { char msg[160]; snprintf(msg, sizeof msg, "%s patch: %s", what, why); runtime_fail(msg); }
/* Mac and the iOS export: checks each site's word and that the padding is empty and inside main's last text page, then
   writes the stubs and the branches. iOS: 1 when the embedded code carries exactly these stubs, 0 when every site still
   holds its original word (built without them); anything else stops the start. */
static inline int world_install(Module *m, const char *what, const WorldSite *sites, unsigned nsite, uint64_t stub_off, uint64_t data_off, unsigned slot) {
    if (data_off + slot + 4 > m->size) world_fail(what, "no data page after main");
#if TARGET_OS_IPHONE
    unsigned orig = 0, ok = 0;
    for (unsigned i = 0; i < nsite; i++) {
        uint32_t got, stub[WORLD_STUB / 4]; uint64_t s = stub_off + (uint64_t)WORLD_STUB * i;
        memcpy(&got, m->base + sites[i].off, 4);
        if (got == sites[i].want) { orig++; continue; }
        world_stub(stub, &sites[i], s, data_off, slot);
        if (got == world_branch(sites[i].off, s) && !memcmp(m->base + s, stub, WORLD_STUB)) ok++;
    }
    if (orig == nsite) return 0;
    if (ok != nsite) world_fail(what, "embedded code mismatch");
    return 1;
#else
    uint64_t page_end = (m->text_end + 0x3fff) & ~0x3fffULL;
    if (stub_off < m->text_end || stub_off + (uint64_t)WORLD_STUB * nsite > page_end) world_fail(what, "no room in main's text padding");
    for (uint64_t i = 0; i < (uint64_t)WORLD_STUB * nsite; i++) if (m->base[stub_off + i]) world_fail(what, "text padding in use");
    for (unsigned i = 0; i < nsite; i++) {
        uint32_t ins; memcpy(&ins, m->base + sites[i].off, 4);
        if (ins != sites[i].want) {
            fprintf(stderr, "[totk] %s patch main+%#llx: found %08x, expected %08x\n", what, (unsigned long long)sites[i].off, ins, sites[i].want);
            world_fail(what, "the game code does not match this game version");
        }
    }
    for (unsigned i = 0; i < nsite; i++) {
        uint32_t stub[WORLD_STUB / 4]; uint64_t s = stub_off + (uint64_t)WORLD_STUB * i;
        world_stub(stub, &sites[i], s, data_off, slot);
        memcpy(m->base + s, stub, WORLD_STUB);
        uint32_t jump = world_branch(sites[i].off, s);
        memcpy(m->base + sites[i].off, &jump, 4);
    }
    return 1;
#endif
}

/* The factors on the data page, and what the settings asked for before the stubs were in (the start values). */
static _Atomic(uint32_t) *g_time_slot, *g_distance_slot;
static int g_time_percent = 100, g_distance_percent = 100;
static inline int time_speed_valid(int p) { return p == 0 || p == 25 || p == 50 || p == 100 || p == 200 || p == 400 || p == 800; }
/* At most the game's own: a far plane past 25 km breaks the horizon seen from high in the sky (the distant land and sky
   islands black past the old far plane at 2800 m, as soon as the factor goes above 1), and the game streams and details the
   land the same either way. */
static inline int distance_clamp(int p) { return p < 10 ? 10 : p > 100 ? 100 : p; }
static inline uint32_t world_bits(float v) { uint32_t bits; memcpy(&bits, &v, 4); return bits; }
static void time_speed_write(void) {
    if (!g_time_slot) return;
    /* 100% = 0 (the game's own); paused = -0.0, which multiplies (non-zero bits) to an unchanged clock */
    atomic_store(g_time_slot, g_time_percent == 100 ? 0 : g_time_percent == 0 ? 0x80000000u : world_bits(g_time_percent / 100.0f));
}
static void distance_write(void) {
    if (!g_distance_slot) return;
    atomic_store(g_distance_slot, g_distance_percent == 100 ? 0 : world_bits(g_distance_percent / 100.0f));
}
/* Live (the settings menu): percent 0 (paused), 25, 50, 100 (the game's own), 200, 400 or 800. */
void runtime_time_speed_set(int percent) {
    if (!time_speed_valid(percent)) percent = 100;
    g_time_percent = percent; time_speed_write();
    fprintf(stderr, "[totk] time of day speed %d%%%s\n", percent, g_time_slot ? "" : " (not available in this build)");
}
/* Live: percent 10..100 (clamped) of the camera's far clip distance, 100 = the game's own. */
void runtime_render_distance_set(int percent) {
    g_distance_percent = distance_clamp(percent); distance_write();
    fprintf(stderr, "[totk] render distance %d%%%s\n", g_distance_percent, g_distance_slot ? "" : " (not available in this build)");
}
static inline int world_env(const char *name, int def, int lo, int hi) {
    const char *e = getenv(name); char rest; int p;
    if (!e || !*e) return def;
    if (sscanf(e, "%d%c", &p, &rest) != 1 || p < lo || p > hi) { char msg[96]; snprintf(msg, sizeof msg, "%s must be a percentage between %d and %d", name, lo, hi); runtime_fail(msg); }
    return p;
}
/* Mac and the iOS export: installs both (always; the factors switch them on). iOS: finds them embedded or leaves the game's
   own. The start values are TOTK_TIME_SPEED (0, 25, 50, 100, 200, 400, 800) and TOTK_RENDER_DISTANCE (10..300, above 100 taken as 100). */
static inline void world_patches_install(Module *m, const char *version, uint64_t stub_off, uint64_t data_off) {
    static const WorldSite time_sites[] = {{0x68c15c, 0xbd403e69 /* ldr s9, [x19, #0x3c] */, 8, 1}};
    static const WorldSite far_sites[] = {{0x628c30, 0xbd413a60 /* ldr s0, [x19, #0x138] */, 8, 2},
                                          {0xb43420, 0xbd4062c0 /* ldr s0, [x22, #0x60] */, 8, 2}};
    int t = world_env("TOTK_TIME_SPEED", g_time_percent, 0, 800);
    if (!time_speed_valid(t)) runtime_fail("TOTK_TIME_SPEED must be 0, 25, 50, 100, 200, 400 or 800");
    int far = distance_clamp(world_env("TOTK_RENDER_DISTANCE", g_distance_percent, 10, 300));   /* above 100: the game's own */
    g_time_percent = t; g_distance_percent = far;
    if (world_install(m, "time speed", time_sites, 1, stub_off, data_off, TIME_SPEED_DATA_SLOT)) {
        g_time_slot = (_Atomic(uint32_t) *)(void *)(m->base + data_off + TIME_SPEED_DATA_SLOT); time_speed_write();
        fprintf(stderr, "[totk] %s time of day: %d%% (TOTK_TIME_SPEED), live via runtime_time_speed_set; stub +%#llx\n", version, t, (unsigned long long)stub_off);
    } else fprintf(stderr, "[totk] %s time of day: the game's own (not patched at build)\n", version);
    if (world_install(m, "render distance", far_sites, 2, stub_off + WORLD_STUB, data_off, RENDER_DISTANCE_DATA_SLOT)) {
        g_distance_slot = (_Atomic(uint32_t) *)(void *)(m->base + data_off + RENDER_DISTANCE_DATA_SLOT); distance_write();
        fprintf(stderr, "[totk] %s render distance: %d%% (TOTK_RENDER_DISTANCE), live via runtime_render_distance_set; 2 camera sites, stubs +%#llx\n",
                version, far, (unsigned long long)(stub_off + WORLD_STUB));
    } else fprintf(stderr, "[totk] %s render distance: the game's own (not patched at build)\n", version);
}
