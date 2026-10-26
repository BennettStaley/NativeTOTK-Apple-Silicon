/* Other picture shapes on the Mac: key 4 cycles the game's 16:9 -> fitting the window / display (a 21:9 or 32:9 monitor, a
   MacBook's 16:10 in full screen below the notch) -> 21:9 -> 16:9. Fitting is skipped while the window is within 1% of
   16:9 or 21:9, where it would show the same. TOTK_ASPECT=32:9, 16:10, 3440x1440 or 2.39 takes the place of 21:9 and
   starts on it; TOTK_ASPECT=display starts on fitting the window. iPhone / iPad: always the screen's shape (no key
   4; TOTK_ASPECT=0 keeps 16:9), see the TARGET_OS_IPHONE part of aspect_install.
   Not a stretched picture: the camera's projection takes the other aspect ratio, so more of the world is seen at the
   sides and shapes keep their proportions (widening only the viewport's width / height division, the first try, gave a
   stretched 16:9 view; the user saw it at once, 2026-10-19). The game still draws into its 16:9 canvas, which then
   holds that wider view squeezed across; the present pass widens the canvas back over the window (nvn_mtl.m
   present_stretch asks runtime_aspect_now). The pixels are the canvas's: across, 1920 of them (1280 on the phone) cover
   the wider window, so it is that much less sharp across; the 3D scene keeps the game's own render size.
   The community's TOTK-Ultrawide mod for emulators (GPL; none of its code is used here) changes the camera parameter
   DefaultAspect (1.777778 in main's read-only data) and a viewport routine's width / height division. DefaultAspect is
   only copied when a camera is made, so it cannot follow a key; instead each place that hands an aspect ratio to the
   perspective projection (sead's +0xb0, found with a watchpoint on the copies) is patched where the value is produced:
   the instruction branches to a stub in main's text padding that runs it and, while a ratio sits on the data page after
   main (+56, 0 = off), puts that instead. The stub keeps its scratch register on the stack, so no liveness is assumed.
   The vertical view stays: wider than 16:9 more is seen at the sides, narrower (16:10) a little less.
   TOTK_ASPECT_VERT=1 (1.4.3 only; 1.2.1 has no room for the stubs) instead widens the camera's fovy below 16:9 so the
   sides stay as at 16:9 (Vert+, aspect_math.h; factor on the data page +60). It stays off by default until its known
   issues (the TODO at aspect_install) are fixed.
   The HUD, drawn into the same 16:9 canvas, is squeezed back by nvn_mtl.m (uw_ui_squeeze). Not adjusted yet: HUD
   markers that follow objects in the world and pixel-snapped positions (5 more routines TOTK-Ultrawide widens in 1.2.1)
   sit off. */
#pragma once
#include "runtime.h"
#include "aspect_math.h"
#include <TargetConditionals.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Atomic(uint32_t) *g_aspect_slot;   /* points into that page once the stubs are in */
static _Atomic(uint32_t) *g_aspect_vert;   /* the Vert+ factor beside it, when Vert+ stubs are in */
static float g_aspect_wide;               /* key 4's wide step: TOTK_ASPECT's ratio, or 21:9 */
static int g_aspect_named;                /* TOTK_ASPECT gave it */
static atomic_int g_aspect_step;          /* key 4: ASPECT_STEP_GAME, _FIT or _WIDE (aspect_math.h) */
static _Atomic(uint32_t) g_aspect_view;   /* the window's width / height (frontend.m), float bits */

/* TOTK_ASPECT as a ratio (ASPECT_DISPLAY for "display", 0 when unset or "0"). A malformed value stops the start. */
static inline double aspect_requested(void) {
    double a = aspect_parse(getenv("TOTK_ASPECT"));
    if (a == ASPECT_BAD) runtime_fail("TOTK_ASPECT must be <w>:<h>, <w>x<h>, a ratio between 0.5 and 8, or display");
    return a;
}

static inline float aspect_bits_float(uint32_t bits) { float v; memcpy(&v, &bits, 4); return v; }
static inline uint32_t aspect_float_bits(float v) { uint32_t bits; memcpy(&bits, &v, 4); return bits; }

/* The ratio in force (0 = the game's own 16:9): nvn_mtl.m stretches the picture over the window while it is set. */
float runtime_aspect_now(void) { return g_aspect_slot ? aspect_bits_float(atomic_load(g_aspect_slot)) : 0; }

/* The panel (frontend.m): key 4's step, -1 where key 4 is not available; runtime_aspect_now is the ratio it shows. */
int runtime_aspect_step(void) { return g_aspect_slot ? atomic_load(&g_aspect_step) : -1; }

static const char *aspect_step_name(int step) {
    return step == ASPECT_STEP_FIT ? "fit to the window / display" : step == ASPECT_STEP_WIDE ? g_aspect_named ? "the chosen ratio" : "21:9"
                                                                                             : "16:9 (the game's own)";
}

/* Vert+ without stubs of its own (the fork's default on 1.4.3, where main's text padding has no room for the 2 x 304
   bytes of Vert+ stubs and their fovy loads already carry the field of view's stubs): below 16:9 the field of view's
   multiplier (fov_install, below) widens the vertical angle by the shapes' ratio, tan-corrected for the game's usual
   ~50 degrees, so the sides stay as at 16:9 and more is seen above and below. TOTK_ASPECT_VERT=0 keeps the vertical
   view instead. Defined after the field of view. */
static void aspect_fov_follow(float a);

/* Puts the ratio of key 4's step (and its Vert+ factor) on the data page; logs a change. */
static void aspect_apply(void) {
    if (!g_aspect_slot) return;
    double want = aspect_step_ratio(atomic_load(&g_aspect_step), aspect_bits_float(atomic_load(&g_aspect_view)), g_aspect_wide);
    float a = aspect_effective(want), k = g_aspect_vert ? aspect_vert_factor(a) : 0;
    uint32_t old = atomic_exchange(g_aspect_slot, aspect_float_bits(a));
    if (g_aspect_vert) atomic_store(g_aspect_vert, aspect_float_bits(k));
    else aspect_fov_follow(a);
    if (old != aspect_float_bits(a)) {
        if (a && k) fprintf(stderr, "[totk] aspect ratio %.4f, vertical view widened %.3fx (Vert+)\n", a, k);
        else if (a) fprintf(stderr, "[totk] aspect ratio %.4f\n", a);
        else fprintf(stderr, "[totk] aspect ratio 16:9 (the game's own)\n");
    }
}

/* frontend.m: the window's drawable is w x h pixels (on every change). While key 4 fits the window, the view follows. */
void runtime_aspect_view(double w, double h) {
    if (!(w > 0 && h > 0)) return;
    atomic_store(&g_aspect_view, aspect_float_bits((float)(w / h)));
    if (atomic_load(&g_aspect_step) == ASPECT_STEP_FIT) aspect_apply();
}

/* The settings menu's aspect ratio (settings.c display_apply) picks key 4's step: ratio < 0 fits the window ("Fill the
   screen", following it through runtime_aspect_view), 0 is the game's own 16:9, above 0 that ratio becomes the wide
   step. Before the stubs are in it is the start step; TOTK_ASPECT wins there. Ignored where the patch is not (iPhone /
   iPad, other game versions). Key 4 keeps cycling from whatever was chosen. */
static float g_aspect_pending = -2;   /* -2: nothing chosen yet */
static void aspect_choose(float ratio) {
    int step = ratio < 0 ? ASPECT_STEP_FIT : ratio > 0 ? ASPECT_STEP_WIDE : ASPECT_STEP_GAME;
    if (ratio > 0) { g_aspect_wide = ratio; g_aspect_named = 1; }
    atomic_store(&g_aspect_step, step);
    aspect_apply();
    fprintf(stderr, "[totk] aspect ratio (settings): %s, %.4f in force\n", aspect_step_name(step), runtime_aspect_now() ? runtime_aspect_now() : 16.0f / 9);
}
void runtime_aspect_set(float ratio) {
    if (!g_aspect_slot) { g_aspect_pending = ratio; return; }
    aspect_choose(ratio);
}

/* Key 4's step the way the settings menu hands it to runtime_aspect_set: -1 fit, 0 16:9, above 0 the wide ratio. Key 4
   writes it through to the setting (input.c, settings_aspect_follow), so the menu shows what is on. */
float runtime_aspect_choice(void) {
    int step = atomic_load(&g_aspect_step);
    return step == ASPECT_STEP_FIT ? -1 : step == ASPECT_STEP_WIDE ? g_aspect_wide : 0;
}

/* Upstream's setters (its key-1 panel; ours is settings_panel_mac.h with runtime_aspect_set above): the ratio picked
   (16:9 for the game's own, the window's shape while fitting; 0 where the shape cannot be changed), TOTK_ASPECT's ratio,
   and a ratio to pick (0.5..8; within 1% of 16:9 the game's own; 0 where unavailable or out of range). */
float runtime_aspect_ratio(void) {
    if (!g_aspect_slot) return 0;
    int step = atomic_load(&g_aspect_step);
    return step == ASPECT_STEP_WIDE ? g_aspect_wide : step == ASPECT_STEP_FIT ? aspect_bits_float(atomic_load(&g_aspect_view)) : (float)ASPECT_GAME;
}
float runtime_aspect_start(void) { return g_aspect_named ? g_aspect_wide : 0; }
int runtime_aspect_set_ratio(float r) {
    if (!g_aspect_slot || !(r >= 0.5f && r <= 8)) return 0;
    aspect_choose(aspect_near(r, ASPECT_GAME) ? 0 : r);
    return 1;
}

/* Key 4 (runtime/input.c). Returns 0 where it is not available (iPhone / iPad, other game versions). */
int runtime_aspect_toggle(void) {
    if (!g_aspect_slot) return 0;
    int step = aspect_next_step(atomic_load(&g_aspect_step), aspect_bits_float(atomic_load(&g_aspect_view)), g_aspect_wide);
    atomic_store(&g_aspect_step, step);
    aspect_apply();
    float a = runtime_aspect_now();
    if (step == ASPECT_STEP_GAME) fprintf(stderr, "[totk] key 4: 16:9 (the game's own)\n");
    else if (a) fprintf(stderr, "[totk] key 4: %s, aspect ratio %.4f%s\n", aspect_step_name(step), a,
                        step == ASPECT_STEP_FIT ? "; the view follows the window until key 4 again" : "");
    else fprintf(stderr, "[totk] key 4: %s, which is 16:9 here\n", aspect_step_name(step));
    return 1;
}

static inline uint32_t aspect_branch(uint64_t from, uint64_t to) {
    int64_t d = (int64_t)to - (int64_t)from;
    if ((d & 3) || d < -(1 << 27) || d >= (1 << 27)) runtime_fail("aspect patch: branch out of range");
    return 0x14000000u | ((uint32_t)(d >> 2) & 0x3ffffff);
}

/* A data-page float stub: one instruction that leaves a float in s<d> (fdiv s<d>, ... or ldr s<d>, [x<n>, #imm], both
   position independent) branches to a stub in main's text padding that runs it, loads the float at data page +slot and,
   when it is non-zero, puts it in s<d> (DPF_REPLACE, 32 bytes: the aspect ratio) or multiplies s<d> by it (DPF_MULTIPLY,
   48 bytes: the field of view, so the game's own changes such as aiming still come through). x10 and, to multiply, d16
   are kept on the stack, so no liveness is assumed. dpf_stub only builds the words: the phone compares its embedded code. */
enum { DPF_REPLACE = 32, DPF_MULTIPLY = 48 };   /* = the stub's size in bytes */
static inline void dpf_stub(uint32_t *stub, uint32_t ins, uint64_t site, uint64_t s, uint64_t data_off, unsigned slot, int kind) {
    int64_t pages = (int64_t)(data_off >> 12) - (int64_t)((s + 8) >> 12);   /* the module base is page aligned */
    if (pages < -(1 << 20) || pages >= (1 << 20)) runtime_fail("data-page stub: data page out of ADRP range");
    uint32_t imm = (uint32_t)pages & 0x1fffff, d = ins & 31; unsigned n = 0;
    stub[n++] = ins;                                                          /* the game's own value into s<d> */
    stub[n++] = 0xf81f0fea;                                                   /* str x10, [sp, #-16]! */
    stub[n++] = 0x90000000u | ((imm & 3) << 29) | ((imm >> 2) << 5) | 10;     /* adrp x10, <data page> */
    stub[n++] = 0xb9400000u | ((slot / 4) << 10) | (10 << 5) | 10;            /* ldr w10, [x10, #slot] */
    if (kind == DPF_REPLACE) {
        stub[n++] = 0x3400004au;                                              /* cbz w10, +8: 0 = the game's own */
        stub[n++] = 0x1e270000u | (10 << 5) | d;                              /* fmov s<d>, w10 */
    } else {
        stub[n++] = 0x340000aau;                                              /* cbz w10, +20: 0 = the game's own */
        stub[n++] = 0xfd0007f0;                                               /* str d16, [sp, #8] */
        stub[n++] = 0x1e270150;                                               /* fmov s16, w10 */
        stub[n++] = 0x1e300800u | (d << 5) | d;                               /* fmul s<d>, s<d>, s16 */
        stub[n++] = 0xfd4007f0;                                               /* ldr d16, [sp, #8] */
    }
    stub[n++] = 0xf84107ea;                                                   /* ldr x10, [sp], #16 */
    stub[n] = aspect_branch(s + 4 * n, site + 4); n++;                        /* b <after the instruction> */
    while (n < (unsigned)kind / 4) stub[n++] = 0xd503201f;                    /* nop, never reached */
}
static inline void dpf_fail(const char *what, const char *why) { char msg[160]; snprintf(msg, sizeof msg, "%s patch: %s", what, why); runtime_fail(msg); }
/* Mac and the iOS export: checks each site (its word in want[], or without want any stub-able float instruction) and that
   the padding from stub_off is empty and inside main's last text page, then writes the stubs and the branches. */
static inline void dpf_install(Module *m, const char *what, const uint64_t *sites, const uint32_t *want, unsigned nsite, uint64_t stub_off,
                               uint64_t data_off, unsigned slot, int kind) {
    uint64_t page_end = (m->text_end + 0x3fff) & ~0x3fffULL;
    if (stub_off < m->text_end || stub_off + (uint64_t)kind * nsite > page_end) dpf_fail(what, "no room in main's text padding");
    if (data_off + slot + 4 > m->size) dpf_fail(what, "no data page after main");
    for (uint64_t i = 0; i < (uint64_t)kind * nsite; i++) if (m->base[stub_off + i]) dpf_fail(what, "text padding in use");
    for (unsigned i = 0; i < nsite; i++) {
        uint32_t ins; memcpy(&ins, m->base + sites[i], 4);
        if (want ? ins != want[i] : (ins & 0xffe0fc00) != 0x1e201800 && (ins & 0xffc00000) != 0xbd400000) {
            fprintf(stderr, "[totk] %s patch main+%#llx: found %08x\n", what, (unsigned long long)sites[i], ins);
            dpf_fail(what, "the camera code does not match this game version");
        }
    }
    for (unsigned i = 0; i < nsite; i++) {
        uint32_t ins, stub[DPF_MULTIPLY / 4]; uint64_t s = stub_off + (uint64_t)kind * i;
        memcpy(&ins, m->base + sites[i], 4);
        dpf_stub(stub, ins, sites[i], s, data_off, slot, kind);
        memcpy(m->base + s, stub, (size_t)kind);
        uint32_t jump = aspect_branch(sites[i], s);
        memcpy(m->base + sites[i], &jump, 4);
    }
}
/* iOS: 1 when the embedded code carries exactly these stubs, 0 when every site still holds its original word (built
   without them); anything else stops the start. */
static inline int dpf_embedded(Module *m, const char *what, const uint64_t *sites, const uint32_t *want, unsigned nsite, uint64_t stub_off,
                               uint64_t data_off, unsigned slot, int kind) {
    unsigned orig = 0, ok = 0;
    for (unsigned i = 0; i < nsite; i++) {
        uint32_t got, stub[DPF_MULTIPLY / 4]; uint64_t s = stub_off + (uint64_t)kind * i;
        memcpy(&got, m->base + sites[i], 4);
        if (got == want[i]) { orig++; continue; }
        dpf_stub(stub, want[i], sites[i], s, data_off, slot, kind);
        if (got == aspect_branch(sites[i], s) && !memcmp(m->base + s, stub, (size_t)kind)) ok++;
    }
    if (orig == nsite) return 0;
    if (ok != nsite) dpf_fail(what, "embedded code mismatch");
    return 1;
}

/* sites: instructions that leave the ratio in an s register (fdiv s<d>, ... or ldr s<d>, [x<n>, #imm]), each with a
   plain stub from stub_off (ASPECT_STUB bytes per site). The first ncam are camera sites: when vert_off is not 0 and
   the inlined setFovy is there, their fovy load also gets a Vert+ stub (ASPECT_VSTUB bytes each, from vert_off). Both
   areas are main's text padding, checked to be zero and inside the last text page; data_off is the page the loader
   reserves after main. */
static inline void aspect_install(Module *m, const char *version, const uint64_t *sites, unsigned nsite, unsigned ncam,
                                  uint64_t stub_off, uint64_t vert_off, uint64_t data_off) {
#if TARGET_OS_IPHONE
    /* The phone fills its screen (user 2026-10-23: "iPhone full screen"), as key 4 on the Mac: the camera's projection takes the
       screen's shape (2.16 on an iPhone 14), so shapes keep their proportions, and the 1280x720 canvas holding that view
       is widened back over the 1558x720 drawable. No extra memory; 1280 pixels across cover the screen's width (82%). The stubs are already in the embedded main (the iOS export runs this function on
       the Mac), so here each site is only checked to branch to its stub and the ratio slot on the data page is used.
       TOTK_ASPECT=0 (or 16:9) keeps the game's 16:9 with bars at the sides. */
    (void)ncam; (void)vert_off;
    const char *e = getenv("TOTK_ASPECT");
    if (e && (!strcmp(e, "0") || !strcmp(e, "16:9"))) { fprintf(stderr, "[totk] %s aspect ratio: 16:9 (TOTK_ASPECT=%s)\n", version, e); return; }
    if (data_off + ASPECT_DATA_SLOT + 4 > m->size) { fprintf(stderr, "[totk] %s aspect ratio: no data page after main, 16:9\n", version); return; }
    for (unsigned i = 0; i < nsite; i++) {
        uint32_t got; memcpy(&got, m->base + sites[i], 4);
        if (got != aspect_b(sites[i], stub_off + ASPECT_STUB * i)) {
            fprintf(stderr, "[totk] %s aspect ratio: site +%#llx is not the export's stub branch (%08x), 16:9\n", version, (unsigned long long)sites[i], got);
            return;
        }
    }
    g_aspect_slot = (_Atomic(uint32_t) *)(void *)(m->base + data_off + ASPECT_DATA_SLOT);
    atomic_store(g_aspect_slot, 0);
    g_aspect_wide = (float)ASPECT_WIDE;
    atomic_store(&g_aspect_step, ASPECT_STEP_FIT);
    aspect_apply();   /* 0 until the front end reports the drawable (runtime_aspect_view) */
    fprintf(stderr, "[totk] %s aspect ratio: fills the screen (the camera takes the drawable's shape; TOTK_ASPECT=0 for 16:9)\n", version);
#else
    double req = aspect_requested();
    /* TODO(Vert+): opt-in (TOTK_ASPECT_VERT=1) until these are fixed. Known issues:
       - Some UI effects (the button-press effect, for one) sit in the wrong place below 16:9: they are drawn outside
         the HUD quads nvn_mtl.m flattens (uw_ui_squeeze) and probably need the same flattening.
       - Lighting: the first Vert+ changed only the projection's half-angle tangent, sine and cosine and left fovy
         (+0xa0) at the game's value; on 1.4.3 at 16:10 (2940x1846, key 4 on) most sunlit ground lost its light and
         shadows sat wrong. Widening the camera's fovy itself before setFovy derives the rest (this stub) fixed it in
         an A/B at that spot, and the user saw correct sun lighting, shadows and horizon in full screen on a build
         with main at 9e8a514, and again on the latest build; only the UI effects above remain. Which pass read the
         stale value was never found; anything else that reads the camera's field of view (other cameras, a copy of
         fovy) has not been checked. */
    int vert = vert_off && aspect_vert_wanted(getenv("TOTK_ASPECT_VERT"));
    void *tanf_at = vert ? runtime_symbol("tanf") : NULL, *atanf_at = vert ? runtime_symbol("atanf") : NULL;
    if (vert && (!tanf_at || !atanf_at)) { fprintf(stderr, "[totk] aspect patch: the game's tanf / atanf not found, no Vert+\n"); vert = 0; }
    uint64_t page_end = (m->text_end + 0x3fff) & ~0x3fffULL;
    if (stub_off < m->text_end || stub_off + ASPECT_STUB * nsite > page_end) runtime_fail("aspect patch: no room in main's text padding");
    if (vert && (vert_off < m->text_end || vert_off + ASPECT_VSTUB * ncam > page_end)) runtime_fail("aspect patch: no room for the Vert+ stubs");
    if (data_off + ASPECT_ATANF_SLOT + 8 > m->size) runtime_fail("aspect patch: no data page after main");
    for (unsigned i = 0; i < ASPECT_STUB * nsite; i++) if (m->base[stub_off + i]) runtime_fail("aspect patch: text padding in use");
    for (unsigned i = 0; vert && i < ASPECT_VSTUB * ncam; i++) if (m->base[vert_off + i]) runtime_fail("aspect patch: text padding for Vert+ in use");
    unsigned nvert = 0;
    for (unsigned i = 0; i < nsite; i++) {
        uint32_t ins; memcpy(&ins, m->base + sites[i], 4);
        if (!aspect_site_ok(ins)) runtime_fail("aspect patch: the camera code does not match this game version");
        uint32_t code[74] = {0};
        uint64_t s = stub_off + ASPECT_STUB * i;
        const char *err = aspect_stub(ins, s, sites[i], data_off, code);
        uint32_t jump = aspect_b(sites[i], s);
        if (err || !jump) runtime_fail(err ? err : "aspect patch: branch out of range");
        if (vert && i < ncam) {   /* Vert+: the fovy load before it, found from the projection register */
            uint32_t around[8], before[16]; memcpy(around, m->base + sites[i] - 16, sizeof around); memcpy(before, m->base + sites[i] - 64, sizeof before);
            int p = aspect_vert_reg(around), j = p >= 0 ? aspect_fovy_at(before, (unsigned)p) : -1;
            if (j < 0) fprintf(stderr, "[totk] aspect patch: camera site +%#llx is not the expected setFovy, no Vert+ there\n", (unsigned long long)sites[i]);
            else {
                uint64_t fs = sites[i] - 64 + 4 * (uint64_t)j, vs = vert_off + ASPECT_VSTUB * i;
                uint32_t vcode[74], vjump = aspect_b(fs, vs);
                const char *verr = aspect_vstub(before[j], vs, fs, data_off, vcode);
                if (verr || !vjump) runtime_fail(verr ? verr : "aspect patch: branch out of range");
                memcpy(m->base + vs, vcode, sizeof vcode);
                memcpy(m->base + fs, &vjump, 4);
                nvert++;
            }
        }
        memcpy(m->base + s, code, ASPECT_STUB);
        memcpy(m->base + sites[i], &jump, 4);
    }
    if (nvert) {
        uint64_t fa = (uint64_t)(uintptr_t)tanf_at, fb = (uint64_t)(uintptr_t)atanf_at;
        memcpy(m->base + data_off + ASPECT_TANF_SLOT, &fa, 8); memcpy(m->base + data_off + ASPECT_ATANF_SLOT, &fb, 8);
    }
    g_aspect_slot = (_Atomic(uint32_t) *)(void *)(m->base + data_off + ASPECT_DATA_SLOT);
    if (nvert) g_aspect_vert = (_Atomic(uint32_t) *)(void *)(m->base + data_off + ASPECT_VERT_SLOT);
    atomic_store(g_aspect_slot, 0);
    if (g_aspect_vert) atomic_store(g_aspect_vert, 0);
    g_aspect_named = req > 0;
    g_aspect_wide = req > 0 ? (float)req : (float)ASPECT_WIDE;
    int step = req > 0 ? ASPECT_STEP_WIDE : req == ASPECT_DISPLAY ? ASPECT_STEP_FIT : ASPECT_STEP_GAME;
    atomic_store(&g_aspect_step, step);
    if (!req && g_aspect_pending > -2) { aspect_choose(g_aspect_pending); step = atomic_load(&g_aspect_step); }   /* the settings' start shape */
    else aspect_apply();
    char wide[48]; if (g_aspect_named) snprintf(wide, sizeof wide, "%.4f (TOTK_ASPECT)", g_aspect_wide); else snprintf(wide, sizeof wide, "21:9");
    fprintf(stderr, "[totk] %s aspect ratio: key 4 cycles 16:9 / fit to the window / %s, starts at %s; %u site(s), %s, stubs +%#llx\n",
            version, wide, aspect_step_name(step), nsite,
            nvert ? "Vert+ below 16:9 (TOTK_ASPECT_VERT=1, experimental: some UI effects sit off)"
                  : vert_off ? "vertical view kept below 16:9 (TOTK_ASPECT_VERT=1: Vert+, experimental)"
                             : "Vert+ below 16:9 through the field of view (TOTK_ASPECT_VERT=0 keeps the vertical view)",
            (unsigned long long)stub_off);
#endif
}

/* Field of view (1.4.3; Mac and iPhone / iPad): TOTK_FOV=<percent> 70..140 scales the main camera's vertical angle from
   the start (100 or unset = the game's own), runtime_fov_set changes it live (the settings menu). The camera hands its
   fovy to sead's PerspectiveProjection (+0xa0 fovy, then sin / cos / tan of half of it at +0xa4 / +0xa8 / +0xac, the
   aspect ratio at +0xb0); each site is the load of that angle in one of the two functions that write the whole
   projection (the ones whose aspect ratio aspect_install replaces). A DPF_MULTIPLY stub scales it there, before the half
   angle and its sin / cos / tan are derived, so the projection stays consistent and the game's own changes (bow aim,
   first person, cutscene cameras) keep coming through, scaled. The multiplier is a float on the data page after main
   (+64; 0 = the game's own). iOS cannot write code at run time: the iOS export (TOTK_EXPORT_IOS) always puts the stubs
   into the phone's code; the phone checks them and only writes the multiplier (the data page is read-write there). */
#define FOV_DATA_SLOT 64
static _Atomic(uint32_t) *g_fov_slot;   /* points into the data page once the stubs are in (or found embedded) */
static inline int fov_clamp(int percent) { return percent < 70 ? 70 : percent > 140 ? 140 : percent; }
static inline uint32_t fov_bits(int percent) {
    float v = fov_clamp(percent) == 100 ? 0.0f : fov_clamp(percent) / 100.0f; uint32_t bits; memcpy(&bits, &v, 4); return bits;
}
/* The player's percentage and the aspect ratio's own factor (Vert+: a screen taller than 16:9 sees more above and below,
   runtime_fov_aspect), multiplied into the one multiplier on the data page. */
static int g_fov_user = 100; static float g_fov_aspect = 1;
static void fov_write(void) {
    if (!g_fov_slot) return;
    float v = fov_clamp(g_fov_user) / 100.0f * g_fov_aspect; uint32_t bits;
    if (fabsf(v - 1) < 0.001f) v = 0;
    memcpy(&bits, &v, 4); atomic_store(g_fov_slot, bits);
}
/* Live: percent 70..140 (clamped), 100 = the game's own. Ignored where the patch is not in (other game versions). */
void runtime_fov_set(int percent) {
    g_fov_user = fov_clamp(percent);
    if (!g_fov_slot) { fprintf(stderr, "[totk] field of view %d%%: not available in this build\n", percent); return; }
    fov_write();
    fprintf(stderr, "[totk] field of view %d%%\n", g_fov_user);
}
/* The vertical angle's factor for the picture's shape (1 = none): aspect_apply through aspect_fov_follow. */
void runtime_fov_aspect(float k) {
    float was = g_fov_aspect;
    g_fov_aspect = k > 0.5f && k < 2 ? k : 1; fov_write();
    if (fabsf(g_fov_aspect - was) > 0.0005f) fprintf(stderr, "[totk] field of view: x%.3f for the screen's shape (Vert+)\n", g_fov_aspect);
}
static void aspect_fov_follow(float a) {
    const char *e = getenv("TOTK_ASPECT_VERT");
    float k = 1;
    if (!(e && !strcmp(e, "0")) && a > 0 && a < 16.0f / 9) { const float half = 0.436f; k = atanf(tanf(half) * (16.0f / 9) / a) / half; }
    runtime_fov_aspect(k);
}
/* TOTK_FOV as a percentage; 100 when unset. A malformed or out-of-range value stops the start. */
static inline int fov_requested(void) {
    const char *e = getenv("TOTK_FOV"); char rest; int p;
    if (!e || !*e) return 100;
    if (sscanf(e, "%d%c", &p, &rest) != 1 || p < 70 || p > 140) runtime_fail("TOTK_FOV must be a percentage between 70 and 140");
    return p;
}
/* want[]: each site's original word, checked before anything is written (Mac, the iOS export) or against the embedded
   code (iOS, where code built without the stubs leaves the game's own field of view). */
static inline void fov_install(Module *m, const char *version, const uint64_t *sites, const uint32_t *want, unsigned nsite, uint64_t stub_off, uint64_t data_off) {
    int start = fov_requested();
#if TARGET_OS_IPHONE
    if (!dpf_embedded(m, "field of view", sites, want, nsite, stub_off, data_off, FOV_DATA_SLOT, DPF_MULTIPLY)) {
        fprintf(stderr, "[totk] %s field of view: the game's own (not patched at build)\n", version); return;
    }
#else
    dpf_install(m, "field of view", sites, want, nsite, stub_off, data_off, FOV_DATA_SLOT, DPF_MULTIPLY);
#endif
    g_fov_slot = (_Atomic(uint32_t) *)(void *)(m->base + data_off + FOV_DATA_SLOT);
    g_fov_user = start; fov_write();
    fprintf(stderr, "[totk] %s field of view: %d%% (TOTK_FOV), live via runtime_fov_set; %u camera site(s), stubs +%#llx\n",
            version, start, nsite, (unsigned long long)stub_off);
}

/* Fog (1.4.3; Mac and iPhone / iPad): TOTK_FOG=<percent> 0..100 scales the distance fog's density from the start (100 or
   unset = the game's own, 0 = none), runtime_fog_set changes it live (the settings menu). The environment uniform writer
   (main+0xd28b4, the per-frame writer of the "Env" block, found with a write watch on that block) blends the four fog
   objects of the two environment palettes in force and stores each as a colour, a direction and a linear ramp: scale =
   1 / (end - start) (main+0xd3050, fcsel s9, s18, s1, ne: 1 when end = start) and offset = -start * scale (the fnmul
   right after it). The fog amount is that ramp, clamped, times the colour's alpha. A DPF_MULTIPLY stub on the fcsel
   scales s9 and so the offset with it: the whole ramp is multiplied, 50 % reaches the game's fog at twice the distance,
   0 % leaves none. The multiplier is a float on the data page after main (+80; 0 = the game's own, FLT_MIN stands for
   0 %). iOS cannot write code at run time: the iOS export always embeds the stub; the phone checks it and writes the
   multiplier. */
#define FOG_DATA_SLOT 80
static _Atomic(uint32_t) *g_fog_slot;
static int g_fog_percent = 100;
static inline int fog_clamp(int p) { return p < 0 ? 0 : p > 100 ? 100 : p; }
static inline uint32_t fog_bits(int percent) {
    float v = percent >= 100 ? 0.0f : percent <= 0 ? 1.17549435e-38f : percent / 100.0f; uint32_t bits;
    memcpy(&bits, &v, 4); return bits;
}
/* Live: percent 0..100 (clamped), 100 = the game's own. Ignored where the patch is not in (other game versions). */
void runtime_fog_set(int percent) {
    g_fog_percent = fog_clamp(percent);
    if (!g_fog_slot) { fprintf(stderr, "[totk] fog %d%%: not available in this build\n", g_fog_percent); return; }
    atomic_store(g_fog_slot, fog_bits(g_fog_percent)); fprintf(stderr, "[totk] fog %d%%\n", g_fog_percent);
}
static inline int fog_requested(void) {
    const char *e = getenv("TOTK_FOG"); char rest; int p;
    if (!e || !*e) return 100;
    if (sscanf(e, "%d%c", &p, &rest) != 1 || p < 0 || p > 100) runtime_fail("TOTK_FOG must be a percentage between 0 and 100");
    return p;
}
static inline void fog_install(Module *m, const char *version, const uint64_t *sites, const uint32_t *want, unsigned nsite, uint64_t stub_off, uint64_t data_off) {
    int start = fog_requested();
#if TARGET_OS_IPHONE
    if (!dpf_embedded(m, "fog", sites, want, nsite, stub_off, data_off, FOG_DATA_SLOT, DPF_MULTIPLY)) {
        fprintf(stderr, "[totk] %s fog: the game's own (not patched at build)\n", version); return;
    }
#else
    dpf_install(m, "fog", sites, want, nsite, stub_off, data_off, FOG_DATA_SLOT, DPF_MULTIPLY);
#endif
    g_fog_slot = (_Atomic(uint32_t) *)(void *)(m->base + data_off + FOG_DATA_SLOT);
    g_fog_percent = start; atomic_store(g_fog_slot, fog_bits(start));
    fprintf(stderr, "[totk] %s fog: %d%% (TOTK_FOG), live via runtime_fog_set; %u site(s), stubs +%#llx\n", version, start, nsite, (unsigned long long)stub_off);
}

/* Shadow resolution (1.4.3; Mac): 2x or 4x the side of the sun's cascaded shadow map (a square 2D array of 4 depth
   layers, "shadow_map_depth"), from the start (TOTK_SHADOW_RES=2 / 4; 0 or unset is the game's own) and live
   (runtime_shadow_res_set, the settings menu). Each view's side is picked in main+0x683480..: a quality table's two
   sizes blended by the camera's state, times a factor, converted (fcvtzs w11), rounded up to 8 (main+0x683604 add w11,
   w11, #7; and ~7) and stored as both width (+0x3d8) and height (+0x3f8) of the view (stride 0xbb0), which the map's
   creation and the shadow passes read. A stub on that add first shifts w11 left by the low byte of the int on the data
   page (+84: 1 = 2x, 2 = 4x), then right by its second byte (1 = 0.5x, 2 = 0.25x; 0 = the game's own) and caps it at
   2048. The smaller maps (TOTK_SHADOW_RES=0.5 / 0.25) cost less GPU time in the shadow passes and blur the edges; the game also makes maps for its other views (424, 800, 848, 720
   and 1440; measured 2026-10-20: 4x without the cap added 635 MB of GPU memory). Both paths to the rounding
   (main+0x6835a8 jumps to it) go through the stub. x16 and the flags are free there (x16 is written next at
   main+0x683618, the flags at main+0x683640). The iOS export embeds the stub; the phone checks it and leaves +84 at 0.
   Live needs nothing more than the data page: the pick is not a boot-time step. It runs every frame (main+0x682fec,
   called only from main+0x6822f0 <- main+0x67e028, a job the graphics setup at main+0xcce420 registers), and so does the
   map's creation: main+0xfca60 (called every frame from main+0xf5818, the render-target pass setup; also from
   main+0x5683e0 and main+0xf6278) reads the side back from the view (+0x68 / +0x88 of the target at view +0x370) and asks
   main+0x1c2590 for a texture: a cache of render targets keyed by a CRC of the descriptor (size, format, layers) that
   returns a matching entry or recycles an unused one (refcount 0, the free list at +0x2238) from the game's own texture
   heap. The game itself changes the side from frame to frame (the title screen goes through 424, 848, 800, 400 and 720),
   so a new factor is taken at the next frame's pick the same way (measured 2026-10-20, Great Sky Island: 800 -> 2048 ->
   800 -> 1600 -> 800 within a frame of each switch; GPU memory 3380 MB before, 3470 MB after the first 4x and flat
   through 20 more switches). */
#define SHADOW_DATA_SLOT 84
enum { SHADOW_STUB = 40, SHADOW_CAP = 2048 };   /* bytes; the largest side the stub makes */
static inline void shadow_stub(uint32_t *s, uint32_t ins, uint64_t site, uint64_t stub, uint64_t data_off) {
    int64_t pages = (int64_t)(data_off >> 12) - (int64_t)(stub >> 12);   /* the module base is page aligned */
    if (pages < -(1 << 20) || pages >= (1 << 20)) runtime_fail("shadow stub: data page out of ADRP range");
    uint32_t imm = (uint32_t)pages & 0x1fffff, d = (ins >> 5) & 31;
    s[0] = 0x90000000u | ((imm & 3) << 29) | ((imm >> 2) << 5) | 16;          /* adrp x16, <data page> */
    s[1] = 0xb9400000u | ((SHADOW_DATA_SLOT / 4) << 10) | (16 << 5) | 16;    /* ldr w16, [x16, #84] */
    s[2] = 0x1ac02000u | (16 << 16) | (d << 5) | d;                          /* lsl w11, w11, w16 (its low 5 bits) */
    s[3] = 0x53087c00u | (16 << 5) | 16;                                     /* lsr w16, w16, #8 */
    s[4] = 0x1ac02400u | (16 << 16) | (d << 5) | d;                          /* lsr w11, w11, w16 */
    s[5] = 0x52800000u | (SHADOW_CAP << 5) | 16;                             /* mov w16, #2048 */
    s[6] = 0x6b00001fu | (16 << 16) | (d << 5);                              /* cmp w11, w16 */
    s[7] = 0x1a80b000u | (16 << 16) | (d << 5) | d;                          /* csel w11, w11, w16, lt */
    s[8] = ins;                                                              /* add w11, w11, #7 (the game's rounding) */
    s[9] = aspect_branch(stub + 36, site + 4);                               /* b <after the add> */
}
/* The factor as the settings hold it: 0 = the game's own, 2 / 4 = 2x / 4x, -2 / -4 = 0.5x / 0.25x. */
static int g_shadow_factor;   /* the factor in force */
int runtime_shadow_res(void) { return g_shadow_factor; }
static _Atomic(uint32_t) *g_shadow_slot;
static int shadow_valid(int k) { return k == 0 || k == 2 || k == 4 || k == -2 || k == -4; }
static uint32_t shadow_word(int k) { return k == 4 ? 2 : k == 2 ? 1 : k == -2 ? 1u << 8 : k == -4 ? 2u << 8 : 0; }
static const char *shadow_name(int k) { return k == 4 ? "4x" : k == 2 ? "2x" : k == -2 ? "0.5x" : k == -4 ? "0.25x" : "the game's own"; }
int runtime_shadow_res_set(int k) {
    if (!shadow_valid(k)) return -1;
    if (!g_shadow_slot) { fprintf(stderr, "[totk] shadow resolution %s: not available in this build\n", shadow_name(k)); return -1; }
    atomic_store(g_shadow_slot, shadow_word(k)); g_shadow_factor = k;
    fprintf(stderr, "[totk] shadow resolution: %s (live)\n", shadow_name(k));
    return 0;
}
static inline void shadow_install(Module *m, const char *version, uint64_t stub_off, uint64_t data_off) {
    const uint64_t site = 0x683604; const uint32_t want = 0x11001d6b;   /* add w11, w11, #7 */
    uint64_t page_end = (m->text_end + 0x3fff) & ~0x3fffULL;
    if (stub_off < m->text_end || stub_off + SHADOW_STUB > page_end) dpf_fail("shadow resolution", "no room in main's text padding");
    if (data_off + SHADOW_DATA_SLOT + 4 > m->size) dpf_fail("shadow resolution", "no data page after main");
    uint32_t stub[SHADOW_STUB / 4], jump = aspect_branch(site, stub_off), got;
    shadow_stub(stub, want, site, stub_off, data_off);
    memcpy(&got, m->base + site, 4);
#if TARGET_OS_IPHONE
    if (got == want) { fprintf(stderr, "[totk] %s shadow resolution: the game's own (not patched at build)\n", version); return; }
    if (got != jump || memcmp(m->base + stub_off, stub, SHADOW_STUB)) dpf_fail("shadow resolution", "embedded code mismatch");
#else
    if (got != want) dpf_fail("shadow resolution", "the shadow code does not match this game version");
    for (uint64_t i = 0; i < SHADOW_STUB; i++) if (m->base[stub_off + i]) dpf_fail("shadow resolution", "text padding in use");
    memcpy(m->base + stub_off, stub, SHADOW_STUB); memcpy(m->base + site, &jump, 4);
    g_shadow_slot = (_Atomic(uint32_t) *)(void *)(m->base + data_off + SHADOW_DATA_SLOT);
#endif
    const char *e = getenv("TOTK_SHADOW_RES"); int k = 0; char rest;
    if (e && *e) {
        if (!strcmp(e, "0.5")) k = -2;
        else if (!strcmp(e, "0.25")) k = -4;
        else if (sscanf(e, "%d%c", &k, &rest) != 1 || (k != 1 && (k < 0 || !shadow_valid(k)))) runtime_fail("TOTK_SHADOW_RES must be 0.25, 0.5, 0, 2 or 4");
    }
    if (k == 1 || getenv("TOTK_EXPORT_IOS")) k = 0;   /* the export builds code only; the phone decides at its own launch */
    atomic_store((_Atomic(uint32_t) *)(void *)(m->base + data_off + SHADOW_DATA_SLOT), shadow_word(k));
    g_shadow_factor = k;
    fprintf(stderr, "[totk] %s shadow resolution: %s (TOTK_SHADOW_RES); main+%#llx, stub +%#llx\n", version, shadow_name(k), (unsigned long long)site, (unsigned long long)stub_off);
}
