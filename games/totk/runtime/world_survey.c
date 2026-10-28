/* Live map survey for the selected TOTK version. No arbitrary memory writes and no raw position pokes.
   Offset leads: aquacluck/totk-lotuskit 59dcfcc; independently checked against our main in
   analysis/continuous-survey-entry-named.c. forceSetMatrix routes through the physics component,
   or queues a next transform, unlike merely changing the actor's displayed position.
   Only called after the actual game frame-end GPU wait, never from the input file thread. */
#include "world_survey.h"
#include "actor_spawn.h"
#include "game_version.h"
#include "shrine_auto.h"
#include <ctype.h>
#include <limits.h>
#include <mach/mach.h>
#include "mach_vm_compat.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef GAME_BOTW
/* Breath of the Wild 1.6.0: no frame-end hook (the survey sync object is never found, see runtime_survey_sync), so
   "survey pos" reads and "survey move" writes the player's position from the command thread. The pointer chains from
   main's data segment are the ones Bloom (Swiffy22) found for 1.6.0 (the botw-save-state tool uses them); the code is
   our own. Chains: Havok position = [[[main+0x2d37af0]+0xf0]+0xd28]+0x5e0 (float xyz);
                    world matrix   = [[main+0x2ca1140]+0x60]+0x398 (3x4 row-major, translation at [3], [7], [11]);
                    camera         = [[main+0x2ce1090]+0x60]+0x20 {position, look-at, up, fovy, ?, aspect}.
   Writing those two is not enough: the actor and the Havok object keep a dozen more copies of the position (feet,
   previous positions, swept transforms), so a move shifts every copy near the player by the same offset ("survey scan"
   lists them). A far place is not loaded, and the player freezes there; the move is therefore a flight at a speed the
   streaming keeps up with (2 m per frame worked, 8 m did not): survey move <tag> X Y Z <frames>, 1200 frames for 2.3 km.
   Verified on the Mac 2026-10-21 (5 m step: walks, camera follows; Hebra to Oman Au shrine: lands, in control). */
_Atomic int runtime_survey_pending;
const int32_t runtime_survey_sync[2] = {0, -1};   /* read by hle_nvn.c only once the survey found the system object, which it never does here */
void runtime_survey_tick(uint64_t frame) { (void)frame; }
void runtime_survey_cancel_user(void) { }
#if defined(GAME_BOTW160)
enum { HAVOK_ROOT = 0x2d37af0, HAVOK_STEP1 = 0xf0, HAVOK_STEP2 = 0xd28, HAVOK_POS = 0x5e0,
       MATRIX_ROOT = 0x2ca1140, MATRIX_STEP1 = 0x60, MATRIX_OFF = 0x398,
       CAMERA_ROOT = 0x2ce1090, CAMERA_STEP1 = 0x60, CAMERA_OFF = 0x20 };   /* camera pan matrix (same research) */
static uintptr_t survey_main;
static _Atomic int survey_enabled;
static int read_mem(uintptr_t address, void *out, size_t size) {
    if (!address || address > UINTPTR_MAX - size) return 0;
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size, (uintptr_t)out, &got) == KERN_SUCCESS && got == size;
}
static uintptr_t read_ptr(uintptr_t address) { uintptr_t p = 0; return read_mem(address, &p, sizeof p) ? p : 0; }
static int valid_xyz(const float xyz[3]) {
    return isfinite(xyz[0]) && isfinite(xyz[1]) && isfinite(xyz[2]) &&
           fabsf(xyz[0]) <= 6000 && xyz[1] >= -1000 && xyz[1] <= 3000 && fabsf(xyz[2]) <= 6000;
}
/* The two positions, or 0 when a link of a chain is missing (before the world is up) or the values are not a place. */
static uintptr_t havok_position(void) {
    uintptr_t a = read_ptr(survey_main + HAVOK_ROOT); if (a) a = read_ptr(a + HAVOK_STEP1); if (a) a = read_ptr(a + HAVOK_STEP2);
    float xyz[3];
    return a && read_mem(a + HAVOK_POS, xyz, sizeof xyz) && valid_xyz(xyz) ? a + HAVOK_POS : 0;
}
static uintptr_t world_matrix(void) {
    uintptr_t a = read_ptr(survey_main + MATRIX_ROOT); if (a) a = read_ptr(a + MATRIX_STEP1);
    float m[12];
    if (!a || !read_mem(a + MATRIX_OFF, m, sizeof m)) return 0;
    for (int i = 0; i < 12; i++) if (!isfinite(m[i])) return 0;
    float t[3] = {m[3], m[7], m[11]};
    return valid_xyz(t) ? a + MATRIX_OFF : 0;
}
void runtime_survey_init(Module *m) {
    const char *enabled = getenv("TOTK_SURVEY");
    if (!enabled || strcmp(enabled, "1")) return;
    const char *owner = getenv("TOTK_SLOT_OWNER"), *profile = getenv("TOTK_PROFILE_DIR");
    char target[PATH_MAX];
    if (!owner || !strcmp(owner, "user") || !profile || !realpath(profile, target))
        runtime_fail("TOTK_SURVEY requires an agent-owned session and a separate test profile");
    if (strcmp(m->name, "main") || HAVOK_ROOT + 8u > m->size || MATRIX_ROOT + 8u > m->size)
        runtime_fail("survey: Breath of the Wild 1.6.0 main expected");
    survey_main = (uintptr_t)m->base;
    atomic_store(&survey_enabled, 1);
    fprintf(stderr, "[survey] enabled (Breath of the Wild 1.6.0): pos and move act on the player's Havok position and world matrix\n");
}
static void report(const char *tag, const char *status) {
    uintptr_t h = havok_position(), w = world_matrix();
    float xyz[3] = {0}, m[12] = {0};
    if (h) read_mem(h, xyz, sizeof xyz);
    if (w) read_mem(w, m, sizeof m);
    fprintf(stderr, "[survey] tag=%s status=%s havok=%#llx xyz=%.6f,%.6f,%.6f matrix=%#llx xyz=%.6f,%.6f,%.6f\n",
            tag, status, (unsigned long long)h, xyz[0], xyz[1], xyz[2], (unsigned long long)w, m[3], m[7], m[11]);
}
/* survey scan X Y Z: diagnostic, lists every float triple within 2 m of X Y Z in the player actor (first 0x4000 bytes of
   [[main+0x2ca1140]+0x60]), the Havok object ([[main+0x2d37af0]+0xf0]+0xd28, 0x1000 bytes) and the actor's +0xf0 object. */
static void scan_block(const char *what, uintptr_t base, size_t size, const float xyz[3]) {
    float *buf = malloc(size); if (!buf) return;
    if (read_mem(base, buf, size)) {
        for (size_t i = 0; i + 3 <= size / 4; i++)
            if (fabsf(buf[i] - xyz[0]) < 2 && fabsf(buf[i + 1] - xyz[1]) < 2 && fabsf(buf[i + 2] - xyz[2]) < 2)
                fprintf(stderr, "[survey-scan] %s+%#zx = %.3f,%.3f,%.3f\n", what, i * 4, buf[i], buf[i + 1], buf[i + 2]);
    } else fprintf(stderr, "[survey-scan] %s at %#llx unreadable\n", what, (unsigned long long)base);
    free(buf);
}
static void survey_scan(const float xyz[3]) {
    uintptr_t actor = read_ptr(survey_main + MATRIX_ROOT); if (actor) actor = read_ptr(actor + MATRIX_STEP1);
    uintptr_t h = read_ptr(survey_main + HAVOK_ROOT); if (h) h = read_ptr(h + HAVOK_STEP1); if (h) h = read_ptr(h + HAVOK_STEP2);
    fprintf(stderr, "[survey-scan] actor=%#llx havok=%#llx looking for %.2f,%.2f,%.2f\n", (unsigned long long)actor, (unsigned long long)h, xyz[0], xyz[1], xyz[2]);
    if (actor) scan_block("actor", actor, 0x4000, xyz);
    if (h) scan_block("havok", h, 0x1000, xyz);
    uintptr_t h1 = read_ptr(survey_main + HAVOK_ROOT); if (h1) h1 = read_ptr(h1 + HAVOK_STEP1);
    if (h1) scan_block("havok1", h1, 0x1000, xyz);
}
int runtime_survey_command(const char *line) {
    if (strncmp(line, "survey ", 7)) return 0;
    char op[16], tag[64]; float xyz[3] = {0}; unsigned hold = 0; int used = 0, kind = 0;
    if (sscanf(line, "survey %15s %63s %n", op, tag, &used) == 2 && used && !line[used] && !strcmp(op, "pos")) kind = 1;
    used = 0;
    if (sscanf(line, "survey move %63s %f %f %f %u %n", tag, &xyz[0], &xyz[1], &xyz[2], &hold, &used) == 5 &&
        used && !line[used] && valid_xyz(xyz) && hold >= 1 && hold <= 3000) kind = 2;
    used = 0;
    if (sscanf(line, "survey scan %f %f %f %n", &xyz[0], &xyz[1], &xyz[2], &used) == 3 && used && !line[used]) {
        if (atomic_load(&survey_enabled)) survey_scan(xyz); return 1;
    }
    if (!kind) { fprintf(stderr, "[survey] rejected: expected pos <tag> or move <tag> X Y Z <1..3000 frames>\n"); return 1; }
    for (const char *p = tag; *p; ++p) if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') {
        fprintf(stderr, "[survey] rejected: invalid tag\n"); return 1;
    }
    if (!atomic_load(&survey_enabled)) { fprintf(stderr, "[survey] tag=%s status=disabled\n", tag); return 1; }
    if (kind == 1) { report(tag, havok_position() ? "position" : "no_player"); return 1; }
    uintptr_t h = havok_position(), w = world_matrix();
    if (!h || !w) { report(tag, "no_player"); return 1; }
    report(tag, "move_begin");
    /* Every float triple within 2 m of the player's position in the actor, the Havok object and its parent, plus the
       world matrix and the camera matrix translations, keeps its offset from the player and moves with him: one pass
       collects them, then they are written for <hold> frames (30 Hz) so the physics settles on the new place. */
    float cur[3]; read_mem(h, cur, sizeof cur);
    struct { uintptr_t at; float rel[3]; } slot[160]; unsigned n = 0;
    uintptr_t actor = read_ptr(survey_main + MATRIX_ROOT); if (actor) actor = read_ptr(actor + MATRIX_STEP1);
    uintptr_t hk1 = read_ptr(survey_main + HAVOK_ROOT); if (hk1) hk1 = read_ptr(hk1 + HAVOK_STEP1);
    uintptr_t hk = hk1 ? read_ptr(hk1 + HAVOK_STEP2) : 0;
    uintptr_t cam0 = read_ptr(survey_main + CAMERA_ROOT), cam = cam0 ? read_ptr(cam0 + CAMERA_STEP1) : 0;
    fprintf(stderr, "[survey] actor=%#llx havok=%#llx havok1=%#llx cam0=%#llx cam=%#llx\n", (unsigned long long)actor, (unsigned long long)hk, (unsigned long long)hk1, (unsigned long long)cam0, (unsigned long long)cam);
    /* the camera block at CAMERA_OFF is {position, look-at, up, fovy, ?, aspect}: position and look-at sit within 10 m of the player */
    struct { uintptr_t base; size_t size; float window; } blocks[4] = {{actor, 0x4000, 2}, {hk, 0x1000, 2}, {hk1, 0x1000, 2}, {cam ? cam + CAMERA_OFF : 0, 0x18, 12}};
    for (unsigned b = 0; b < 4; b++) {
        if (!blocks[b].base) continue;
        float *buf = malloc(blocks[b].size); if (!buf) continue;
        float win = blocks[b].window;
        if (read_mem(blocks[b].base, buf, blocks[b].size))
            for (size_t k = 0; k + 3 <= blocks[b].size / 4 && n < 150; k++)
                if (fabsf(buf[k] - cur[0]) < win && fabsf(buf[k + 1] - cur[1]) < win && fabsf(buf[k + 2] - cur[2]) < win) {
                    slot[n].at = blocks[b].base + k * 4;
                    for (int c = 0; c < 3; c++) slot[n].rel[c] = buf[k + c] - cur[c];
                    n++; k += 2;
                }
        free(buf);
    }
    /* matrix translations are not contiguous: world matrix [3],[7],[11]; camera matrix at CAMERA_OFF the same */
    uintptr_t mats[1] = {w}; unsigned nmat = 0; struct { uintptr_t at; float rel[3]; } mat[1];
    for (unsigned q = 0; q < 1; q++) {
        float m[12];
        if (!mats[q] || !read_mem(mats[q], m, sizeof m)) continue;
        mat[nmat].at = mats[q]; mat[nmat].rel[0] = m[3] - cur[0]; mat[nmat].rel[1] = m[7] - cur[1]; mat[nmat].rel[2] = m[11] - cur[2];
        fprintf(stderr, "[survey] %s matrix at %#llx: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n", q ? "camera" : "world", (unsigned long long)mats[q],
                m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
        if (q == 1 && (fabsf(mat[nmat].rel[0]) > 300 || fabsf(mat[nmat].rel[1]) > 300 || fabsf(mat[nmat].rel[2]) > 300)) continue;   /* camera not near the player: leave it */
        nmat++;
    }
    fprintf(stderr, "[survey] tag=%s moving %u triples and %u matrices by %.2f,%.2f,%.2f\n", tag, n, nmat, xyz[0] - cur[0], xyz[1] - cur[1], xyz[2] - cur[2]);
    /* A far place is not loaded: the player is frozen until the world streams in around him, which never happens when he
       appears there at once. So the move is a flight: a straight line from here to there over <hold> frames (30 Hz), the
       world streaming in on the way as it does for a fast glide. 300 frames carry 2.3 km at about 8 m per frame. */
    for (unsigned i = 0; i < hold; i++) {
        float f = (float)(i + 1) / (float)hold, at[3];
        for (int c = 0; c < 3; c++) at[c] = cur[c] + (xyz[c] - cur[c]) * f;
        for (unsigned k = 0; k < n; k++) { float v[3] = {at[0] + slot[k].rel[0], at[1] + slot[k].rel[1], at[2] + slot[k].rel[2]}; memcpy((void *)slot[k].at, v, sizeof v); }
        for (unsigned k = 0; k < nmat; k++) { float *m = (float *)mat[k].at; m[3] = at[0] + mat[k].rel[0]; m[7] = at[1] + mat[k].rel[1]; m[11] = at[2] + mat[k].rel[2]; }
        usleep(33333);
    }
    report(tag, "move_done");
    return 1;
}
#endif   /* GAME_BOTW160 */
#if !defined(GAME_BOTW160)
void runtime_survey_init(Module *m) {
    (void)m;
    if (getenv("TOTK_SURVEY")) runtime_fail("survey: only Breath of the Wild 1.6.0 has the player position chains");
}
int runtime_survey_command(const char *line) { (void)line; return 0; }
#endif
int runtime_noclip_available(void) { return 0; }
void runtime_noclip_set(int on) { (void)on; }
int runtime_noclip_on(void) { return 0; }
void runtime_noclip_set_speed(float v) { (void)v; }
int runtime_noclip_goto(const float xyz[3]) { (void)xyz; return 0; }
float runtime_noclip_speed(void) { return 0; }
const int runtime_noclip_speeds[NOCLIP_NSPEEDS] = {5, 12, 25, 50, 100};
void runtime_noclip_step_speed(int dir) { (void)dir; }
uint64_t runtime_noclip_taken(void) { return 0; }
int runtime_autoshrine_available(void) { return 0; }   /* auto shrine: the TOTK shrines only */
int runtime_autoshrine_start(int one) { (void)one; return 0; }
int runtime_autoshrine_finish(int shrine) { (void)shrine; return 0; }
void runtime_autoshrine_stop(void) { }
void runtime_autoshrine_set_chests(int on) { (void)on; }
int runtime_autoshrine_chests(void) { return 0; }
int runtime_autoshrine_status(char *text, size_t size) { if (size) snprintf(text, size, "Not available in this game"); return 0; }
int runtime_autoshrine_command(const char *line) { (void)line; return 0; }
void runtime_autoshrine_romfs(const char *path) { (void)path; }
#else
enum { SCENE_SLOT = GAME_ADDR(0x4663f08), FORCE_MATRIX = GAME_ADDR(0x69a18c), ACTOR_POS = GAME_ACTOR_POS, ACTOR_ROT = GAME_ACTOR_ROT };
static uintptr_t survey_main;
static void (*survey_force)(void *, const float *, uint32_t);
static _Atomic int survey_enabled;
_Atomic int runtime_survey_pending;
/* The frame-end sync, read by hle_nvn.c (built once for all versions): *(sys+[0]), then +[1] through one more pointer if
   [1] >= 0. 1.4.3's frame controller (main+0x2db94c) first waits on *(*(sys+0x150)+0x78), found with TOTK_SURVEY_DEBUG. */
#if defined(TOTK_GAME_V143)
const int32_t runtime_survey_sync[2] = {0x150, 0x78};
#else
const int32_t runtime_survey_sync[2] = {0x1d8, -1};
#endif
static pthread_mutex_t survey_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int kind; char tag[64]; float xyz[3]; unsigned hold; } request;
static unsigned hold_left;
static float hold_xyz[3];
/* survey fly (benchmark fly-through, 2026-10-20): from where the player is to xyz over fly_total frames, smoothed. */
static unsigned fly_total;
static float fly_from[3], fly_to[3];
static char hold_tag[64];
static _Thread_local int in_tick;
/* Noclip: the same hold as "survey move", every frame while on, at a point the player steers (runtime_noclip_*). */
static void (*noclip_force)(void *, const float *, uint32_t);
static _Atomic int noclip_wanted;
static _Atomic uint32_t noclip_speed_bits = 0x41400000;   /* 12.0f m/s */
static struct { int holding; float xyz[3]; uint64_t at_ns; } noclip;   /* frame-end thread only */
static pthread_mutex_t noclip_goto_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int pending; float xyz[3]; } noclip_goto;   /* a teleport for the next tick (runtime_noclip_goto) */
static _Atomic int as_active;   /* auto shrine is running (below, before runtime_survey_tick) */
/* A teleport is a flight: a far place is not loaded, and a player held there is treated as fallen out of the world (the
   game fades out and puts him back where he last stood, 1.4.3, 2026-10-26). Flying there at a speed the streaming keeps
   up with loads the world on the way, as for a fast glide. Frame-end thread.
   The way is up, across and down, never through the ground (inside the terrain the player is also put back): up to
   NOCLIP_FLIGHT_CLEAR metres above the higher end, level to above the target, then straight down. */
enum { NOCLIP_FLIGHT_SPEED = 120, NOCLIP_FLIGHT_CLEAR = 60 };   /* metres a second, metres */
static struct { int on; float to[3], cruise; } noclip_flight;
enum { NOCLIP_UP = 1 << 9, NOCLIP_DOWN = 1 << 8 };   /* npad ZR, ZL: the triggers, the same on every controller layout */
enum { NOCLIP_SLOWER = 1 << 6, NOCLIP_FASTER = 1 << 7 };   /* npad L, R: the shoulder buttons */
void dm_gfx_camera_want(int on);   /* nvn_mtl.m: the game camera of each frame, for flying relative to it */
int dm_gfx_camera(float pos[3], float right[3], uint64_t *frame);
/* survey route (NX Optimizer's benchmark, 2026-10-20): one scene of UltraCam's benchmark, played as UltraCam plays it
   (subsdk3 0x9a250, 0xa0b30, 0xa4f40; reversed from the player's own NX Optimizer files). At the start Link is put 5 m above the scene's start and the camera waits at the first keyframe for
   10 s (from when Link is no more than 2 m above the start; re-teleported if he is more than 25 m away), the HUD hidden.
   An "hour H" line sets the time of day to H:00 at the start, as UltraCam does (see clock_find_step). Then the camera
   flies through the keyframes in real time: each keyframe is a time T, an eye, a view direction, an up and a vertical
   FOV; between two keyframes the frames are blended linearly, with the last and first quarter of each segment blended
   into its neighbours, and the last segment eased out and stretched 1.5 times. Through the look-at override the 1.4.3
   patches install in survey sessions (runtime_bench_cam, NULL elsewhere). File: "start X Y Z", "hour H", then
   "k T PX PY PZ FX FY FZ UX UY UZ FOV" per keyframe (FOV in radians). */
uint8_t *runtime_bench_cam;
enum { ROUTE_MAX = 64 };
typedef struct { float pos[3], fwd[3], up[3], fov; } CamFrame;
typedef struct { float t; CamFrame f; } RouteKey;
static RouteKey route[ROUTE_MAX], route_next[ROUTE_MAX];
static unsigned route_n, route_next_n;
static float route_start[3], route_next_start[3];
static int route_hour, route_next_hour;
static double route_last;
static float route_t, route_wait, route_clock0;   // route_clock0: the time of day when measuring began (-1: no clock)
static int route_on;   // 1 waiting at the first keyframe, 2 flying (measured)
/* "freeze T" lines in the route file (screenshots to compare with another run): the camera flies as usual and holds at
   each T for 5 s, reporting route_frozen pose=<n> after 3 s there, then flies on. */
enum { FREEZE_MAX = 64 };
static float route_freeze[FREEZE_MAX], route_next_freeze[FREEZE_MAX], route_held;
static unsigned route_freezes, route_next_freezes, route_pose;
static float route_holdfor = 5, route_next_holdfor = 5;   // "holdfor S": seconds at each freeze (debugging sessions park longer)
static int route_frozen;

static int read_mem(uintptr_t address, void *out, size_t size) {
    if (!address || address > UINTPTR_MAX - size) return 0;
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size, (uintptr_t)out, &got) == KERN_SUCCESS && got == size;
}
static uintptr_t read_ptr(uintptr_t address) { uintptr_t p = 0; return read_mem(address, &p, sizeof p) ? p : 0; }
static int valid_xyz(const float xyz[3]) {
    return isfinite(xyz[0]) && isfinite(xyz[1]) && isfinite(xyz[2]) &&
           fabsf(xyz[0]) <= 5500 && xyz[1] >= -1500 && xyz[1] <= 3500 && fabsf(xyz[2]) <= 5000;
}
static double now_seconds(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
/* Block +0 turns the camera override on, +104 hides the HUD (1.2.1; as UltraCam's AutoHideUI in its benchmark). */
static void cam_off(void) {
    if (!runtime_bench_cam) return;
    atomic_store((_Atomic uint32_t *)(void *)runtime_bench_cam, 0);
    atomic_store((_Atomic uint32_t *)(void *)(runtime_bench_cam + 104), 0);
}
static void norm3(float v[3]) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) for (int i = 0; i < 3; i++) v[i] /= l;
}
static void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1]; out[1] = a[2] * b[0] - a[0] * b[2]; out[2] = a[0] * b[1] - a[1] * b[0];
}
/* UltraCam's frame blend (a4f40 + a3fb0): component-wise lerp, the basis rebuilt from the view direction, FOV lerped in
   degrees and kept within 5..160. */
static CamFrame cam_blend(float x, const CamFrame *a, const CamFrame *b) {
    CamFrame o; float up0[3], right[3];
    for (int k = 0; k < 3; k++) {
        o.pos[k] = a->pos[k] + (b->pos[k] - a->pos[k]) * x;
        o.fwd[k] = a->fwd[k] + (b->fwd[k] - a->fwd[k]) * x;
        up0[k] = a->up[k] + (b->up[k] - a->up[k]) * x;
    }
    norm3(o.fwd); cross3(up0, o.fwd, right); norm3(right); cross3(o.fwd, right, o.up); norm3(o.up);
    float da = a->fov * 57.29578f, deg = da + (b->fov * 57.29578f - da) * x;
    o.fov = deg > 160 ? 2.7925f : fmaxf(deg, 5) * 0.0174533f;
    return o;
}
static float ease_last(float x) { return 1.0f - (float)pow(1.0 - x / 1.5, 1.55); }
static float route_length(void) { return route[route_n - 2].t + 1.5f * (route[route_n - 1].t - route[route_n - 2].t); }
/* UltraCam's benchmark tick (9a250): the frame at t seconds into the flight. */
static CamFrame route_sample(float t) {
    unsigned n = route_n, k = 0;
    for (; k + 1 < n; k++) {
        const float a = route[k].t, b = route[k + 1].t;
        if ((a <= t && t <= b) || (k == n - 2 && a + (t - a) / 1.5f <= b)) break;
    }
    if (k + 1 >= n) k = n - 2;
    const RouteKey *K = route;
    float u = (t - K[k].t) / (K[k + 1].t - K[k].t);
    int last = k + 1 == n - 1;
    if (u < 0.25f && k > 0) {
        float h = 0.5f * u + 0.125f;
        CamFrame p = cam_blend(h + 0.75f, &K[k - 1].f, &K[k].f), q = cam_blend(last ? ease_last(h) : h, &K[k].f, &K[k + 1].f);
        return cam_blend(2 * u + 0.5f, &p, &q);
    }
    if (u > 0.75f && !last) {
        float g = 0.5f * u - 0.375f;
        CamFrame p = cam_blend(0.5f * u + 0.375f, &K[k].f, &K[k + 1].f),
                 q = cam_blend(k + 1 == n - 2 ? ease_last(g) : g, &K[k + 1].f, &K[k + 2].f);
        return cam_blend(0.5f - 2 * (1 - u), &p, &q);
    }
    return cam_blend(last ? ease_last(u) : u, &K[k].f, &K[k + 1].f);
}
/* The target is one metre along the view direction, as UltraCam's camera hook (a6a40) puts it. */
static void cam_set(const CamFrame *c) {
    uint8_t *b = runtime_bench_cam;
    double eye[3], at[3]; float f[6];
    for (int k = 0; k < 3; k++) { eye[k] = c->pos[k]; at[k] = c->pos[k] + c->fwd[k]; f[k] = (float)eye[k]; f[3 + k] = (float)at[k]; }
    memcpy(b + 8, eye, sizeof eye); memcpy(b + 32, at, sizeof at); memcpy(b + 56, c->up, 12); memcpy(b + 72, f, sizeof f);
    memcpy(b + 96, &c->fov, 4);
    atomic_store((_Atomic uint32_t *)(void *)b, 1);
    atomic_store((_Atomic uint32_t *)(void *)(b + 104), 1);
}
/* Reads a route file into route_next; 0 with a message on stderr when it is malformed. */
static int route_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[survey] rejected: cannot open route %s\n", path); return 0; }
    char line[512]; int have_start = 0; route_next_n = 0; route_next_hour = -1; route_next_freezes = 0; route_next_holdfor = 5;
    while (fgets(line, sizeof line, f)) {
        RouteKey k;
        if (sscanf(line, "start %f %f %f", &route_next_start[0], &route_next_start[1], &route_next_start[2]) == 3) have_start = 1;
        else if (sscanf(line, "hour %d", &route_next_hour) == 1 && route_next_hour >= 0 && route_next_hour <= 23) {}   // the scene's time of day
        else if (sscanf(line, "holdfor %f", &route_next_holdfor) == 1 && route_next_holdfor >= 3) {}
        else if (route_next_freezes < FREEZE_MAX && sscanf(line, "freeze %f", &route_next_freeze[route_next_freezes]) == 1 &&
                 route_next_freeze[route_next_freezes] >= 0 &&
                 (!route_next_freezes || route_next_freeze[route_next_freezes] > route_next_freeze[route_next_freezes - 1]))
            route_next_freezes++;
        else if (sscanf(line, "k %f %f %f %f %f %f %f %f %f %f %f", &k.t, &k.f.pos[0], &k.f.pos[1], &k.f.pos[2], &k.f.fwd[0],
                        &k.f.fwd[1], &k.f.fwd[2], &k.f.up[0], &k.f.up[1], &k.f.up[2], &k.f.fov) == 11 && route_next_n < ROUTE_MAX &&
                 valid_xyz(k.f.pos) && k.f.fov > 0 && k.f.fov < 3 && (!route_next_n || k.t > route_next[route_next_n - 1].t))
            route_next[route_next_n++] = k;
        else if (line[0] != '\n' && line[0] != '#') { fclose(f); fprintf(stderr, "[survey] rejected: bad route line: %s", line); return 0; }
    }
    fclose(f);
    if (!have_start || !valid_xyz(route_next_start) || route_next_n < 2) {
        fprintf(stderr, "[survey] rejected: route needs a start and 2+ keyframes\n"); return 0;
    }
    return 1;
}
static void survey_debug_layout(uintptr_t module, uintptr_t scene) {
    static unsigned reports;
    if (!getenv("TOTK_SURVEY_DEBUG") || reports++ >= 3) return;
    fprintf(stderr, "[survey-layout] module=%#llx scene=%#llx\n", (unsigned long long)module, (unsigned long long)scene);
    int count = 0; read_mem(scene + 0x50, &count, 4);
    uintptr_t components = read_ptr(scene + 0x58);
    fprintf(stderr, "[survey-layout] components=%#llx count=%d\n", (unsigned long long)components, count);
    if (count < 1 || count > 256) return;
    for (int i = 0; i < count; ++i) {
        uintptr_t c = read_ptr(components + i * 8), ref = read_ptr(c + 8);
        char name[200] = {0}; read_mem(ref, name, sizeof name - 1);
        int n = 0; read_mem(c + 0x20, &n, 4); uintptr_t links = read_ptr(c + 0x28);
        fprintf(stderr, "[survey-layout] component=%d ptr=%#llx ref=%.199s count=%d links=%#llx\n", i, (unsigned long long)c, name, n, (unsigned long long)links);
        if (i != 13 || n < 1 || n > 64) continue;
        for (int j = 0; j < n; ++j) {
            uintptr_t data = read_ptr(links + j * 0x70 + 8), actor = read_ptr(data + 0x40), actor_name = read_ptr(actor + GAME_ACTOR_NAME);
            char text[64] = {0}; read_mem(actor_name, text, sizeof text - 1);
            fprintf(stderr, "[survey-layout] link=%d data=%#llx actor=%#llx name=%.63s\n", j, (unsigned long long)data, (unsigned long long)actor, text);
        }
    }
}
static uintptr_t player_actor(void) {
    uintptr_t module = read_ptr(survey_main + SCENE_SLOT);
    uintptr_t scene = module ? read_ptr(module + 0x1e8) : 0;
    survey_debug_layout(module, scene);
    int count = 0;
    if (!scene || !read_mem(scene + 0x50, &count, 4) || count < 14 || count > 256) return 0;
    uintptr_t components = read_ptr(scene + 0x58);
    uintptr_t resident = components ? read_ptr(components + 13 * 8) : 0;
    if (!resident || !read_mem(resident + 0x20, &count, 4) || count < 1 || count > 64) return 0;
    uintptr_t links = read_ptr(resident + 0x28);
    if (!links) return 0;
    for (int i = 0; i < count; ++i) {
        uintptr_t data = read_ptr(links + (uintptr_t)i * 0x70 + 8);
        uintptr_t actor = data ? read_ptr(data + 0x40) : 0;
        uintptr_t name = actor ? read_ptr(actor + GAME_ACTOR_NAME) : 0;
        char text[7];
        if (name && read_mem(name, text, sizeof text) && !memcmp(text, "Player", 7)) {
            float xyz[3];
            if (read_mem(actor + ACTOR_POS, xyz, sizeof xyz) && valid_xyz(xyz)) return actor;
        }
    }
    return 0;
}
void runtime_survey_init(Module *m) {
#if defined(TOTK_GAME_V143)
    const uint32_t expected[] = {0xa9bc7bfd, 0xa9015ff8, 0xa90257f6, 0xa9034ff4};   // 1.4.3 inlines the physics call: larger frame
#else
    const uint32_t expected[] = {0xa9bd7bfd, 0xf9000bf5, 0x910003fd, 0xa9024ff4};
#endif
    int force_ok = !strcmp(m->name, "main") && FORCE_MATRIX + sizeof expected <= m->text_end && !memcmp(m->base + FORCE_MATRIX, expected, sizeof expected);
    /* The actor spawner (actor_spawn.c, 1.4.3) and noclip use this frame-end tick in every session: they need the player,
       found through the scene slot, and noclip the forceSetMatrix entry checked above. pos / move stay behind TOTK_SURVEY. */
    if (!strcmp(m->name, "main") && SCENE_SLOT + 8u <= m->size && runtime_spawn_init(m)) {
        survey_main = (uintptr_t)m->base;
        if (force_ok) noclip_force = (void (*)(void *, const float *, uint32_t))(m->base + FORCE_MATRIX);
        fprintf(stderr, "[noclip] %s\n", force_ok ? "available (Cheats tab)" : "[red] off: the forceSetMatrix entry differs from " TOTK_GAME_VERSION);
    }
    const char *enabled = getenv("TOTK_SURVEY");
    if (!enabled || strcmp(enabled, "1")) return;
    const char *owner = getenv("TOTK_SLOT_OWNER"), *profile = getenv("TOTK_PROFILE_DIR");
    char target[PATH_MAX], user[PATH_MAX];
    if (!owner || !strcmp(owner, "user") || !profile || !realpath(profile, target) ||
        (realpath("analysis/phase6-profile", user) && !strcmp(target, user)))
        runtime_fail("TOTK_SURVEY requires an agent-owned session and a separate test profile");
    if (strcmp(m->name, "main") || SCENE_SLOT + 8u > m->size || !force_ok)
        runtime_fail("survey requires verified game-version forceSetMatrix entry");
    survey_main = (uintptr_t)m->base;
    survey_force = (void (*)(void *, const float *, uint32_t))(m->base + FORCE_MATRIX);
    survey_enabled = 1;
    fprintf(stderr, "[survey] enabled: test profile, game frame-end commands, original forceSetMatrix\n");
    runtime_spawn_allow_commands();   /* the input file's "spawn" lines, next to "survey" (actor_spawn.c) */
}
static void report(const char *tag, const char *status, uint64_t frame, uintptr_t actor) {
    float xyz[3];
    if (actor && read_mem(actor + ACTOR_POS, xyz, sizeof xyz) && valid_xyz(xyz))
        fprintf(stderr, "[survey] tag=%s status=%s frame=%llu actor=%#llx xyz=%.6f,%.6f,%.6f\n",
                tag, status, (unsigned long long)frame, (unsigned long long)actor, xyz[0], xyz[1], xyz[2]);
    else fprintf(stderr, "[survey] tag=%s status=%s frame=%llu actor=0\n", tag, status, (unsigned long long)frame);
}
/* survey clockscan <lo> <hi> / survey clockscan2 <dmin> <dmax> (diagnostic): floats in the guest's writable memory
   between lo and hi, then those of them that changed by dmin..dmax since; finds the time of day by its rate. */
static struct { uintptr_t at; float v; } *scan_hits;
static size_t scan_n;
static void clock_scan(float lo, float hi, int second, float dmin, float dmax) {
    enum { MAXHITS = 1 << 22, DENSE = 50000 };
    if (!second) { free(scan_hits); scan_hits = malloc(sizeof *scan_hits * MAXHITS); scan_n = 0; }
    if (!scan_hits) return;
    if (second) {
        size_t kept = 0;
        for (size_t i = 0; i < scan_n; i++) {
            float v; if (!read_mem(scan_hits[i].at, &v, 4)) continue;
            float d = v - scan_hits[i].v;
            if (d >= dmin && d <= dmax) {
                if (kept < 40) fprintf(stderr, "[survey] clock hit %#llx %.3f -> %.3f\n", (unsigned long long)scan_hits[i].at, scan_hits[i].v, v);
                scan_hits[kept].at = scan_hits[i].at; scan_hits[kept++].v = v;
            }
        }
        fprintf(stderr, "[survey] clockscan2: %zu of %zu changed by %.2f..%.2f\n", kept, scan_n, dmin, dmax);
        scan_n = kept; return;
    }
    const char *ef = getenv("TOTK_CLOCKSCAN_FROM"), *et = getenv("TOTK_CLOCKSCAN_TO");   // diagnostic: other address ranges
    mach_vm_address_t addr = ef ? strtoull(ef, NULL, 16) : 0x7000000000ULL, end = et ? strtoull(et, NULL, 16) : 0x8000000000ULL;
    size_t regions = 0, bytes = 0, dense = 0;
    while (addr < end && scan_n < MAXHITS) {
        mach_vm_size_t size = 0; natural_t depth = 1; vm_region_submap_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS) break;
        if (addr >= end) break;
        if ((info.protection & (VM_PROT_READ | VM_PROT_WRITE)) == (VM_PROT_READ | VM_PROT_WRITE) && !info.is_submap) {
            regions++; bytes += size;
            const float *f = (const float *)(uintptr_t)addr;
            size_t first = scan_n;
            for (size_t i = 0; i < size / 4 && scan_n < MAXHITS; i++) {
                float v = f[i];
                if (v >= lo && v <= hi) { scan_hits[scan_n].at = (uintptr_t)(f + i); scan_hits[scan_n++].v = v; }
            }
            if (scan_n - first > DENSE) { dense++; scan_n = first; }   // a buffer full of such values (pictures, pools): not a clock
        }
        addr += size;
    }
    fprintf(stderr, "[survey] clockscan: %zu floats in %.4f..%.4f over %zu regions, %zu MB (%zu dense regions left out)\n", scan_n, lo, hi, regions, bytes >> 20, dense);
}
/* The time of day (seconds, a float the game advances by 60 a second) has no fixed address: the first route with an
   "hour" needs it found first, while the player stands in the world (survey clockfind; the clock stops while an area
   loads, as after a teleport): any float in 1..24 h in the guest's memory, then twice more, 2 s apart, only those. The clock is the largest group of values that advanced by 40..130 a second and agree
   within a minute, preferring the group with the game's own clock (+0xc0f7b0 / +0xd1d358 in its heap block in 1.2.1):
   the clock and the copies made for drawing. (It runs at about 80 a second in the recomp, 60 on a Switch.) Each scene
   then writes its hour into them at its start, as UltraCam does (b1910); not found = no hour set, reported. */
enum { CLOCK_MAX = 16 };
static uintptr_t clock_at[CLOCK_MAX];
static unsigned clock_n;
static int clock_state;   // 3 once searched (found or not)
static void clock_write(int hour) {
    for (unsigned i = 0; i < clock_n; i++) *(volatile float *)clock_at[i] = (float)(hour * 3600);
    if (clock_n) fprintf(stderr, "[survey] time of day %02d:00 (%u clock values)\n", hour, clock_n);
}
/* Runs on the input-file thread (survey clockfind) while the game runs on: the frame thread must not stall for the scan. */
static void clock_find(void) {
    clock_scan(3600, 86000, 0, 0, 0);
    double t0 = now_seconds(); usleep(2500000);
    double dt = now_seconds() - t0;
    clock_scan(0, 0, 1, (float)(dt * 20), (float)(dt * 110));   // the slow full scan read each value at a different time
    t0 = now_seconds(); usleep(2000000); dt = now_seconds() - t0;
    size_t kept = 0;
    for (size_t i = 0; i < scan_n; i++) {   // both readings fast: the rate is exact
        float v; if (!read_mem(scan_hits[i].at, &v, 4)) continue;
        float rate = (float)((v - scan_hits[i].v) / dt);
        if (getenv("TOTK_CLOCK_DEBUG") && v > 18000 && v < 32000) fprintf(stderr, "[survey] clock candidate %#llx %.1f rate %.2f/s\n", (unsigned long long)scan_hits[i].at, v, rate);
        if (rate >= 40 && rate <= 130) { scan_hits[kept].at = scan_hits[i].at; scan_hits[kept++].v = v; }
    }
    size_t best = 0, best_i = 0;
    for (size_t i = 0; i < kept; i++) {   // the group holding the game's own clock (at these offsets in 1.2.1), else the largest
        size_t n = 0;
        unsigned low = (unsigned)(scan_hits[i].at & 0xffffff);
        for (size_t j = 0; j < kept; j++) if (fabsf(scan_hits[j].v - scan_hits[i].v) <= 60) n++;
        /* The game's clock in 1.2.1 sits 0xd4428 into a 1 MB-aligned heap block (7 values, 2026-10-21: the runs whose
           light matched citron); other timers of the same rate (9 or 16 values) give the wrong sky. */
        if ((scan_hits[i].at & 0xfffff) == 0xd4428) n += 2000;
        if (low == 0xc0f7b0 || low == 0xd1d358) n += 1000;
        if (n > best) { best = n; best_i = i; }
    }
    pthread_mutex_lock(&survey_lock);
    clock_n = 0;
    for (size_t j = 0; j < kept && clock_n < CLOCK_MAX; j++)
        if (best && fabsf(scan_hits[j].v - scan_hits[best_i].v) <= 60) clock_at[clock_n++] = scan_hits[j].at;
    clock_state = 3;
    pthread_mutex_unlock(&survey_lock);
    for (unsigned i = 0; i < clock_n; i++) fprintf(stderr, "[survey] clock value at %#llx\n", (unsigned long long)clock_at[i]);
    fprintf(stderr, "[survey] time of day: %zu values advanced like the clock, %u agree (%.0f s)%s\n", kept, clock_n,
            best ? scan_hits[best_i].v : 0.0f, clock_n ? "" : "; scene hours are not set");
    free(scan_hits); scan_hits = NULL; scan_n = 0;
}
int runtime_survey_command(const char *line) {
    if (strncmp(line, "survey ", 7)) return 0;
    { float x, y;
      if (sscanf(line, "survey clockscan2 %f %f", &x, &y) == 2) { clock_scan(0, 0, 1, x, y); return 1; }
      if (!strncmp(line, "survey clockfind", 16)) { clock_find(); return 1; }   // the clock for route hours (about 6 s)
      { unsigned long long pa; unsigned pn;   // diagnostic: survey peek <hex address> <bytes>, words and module offsets
        if (sscanf(line, "survey peek %llx %u", &pa, &pn) == 2 && pn <= 4096) {
            for (unsigned o = 0; o < pn; o += 8) { uint64_t w = 0; if (!read_mem((uintptr_t)pa + o, &w, 8)) break;
                const char *nm = NULL; uint64_t off = 0; extern int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off);
                float f[2]; memcpy(f, &w, 8);
                if (runtime_module_offset(w, &nm, &off)) fprintf(stderr, "[peek] +%#x %016llx %s+%#llx\n", o, (unsigned long long)w, nm, (unsigned long long)off);
                else fprintf(stderr, "[peek] +%#x %016llx  %g %g\n", o, (unsigned long long)w, f[0], f[1]); }
            return 1; } }
      if (!strncmp(line, "survey clockwatch", 17)) {   // diagnostic: log the game code that writes the clock (gfx_bridge.c watch)
          extern void runtime_watch_set(uintptr_t addr, size_t len);
          for (unsigned i = 0; i < clock_n; i++) if ((clock_at[i] & 0xffffff) == 0xc0f7b0) { runtime_watch_set(clock_at[i], 4); return 1; }
          if (clock_n) runtime_watch_set(clock_at[0], 4);
          return 1; }
      if (sscanf(line, "survey clockset %f", &x) == 1) {   // the hits of the last clockscan2 set to x hours (seconds of the day)
          for (size_t i = 0; i < scan_n; i++) *(volatile float *)scan_hits[i].at = x * 3600;
          fprintf(stderr, "[survey] clockset %.2f h: %zu values\n", x, scan_n); return 1; }
      if (sscanf(line, "survey clockscan %f %f", &x, &y) == 2) { clock_scan(x, y, 0, 0, 0); return 1; } }
    if (!strncmp(line, "survey clocks", 13)) {   // diagnostic: the objects main+0xab92cc was last called with, and their +0x20..+0x3c
        if (!runtime_bench_cam) return 1;
        uintptr_t seen[16]; unsigned n = 0, calls = 0;
        read_mem((uintptr_t)runtime_bench_cam + 108, &calls, 4);
        for (unsigned i = 0; i < 16; i++) {
            uintptr_t p = 0; read_mem((uintptr_t)runtime_bench_cam + 128 + i * 8, &p, 8);
            unsigned k = 0; while (k < n && seen[k] != p) k++;
            if (!p || k < n) continue;
            seen[n++] = p; float f[8] = {0}; read_mem(p + 0x20, f, sizeof f);
            fprintf(stderr, "[survey] clock candidate %#llx calls=%u +0x20..: %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f\n",
                    (unsigned long long)p, calls, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
        }
        return 1;
    }
    char op[16], tag[64]; float xyz[3] = {0}; unsigned hold = 0; int used = 0, kind = 0;
    if (sscanf(line, "survey %15s %63s %n", op, tag, &used) == 2 && used && !line[used] && !strcmp(op, "pos")) kind = 1;
    used = 0;
    if (sscanf(line, "survey move %63s %f %f %f %u %n", tag, &xyz[0], &xyz[1], &xyz[2], &hold, &used) == 5 &&
        used && !line[used] && valid_xyz(xyz) && hold >= 1 && hold <= 600) kind = 2;
    used = 0;
    if (!kind && sscanf(line, "survey fly %63s %f %f %f %u %n", tag, &xyz[0], &xyz[1], &xyz[2], &hold, &used) == 5 &&
        used && !line[used] && valid_xyz(xyz) && hold >= 1 && hold <= 36000) kind = 3;
    char path[1024];
    used = 0;
    if (!kind && sscanf(line, "survey route %63s %1023s %n", tag, path, &used) == 2 && used && !line[used]) kind = 4;
    if (!kind) { fprintf(stderr, "[survey] rejected: expected pos <tag>, move <tag> X Y Z <1..600 frames>, fly <tag> X Y Z <1..36000 frames> or route <tag> <file>\n"); return 1; }
    for (const char *p = tag; *p; ++p) if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') {
        fprintf(stderr, "[survey] rejected: invalid tag\n"); return 1;
    }
    pthread_mutex_lock(&survey_lock);
    if (!survey_enabled) report(tag, "disabled", 0, 0);
    else if (request.kind || hold_left || route_on) report(tag, "busy", 0, 0);
    else if (kind == 4 && !runtime_bench_cam) report(tag, "no_camera", 0, 0);
    else if (kind == 4 && !route_load(path)) report(tag, "bad_route", 0, 0);
    else {
        request.kind = kind; strcpy(request.tag, tag); memcpy(request.xyz, xyz, sizeof xyz); request.hold = hold;
        atomic_store_explicit(&runtime_survey_pending, 1, memory_order_release);
    }
    pthread_mutex_unlock(&survey_lock);
    return 1;
}
void runtime_autoshrine_cancel_user(void);
void runtime_survey_cancel_user(void) {
    runtime_autoshrine_cancel_user();   /* a run of auto shrine stops too (world_survey.c, below) */
    if (!atomic_load_explicit(&runtime_survey_pending, memory_order_relaxed)) return;
    pthread_mutex_lock(&survey_lock);
    request.kind = 0; hold_left = 0; route_on = 0; cam_off(); atomic_store(&runtime_survey_pending, 0);
    if (atomic_load(&runtime_spawn_waiting) || atomic_load(&noclip_wanted) || atomic_load(&as_active)) atomic_store(&runtime_survey_pending, 1);   /* spawns, noclip and auto shrine (its own stop) are not cancelled here */
    fprintf(stderr, "[survey] cancelled: user input\n");
    pthread_mutex_unlock(&survey_lock);
}
int runtime_noclip_available(void) { return noclip_force != NULL; }
int runtime_noclip_on(void) { return atomic_load(&noclip_wanted); }
void runtime_noclip_set(int on) {
    if (on && !noclip_force) return;
    atomic_store(&noclip_wanted, !!on);
    dm_gfx_camera_want(on);
    if (on) atomic_store_explicit(&runtime_survey_pending, 1, memory_order_release);   /* the tick runs every frame while on */
    fprintf(stderr, "[noclip] %s\n", on ? "on" : "off");
}
void runtime_noclip_set_speed(float v) {
    if (!(v >= 1 && v <= 200)) return;
    uint32_t bits; memcpy(&bits, &v, 4); atomic_store(&noclip_speed_bits, bits);
}
int runtime_noclip_goto(const float xyz[3]) {
    if (!noclip_force || !valid_xyz(xyz)) return 0;
    pthread_mutex_lock(&noclip_goto_lock);
    memcpy(noclip_goto.xyz, xyz, sizeof noclip_goto.xyz); noclip_goto.pending = 1;
    pthread_mutex_unlock(&noclip_goto_lock);
    fprintf(stderr, "[noclip] teleport to %.1f,%.1f,%.1f\n", xyz[0], xyz[1], xyz[2]);
    runtime_noclip_set(1);   /* held there while the world streams in; switching noclip off lands */
    return 1;
}
float runtime_noclip_speed(void) { uint32_t bits = atomic_load(&noclip_speed_bits); float v; memcpy(&v, &bits, 4); return v; }
const int runtime_noclip_speeds[NOCLIP_NSPEEDS] = {5, 12, 25, 50, 100};
void runtime_noclip_step_speed(int dir) {
    int at = 0; while (at < NOCLIP_NSPEEDS - 1 && runtime_noclip_speeds[at] < (int)lroundf(runtime_noclip_speed())) at++;
    at += dir; if (at < 0) at = 0; if (at > NOCLIP_NSPEEDS - 1) at = NOCLIP_NSPEEDS - 1;
    runtime_noclip_set_speed((float)runtime_noclip_speeds[at]);
}
uint64_t runtime_noclip_taken(void) { return atomic_load(&noclip_wanted) ? NOCLIP_UP | NOCLIP_DOWN | NOCLIP_SLOWER | NOCLIP_FASTER : 0; }
/* Frame-end thread: the first frame with a player takes his position; after that he is set to the held point, which the
   left stick moves relative to the camera (up: away from it, right: to the screen's right; level, whatever the camera's
   pitch) and the triggers raise (ZR / RT) and lower (ZL / LT). Without a camera yet, his facing. Gravity and collisions
   act between frames and are undone by the next hold. */
static void noclip_tick(uintptr_t actor) {
    if (!atomic_load(&noclip_wanted) || !noclip_force || !actor) {
        if (noclip.holding) fprintf(stderr, "[noclip] released at %.1f,%.1f,%.1f\n", noclip.xyz[0], noclip.xyz[1], noclip.xyz[2]);
        noclip.holding = 0;
        if (!atomic_load(&noclip_wanted)) noclip_flight.on = 0;
        return;
    }
    float rot[9], matrix[12];
    uint64_t now = runtime_now_ns();
    if (!read_mem(actor + ACTOR_ROT, rot, sizeof rot)) return;
    for (unsigned i = 0; i < 9; i++) if (!isfinite(rot[i]) || fabsf(rot[i]) > 2) return;
    if (!noclip.holding) {
        if (!read_mem(actor + ACTOR_POS, noclip.xyz, sizeof noclip.xyz) || !valid_xyz(noclip.xyz)) return;
        noclip.holding = 1; noclip.at_ns = now;
        fprintf(stderr, "[noclip] holding from %.1f,%.1f,%.1f\n", noclip.xyz[0], noclip.xyz[1], noclip.xyz[2]);
    }
    pthread_mutex_lock(&noclip_goto_lock);
    if (noclip_goto.pending) {
        memcpy(noclip_flight.to, noclip_goto.xyz, sizeof noclip_flight.to); noclip_flight.on = 1; noclip_goto.pending = 0;
        noclip_flight.cruise = fmaxf(noclip.xyz[1], noclip_flight.to[1]) + NOCLIP_FLIGHT_CLEAR;
    }
    pthread_mutex_unlock(&noclip_goto_lock);
    float dt = (float)(now - noclip.at_ns) * 1e-9f;
    noclip.at_ns = now;
    if (!(dt > 0)) dt = 0;
    if (dt > 0.1f) dt = 0.1f;   /* a pause or a menu */
    int32_t sx = 0, sy = 0;
    runtime_input_left_stick(&sx, &sy);
    float push = sqrtf((float)sx * sx + (float)sy * sy) / 32767.0f;
    if (push < 0.15f) push = 0; else if (push > 1) push = 1;
    float speed = runtime_noclip_speed(), mx = 0, mz = 0, cam[3], right[3];
    uint64_t cam_frame = 0;
    if (push > 0 && dm_gfx_camera(cam, right, &cam_frame)) {
        float fx = noclip.xyz[0] - cam[0], fz = noclip.xyz[2] - cam[2], fl = sqrtf(fx * fx + fz * fz);
        float rx = right[0], rz = right[2], rl = sqrtf(rx * rx + rz * rz);
        if (fl > 1e-3f && rl > 1e-3f) {
            fx /= fl; fz /= fl; rx /= rl; rz /= rl;
            float k = push / (sqrtf((float)sx * sx + (float)sy * sy) / 32767.0f);   /* the stick's direction at the dead-zoned length */
            mx = (fx * sy + rx * sx) / 32767.0f * k; mz = (fz * sy + rz * sx) / 32767.0f * k;
            static int said;
            if (!said++) fprintf(stderr, "[noclip] camera: forward %.2f,%.2f right %.2f,%.2f (cross %.2f: +1 when the right axis is forward x up)\n", fx, fz, rx, rz, -fz * rx + fx * rz);
        }
    } else if (push > 0) {   /* no camera yet: where he faces */
        float fx = rot[2], fz = rot[8], len = sqrtf(fx * fx + fz * fz);
        if (len > 1e-3f) { mx = fx / len * push; mz = fz / len * push; }
    }
    uint64_t buttons = runtime_input_buttons();
    static uint64_t buttons_before;
    uint64_t pressed = buttons & ~buttons_before; buttons_before = buttons;
    if (pressed & (NOCLIP_SLOWER | NOCLIP_FASTER)) {
        runtime_noclip_step_speed(pressed & NOCLIP_FASTER ? 1 : -1);
        fprintf(stderr, "[noclip] speed %.0f m/s\n", runtime_noclip_speed());
    }
    float up = (buttons & NOCLIP_UP ? 1.0f : 0.0f) - (buttons & NOCLIP_DOWN ? 1.0f : 0.0f);
    float next[3] = {noclip.xyz[0] + mx * speed * dt, noclip.xyz[1] + up * speed * dt, noclip.xyz[2] + mz * speed * dt};
    if (noclip_flight.on) {   /* flying to a teleport target; the stick or the triggers end it */
        float hx = noclip_flight.to[0] - noclip.xyz[0], hz = noclip_flight.to[2] - noclip.xyz[2], across = sqrtf(hx * hx + hz * hz);
        float step = NOCLIP_FLIGHT_SPEED * dt, *at = noclip.xyz;
        memcpy(next, at, sizeof next);
        if (push > 0 || up != 0) { noclip_flight.on = 0; fprintf(stderr, "[noclip] flight stopped by the player, %.0f m short\n", across); }
        else if (across > 0.5f && at[1] < noclip_flight.cruise - 0.5f) next[1] = fminf(at[1] + step, noclip_flight.cruise);   /* up */
        else if (across > 0.5f) {   /* across */
            float k = fminf(step, across) / across; next[0] = at[0] + hx * k; next[2] = at[2] + hz * k;
        } else {   /* down */
            next[0] = noclip_flight.to[0]; next[2] = noclip_flight.to[2];
            float dy = noclip_flight.to[1] - at[1];
            if (fabsf(dy) <= step) { next[1] = noclip_flight.to[1]; noclip_flight.on = 0; fprintf(stderr, "[noclip] arrived at %.1f,%.1f,%.1f\n", next[0], next[1], next[2]); }
            else next[1] = at[1] + (dy > 0 ? step : -step);
        }
    }
    if (valid_xyz(next)) memcpy(noclip.xyz, next, sizeof next);   /* the map's edges stop it */
    for (unsigned row = 0; row < 3; row++) {
        for (unsigned col = 0; col < 3; col++) matrix[row * 4 + col] = rot[row * 3 + col];
        matrix[row * 4 + 3] = noclip.xyz[row];
    }
    noclip_force((void *)actor, matrix, 0);
}
/* Puts the player at xyz with his current rotation (forceSetMatrix); 0 when the rotation does not read back sane. */
static int force_at(uintptr_t actor, const float xyz[3]) {
    float rot[9] = {0}, matrix[12];
    if (!read_mem(actor + ACTOR_ROT, rot, sizeof rot)) return 0;
    for (unsigned i = 0; i < 9; i++) if (!isfinite(rot[i]) || fabsf(rot[i]) > 2) return 0;
    for (unsigned row = 0; row < 3; row++) {
        for (unsigned col = 0; col < 3; col++) matrix[row * 4 + col] = rot[row * 3 + col];
        matrix[row * 4 + 3] = xyz[row];
    }
    survey_force((void *)actor, matrix, 0);
    return 1;
}

/* ---- Auto shrine (Tools › Shrines; shrine_auto.h has what the game's files say): one shrine after another, the nearest
   first. For each: noclip to a spot in front of the entrance, set down facing in; the gate's A while it has never been
   opened; then Link walks in with the left stick (the way in starts only when he walks through the doorway, not when he
   is set down there); inside, noclip to the altar, A; the game plays the blessing, marks the shrine Clear and takes him
   out. Any cutscene on the way is skipped where the game allows it (AS_SCENE: the skip button twice, then B for a
   message; the game's first viewing of some cannot be skipped and is waited out). If the altar's scene ends with Link
   still inside, he walks into the start lift. Back outside, the autosave that follows must say Clear. Every press is a
   synthetic key (input.c), which never counts as the player's own input. Readiness: the shrine's and the surface's map
   files opening (romfs.c calls runtime_autoshrine_romfs), Link's position, and whether he answers the stick (a short
   push that moves him: he is in control; none: a cutscene, a message or a load holds him). Each step has a time limit;
   on any miss the run stops with Link set down where he last stood. A press of the player's own (input.c user_input)
   stops it at once. Frame-end thread. ---- */
size_t runtime_romfs_file_count(void);
const char *runtime_romfs_file_name(size_t i);
uint8_t *runtime_romfs_host_read(const char *path, size_t *n);
uint8_t *runtime_romfs_zs_decompress(const uint8_t *src, size_t n, size_t *out);
void runtime_input_key_synthetic(unsigned code, int down);
int settings_get(const char *key);
enum { AS_KEY_A = 38, AS_KEY_B = 40, AS_KEY_X = 34, AS_KEY_PLUS = 36, AS_KEY_ZL = 58, AS_KEY_UP = 13, AS_KEY_DOWN = 1, AS_KEY_LEFT = 0, AS_KEY_RIGHT = 2 };   /* Mac key codes (input.c button(); WASD is the left stick) */
/* The skip button: + (the "Skip" prompt, EventSkip_00, shows at the lower right), X as some guides say, or none
   ("autoshrine skip plus | x | none"). */
static _Atomic int as_skip_key = AS_KEY_PLUS;
static ShrineInfo as_shrine[SHRINES];
static Landmark as_mark[LANDMARKS];   /* names, Great Sky Island and the Depths for the story lock (landmarks.h) */
static _Atomic int as_loaded;         /* 0 not read, 1 reading, 2 read, 3 not this game */
static _Atomic int as_request;        /* 1 run, 2 one shrine, 3 stop: for the frame-end thread */
static _Atomic int as_want = -1;      /* with 2: that shrine (runtime_autoshrine_finish), -1 the nearest */
static _Atomic int as_chests, as_cancel;
static _Atomic uint32_t as_seq_inside, as_seq_field, as_seq_goal, as_seq_event;   /* RomFS opens: a shrine's map, the surface's map, the goal's event, any event flow */
static _Atomic int as_inside_index = -1, as_watch_field;   /* watch_field: log the next surface map file */
static pthread_mutex_t as_status_lock = PTHREAD_MUTEX_INITIALIZER;
static char as_status[600] = "Not started";
enum { AS_OFF, AS_PICK, AS_MOVE, AS_GATE, AS_SCENE, AS_WALK, AS_LOADING, AS_INSIDE, AS_CHEST, AS_GOAL, AS_BACK, AS_SURFACE, AS_SAVED, AS_STOPPING };
static const char *const AS_STEP_TEXT[] = {"stopped", "choosing the next shrine", "flying", "opening the gate", "skipping a cutscene", "walking in",
                                           "waiting for the shrine to load", "inside", "opening a chest", "at the altar", "walking into the start lift",
                                           "back outside", "waiting for the autosave", "setting Link down"};
static struct {
    int step, one, target, k, chest, done, after, tries, event, back, flight, phase, have_last, have_safe, keyn, inside;   /* inside: in the shrine's own map */
    int walk, until_field, skips;   /* walk: the WASD keys held (SHRINE_STICK_...); until_field / skips: AS_SCENE's */
    double started, t0, phase_t, last_tick, save_time;
    float stand[3], face[3], last[3], safe[3], probe[3];
    uint32_t seq_inside, seq_field, seq_goal;
    uint8_t state[SHRINES], run[SHRINES];
    char left_out[200], why[200];
    struct { unsigned code; double up; } key[4];
} as;
static void as_say(void) {
    char line[600]; const char *name = as.k >= 0 ? as_mark[LANDMARK_FIRST_SHRINE + as.k].name : "";
    if (as.step != AS_OFF) snprintf(line, sizeof line, "Running: %s%s%s · %d done%s%s", name, *name ? " · " : "", AS_STEP_TEXT[as.step], as.done,
                                    as.left_out[0] ? " · left out: " : "", as.left_out);
    else snprintf(line, sizeof line, "%s · %d done%s%s", as.why[0] ? as.why : "Stopped", as.done, as.left_out[0] ? " · left out: " : "", as.left_out);
    pthread_mutex_lock(&as_status_lock); snprintf(as_status, sizeof as_status, "%s", line); pthread_mutex_unlock(&as_status_lock);
}
static void as_key(unsigned code, double now, double seconds) {
    if (as.keyn >= 4) return;
    runtime_input_key_synthetic(code, 1); as.key[as.keyn].code = code; as.key[as.keyn++].up = now + seconds;
}
static void as_keys_tick(double now, int all) {
    for (int i = 0; i < as.keyn;)
        if (all || now >= as.key[i].up) { runtime_input_key_synthetic(as.key[i].code, 0); as.key[i] = as.key[--as.keyn]; }
        else i++;
}
static int as_pos(uintptr_t actor, float xyz[3]) { return actor && read_mem(actor + ACTOR_POS, xyz, 12) && valid_xyz(xyz); }
/* Link at xyz facing face (level): forceSetMatrix with a turn about the vertical; his local +z (the matrix's third column) is
   where he faces, as noclip reads it. */
static void as_force_facing(uintptr_t actor, const float xyz[3], const float face[3]) {
    float dx = face[0] - xyz[0], dz = face[2] - xyz[2], l = sqrtf(dx * dx + dz * dz), s = l > 1e-3f ? dx / l : 0, c = l > 1e-3f ? dz / l : 1;
    float m[12] = {c, 0, s, xyz[0], 0, 1, 0, xyz[1], -s, 0, c, xyz[2]};
    if (noclip_force && actor) noclip_force((void *)actor, m, 0);
}
/* A short push of the left stick (WASD) toward to, picked from the camera's view: he steps toward the prompt and faces it. */
static void as_nudge(uintptr_t actor, const float to[3], double now) {
    float cam[3], right[3], at[3]; uint64_t frame;
    if (!as_pos(actor, at) || !dm_gfx_camera(cam, right, &frame)) return;
    float fx = at[0] - cam[0], fz = at[2] - cam[2], fl = sqrtf(fx * fx + fz * fz), rl = sqrtf(right[0] * right[0] + right[2] * right[2]);
    float dx = to[0] - at[0], dz = to[2] - at[2], dl = sqrtf(dx * dx + dz * dz);
    if (fl < 1e-3f || rl < 1e-3f || dl < 1e-3f) return;
    float y = (dx * fx + dz * fz) / (dl * fl), x = (dx * right[0] + dz * right[2]) / (dl * rl);
    if (y > 0.38f) as_key(AS_KEY_UP, now, 0.3); else if (y < -0.38f) as_key(AS_KEY_DOWN, now, 0.3);
    if (x > 0.38f) as_key(AS_KEY_RIGHT, now, 0.3); else if (x < -0.38f) as_key(AS_KEY_LEFT, now, 0.3);
    fprintf(stderr, "[autoshrine] nudge toward %.1f,%.1f,%.1f (stick %.2f across, %.2f ahead)\n", to[0], to[1], to[2], x, y);
}
/* The left stick held toward to (WASD from the camera's view, renewed each frame); to NULL lets go. */
static void as_walk(uintptr_t actor, const float to[3]) {
    static const unsigned code[4] = {AS_KEY_UP, AS_KEY_DOWN, AS_KEY_LEFT, AS_KEY_RIGHT};
    int want = 0; float cam[3], right[3], at[3]; uint64_t frame;
    if (to && as_pos(actor, at)) {
        float fwd[2], rt[2], dir[2], cl;
        shrine_unit_xz(at, to, dir);
        if (dm_gfx_camera(cam, right, &frame) && (cl = sqrtf(right[0] * right[0] + right[2] * right[2])) > 1e-3f) {
            shrine_unit_xz(cam, at, fwd); rt[0] = right[0] / cl; rt[1] = right[2] / cl;
            want = shrine_stick(fwd, rt, dir);
        } else want = SHRINE_STICK_UP;   /* no camera: ahead, as he was turned */
    }
    for (int i = 0; i < 4; i++)
        if ((want ^ as.walk) & (1 << i)) runtime_input_key_synthetic(code[i], !!(want & (1 << i)));
    as.walk = want;
}
static double as_save_time(void) {
    const char *profile = getenv("TOTK_PROFILE_DIR"); char path[1800]; struct stat st;
    if (!profile) return 0;
    map_pins_newest_save(profile, path, sizeof path);
    return path[0] && !stat(path, &st) ? st.st_mtimespec.tv_sec + st.st_mtimespec.tv_nsec * 1e-9 : 0;
}
/* Each shrine's DungeonState from the newest save, and the story lock into lm; 0 when there is no save. */
static int as_read_save(Landmark *lm) {
    const char *profile = getenv("TOTK_PROFILE_DIR"); char path[1800]; int got = 0;
    memset(as.state, SHRINE_STATE_UNKNOWN, sizeof as.state);
    memcpy(lm, as_mark, sizeof as_mark);
    LandmarkProgress progress = {0, 0, 0};
    if (profile && landmarks_state_read(profile, lm, &progress) >= 0) {
        map_pins_newest_save(profile, path, sizeof path);
        FILE *f = path[0] ? fopen(path, "rb") : NULL;
        uint8_t *b = f ? malloc(4 << 20) : NULL; size_t n = b ? fread(b, 1, 4 << 20, f) : 0;
        if (f) fclose(f);
        got = b ? shrine_states_parse(b, n, as.state) : 0;
        free(b);
    }
    int lock = settings_get("teleport.story_lock");
    landmarks_lock(lm, &progress, lock != 0);   /* on unless switched off (-1: no such setting) */
    return got;
}
static void as_step(int step, double now) {
    as_walk(0, NULL);
    as.step = step; as.t0 = now; as.phase = 0; as.tries = 0; as.have_last = 0; as.event = 0;
    as.seq_inside = atomic_load(&as_seq_inside); as.seq_field = atomic_load(&as_seq_field); as.seq_goal = atomic_load(&as_seq_goal);
    atomic_store(&as_watch_field, step == AS_GOAL || step == AS_BACK);
    if (step != AS_OFF && step != AS_STOPPING)
        fprintf(stderr, "[autoshrine] %s%s%s\n", as.k >= 0 ? as_mark[LANDMARK_FIRST_SHRINE + as.k].name : "", as.k >= 0 ? ": " : "", AS_STEP_TEXT[step]);
    as_say();
}
static void as_move(const float stand[3], const float face[3], int after, double now) {
    memcpy(as.stand, stand, 12); memcpy(as.face, face, 12); as.after = after; as.flight = 0;
    as_step(AS_MOVE, now);
}
/* The run ends: ok (the last one done, or nothing left) or why it stopped. Link is set down where he last stood, unless
   the player stopped it (then noclip stays as it is, for him to steer). */
static void as_end(int ok, const char *why, double now, int by_player) {
    as_keys_tick(now, 1); as_walk(0, NULL);
    snprintf(as.why, sizeof as.why, "%s%s", ok ? "Finished: " : "Stopped: ", why);
    fprintf(stderr, "[autoshrine] %s (%d done)\n", as.why, as.done);
    if (!ok && as.k >= 0) as.run[as.k] = SHRINE_SKIP_FAILED;
    if (!by_player && atomic_load(&noclip_wanted) && as.have_safe) {   /* back to where he last stood: a flight outside, a straight line inside */
        float at[3] = {as.safe[0], as.safe[1] + 0.5f, as.safe[2]};
        if (!as.inside && noclip.holding && landmark_distance(noclip.xyz, at) > 40) runtime_noclip_goto(at);
        memcpy(as.stand, at, 12); as_step(AS_STOPPING, now); return;
    }
    if (!by_player && atomic_load(&noclip_wanted)) runtime_noclip_set(0);
    if (!atomic_load(&noclip_wanted)) dm_gfx_camera_want(0);
    as.step = AS_OFF; atomic_store(&as_active, 0); as_say();
}
static void as_fail(const char *what, double now) {
    char why[200]; const char *name = as.k >= 0 ? as_mark[LANDMARK_FIRST_SHRINE + as.k].name : "";
    snprintf(why, sizeof why, "%s%s%s", name, *name ? ": " : "", what);
    as_end(0, why, now, 0);
}
/* A press cycle at a prompt (every period seconds, at most tries cycles): a nudge toward the prompt from the second cycle,
   ZL (the camera behind Link), then A. Returns 0 once the cycles are used up. */
static int as_press_cycle(uintptr_t actor, const float prompt[3], double now, double period, int tries) {
    double t = now - as.t0 - as.tries * period;
    if (as.tries >= tries) return 0;
    if (as.phase == 0) { if (as.tries) as_nudge(actor, prompt, now); as.phase = 1; }
    else if (as.phase == 1 && t >= 0.6) { as_key(AS_KEY_ZL, now, 0.15); as.phase = 2; }
    else if (as.phase == 2 && t >= 1.1) { as_key(AS_KEY_A, now, 0.15); fprintf(stderr, "[autoshrine] A (try %d)\n", as.tries + 1); as.phase = 3; }
    else if (as.phase == 3 && t >= period) { as.tries++; as.phase = 0; }
    return 1;
}
static void as_pick(uintptr_t actor, double now) {
    float at[3];
    if (!as_pos(actor, at)) { if (now - as.t0 > 30) as_end(0, "no player to start from", now, 0); return; }
    static Landmark lm[LANDMARKS]; int skip[SHRINES], count[SHRINE_SKIPS] = {0};
    int found = as_read_save(lm);
    for (int k = 0; k < SHRINES; k++) count[skip[k] = shrine_skip(&as_shrine[k], as.state[k], lm[LANDMARK_FIRST_SHRINE + k].lock, as.run[k])]++;
    size_t o = 0; as.left_out[0] = 0;
    for (int r = 1; r < SHRINE_SKIPS && o < sizeof as.left_out - 1; r++)
        if (count[r]) o += (size_t)snprintf(as.left_out + o, sizeof as.left_out - o, "%s%d %s", o ? ", " : "", count[r], SHRINE_SKIP_TEXT[r]);
    fprintf(stderr, "[autoshrine] %d shrines to do; %d DungeonState flags in the save; left out: %s\n", count[SHRINE_GO], found, as.left_out[0] ? as.left_out : "none");
    if (as.one && as.done) { as_end(1, "the shrine is done", now, 0); return; }
    if (as.target >= SHRINES) as.target = -1;
    if (as.target >= 0 && skip[as.target] != SHRINE_GO) {   /* the shrine chosen on the Shrines tab, which the run cannot do */
        char why[160]; snprintf(why, sizeof why, "%s is left out: %s", as_mark[LANDMARK_FIRST_SHRINE + as.target].name, SHRINE_SKIP_TEXT[skip[as.target]]);
        as.k = -1; as_end(0, why, now, 0); return;
    }
    int k = as.target >= 0 ? as.target : shrine_next(as_shrine, skip, at);
    if (k < 0) { as.k = -1; as_end(1, "no shrine left to do", now, 0); return; }
    as.k = k;
    const ShrineInfo *s = &as_shrine[k];
    fprintf(stderr, "[autoshrine] next: %s (Dungeon%03d, %s, %.0f m away, save says %s, %d chest%s)\n", as_mark[LANDMARK_FIRST_SHRINE + k].name, k,
            SHRINE_KIND_TEXT[s->kind], landmark_distance(s->entrance, at), as.state[k] ? SHRINE_STATE_NAMES[as.state[k]] : "nothing", s->nchests, s->nchests == 1 ? "" : "s");
    float stand[3], face[3];
    int gate = as.state[k] == SHRINE_STATE_APPEAR || as.state[k] == SHRINE_STATE_UNKNOWN;   /* never opened: its gate's A first */
    shrine_stand(s, SHRINE_AT_APPROACH, 0, stand, face);
    as_move(stand, face, gate ? AS_GATE : AS_WALK, now);
}
/* A cutscene (or a message, or a load) holds Link: each 3 s cycle first pushes the stick briefly (he moves: it is over),
   else presses the skip button twice (once shows the prompt, once skips; while the game does not offer a skip nothing
   happens) and B for a message. until_field: it ends when the surface's map loads instead (the altar's scene). */
static void as_scene(int after, int until_field, double now) {
    as.after = after; as.until_field = until_field; as.skips = 0;
    as_step(AS_SCENE, now);
}
static void as_after_inside(double now) {
    const ShrineInfo *s = &as_shrine[as.k]; float stand[3], face[3];
    if (atomic_load(&as_chests) && as.chest < s->nchests) { shrine_stand(s, SHRINE_AT_CHEST, as.chest, stand, face); as_move(stand, face, AS_CHEST, now); return; }
    shrine_stand(s, SHRINE_AT_GOAL, 0, stand, face); as_move(stand, face, AS_GOAL, now);
}
static void as_go_on(double now) {   /* AS_SCENE is over */
    if (as.after == AS_INSIDE) { as.chest = 0; as_after_inside(now); return; }
    as_step(as.after, now);
}
static void as_tick_move(uintptr_t actor, double now, double dt) {
    float at[3] = {0, 0, 0}; int have = as_pos(actor, at);
    if (as.phase == 0) {   /* noclip on: on the surface a flight (up, across, down) when far, else (and inside a shrine) a straight line */
        if (!have) { if (now - as.t0 > 20) as_fail("no player to move", now); return; }
        as_keys_tick(now, 1);   /* no key of ours held: noclip reads WASD and ZL as the player steering */
        float d = landmark_distance(at, as.stand); as.flight = d > 40 && !as.inside;
        if (as.flight) runtime_noclip_goto(as.stand); else runtime_noclip_set(1);
        as.phase_t = now + d / 100 + 30; as.phase = 1;   /* phase_t: the flight's time limit */
        fprintf(stderr, "[autoshrine] %s to %.1f,%.1f,%.1f (%.0f m)\n", as.flight ? "flight" : "straight", as.stand[0], as.stand[1], as.stand[2], d);
        return;
    }
    if (as.phase == 1) {
        if (now > as.phase_t) { as_fail("the flight did not arrive in time", now); return; }
        if (!noclip.holding) return;
        if (as.flight) {
            pthread_mutex_lock(&noclip_goto_lock); int pending = noclip_goto.pending; pthread_mutex_unlock(&noclip_goto_lock);
            if (pending || noclip_flight.on) return;
            if (landmark_distance(noclip.xyz, as.stand) > 1.5f) { as_fail("the flight stopped short", now); return; }
        } else {
            float d = landmark_distance(noclip.xyz, as.stand), step = 30 * (float)dt;
            if (d > step) { for (int c = 0; c < 3; c++) noclip.xyz[c] += (as.stand[c] - noclip.xyz[c]) * step / d; return; }
        }
        memcpy(noclip.xyz, as.stand, 12); as.phase = 2; as.phase_t = now;
        return;
    }
    if (as.phase == 2) {   /* held there half a second, turned to face the prompt, then set down */
        memcpy(noclip.xyz, as.stand, 12); as_force_facing(actor, as.stand, as.face);
        if (now - as.phase_t >= 0.6) { runtime_noclip_set(0); dm_gfx_camera_want(1); as.phase = 3; as.phase_t = now; }
        return;
    }
    if (as.phase == 3) {   /* landed: on his feet near the place */
        if (now - as.phase_t < 1.5) return;
        float dx = have ? at[0] - as.stand[0] : 99, dz = have ? at[2] - as.stand[2] : 99;
        if (!have || dx * dx + dz * dz > 16 || at[1] < as.stand[1] - 6) {
            fprintf(stderr, "[autoshrine] landed at %.1f,%.1f,%.1f, wanted %.1f,%.1f,%.1f\n", at[0], at[1], at[2], as.stand[0], as.stand[1], as.stand[2]);
            as_fail("Link did not land where he was set down", now); return;
        }
        memcpy(as.safe, at, 12); as.have_safe = 1;
        as_key(AS_KEY_ZL, now, 0.15);   /* the camera behind him, toward the prompt */
        as.phase = 4; as.phase_t = now;
        return;
    }
    if (now - as.phase_t >= 0.5) as_step(as.after, now);
}
static void autoshrine_tick(uintptr_t actor, double now) {
    int req = atomic_exchange(&as_request, 0);
    if (req == 3 && as.step != AS_OFF && as.step != AS_STOPPING) { as_end(0, "stopped from the menu or the input file", now, 0); return; }
    if ((req == 1 || req == 2) && as.step == AS_OFF) {
        int chests = atomic_load(&as_chests); memset(&as, 0, sizeof as); as.k = -1; as.one = req == 2; as.target = atomic_exchange(&as_want, -1); as.started = now; as.last_tick = now;
        atomic_store(&as_cancel, 0); dm_gfx_camera_want(1);
        fprintf(stderr, "[autoshrine] start: %s, chests %s\n", as.target >= 0 ? "one chosen shrine" : as.one ? "the nearest shrine" : "every shrine it can do, nearest first", chests ? "on" : "off");
        as_step(AS_PICK, now);
    }
    if (as.step == AS_OFF) return;
    double dt = now - as.last_tick; as.last_tick = now;
    if (dt > 0.1) dt = 0.1;
    as_keys_tick(now, 0);
    if (atomic_exchange(&as_cancel, 0) && now - as.started > 2 && as.step != AS_STOPPING) { as_end(0, "your own input (a key, a button or the stick)", now, 1); return; }
    double t = now - as.t0;
    const ShrineInfo *s = as.k >= 0 ? &as_shrine[as.k] : NULL;
    float at[3]; int have = as_pos(actor, at);
    switch (as.step) {
    case AS_PICK: as_pick(actor, now); return;
    case AS_MOVE: as_tick_move(actor, now, dt); return;
    case AS_GATE:   /* the gate's A: DmF_SY_SmallDungeon1Open (Link authenticates; skippable once seen before), then walk in */
        if (as_press_cycle(actor, s->entrance, now, 3, 1)) return;
        as_scene(AS_WALK, 0, now);
        return;
    case AS_SCENE: {
        if (as.until_field && (atomic_load(&as_seq_field) != as.seq_field || (have && landmark_distance(at, s->entrance) < 200))) {
            fprintf(stderr, "[autoshrine] the altar's scene took Link out (%d skip presses)\n", as.skips);
            as.back = 0; as.have_safe = 0; as.inside = 0; as_step(AS_SURFACE, now); return;
        }
        if (t > 180) { as_fail("a cutscene did not end within 3 minutes", now); return; }
        double c = t - 3.0 * as.tries; int key = atomic_load(&as_skip_key);
        if (as.phase == 0) {   /* a short push of the stick */
            if (!have) { if (c >= 3) as.tries++; return; }
            memcpy(as.probe, at, 12); as_key(AS_KEY_UP, now, 0.35); as.phase = 1;   /* ahead only: back could walk him into a doorway */
        } else if (as.phase == 1 && c >= 0.8) {
            float moved = have ? landmark_distance(at, as.probe) : 0;
            if (moved > 0.3f && (!as.until_field || t > 10)) {
                fprintf(stderr, "[autoshrine] Link answers the stick (%.1f m) after %.0f s, %d skip presses\n", moved, t, as.skips);
                if (as.until_field) {   /* the altar's scene is over and he is still inside: the start lift */
                    float stand[3], face[3]; shrine_stand(s, SHRINE_AT_BACK_APPROACH, 0, stand, face);
                    fprintf(stderr, "[autoshrine] still inside after the altar; walking into the start lift\n");
                    as_move(stand, face, AS_BACK, now); return;
                }
                as_go_on(now); return;
            }
            if (key) { as_key((unsigned)key, now, 0.15); as.skips++; }
            as.phase = 2;
        } else if (as.phase == 2 && c >= 1.4) { if (key) { as_key((unsigned)key, now, 0.15); as.skips++; } as.phase = 3; }
        else if (as.phase == 3 && c >= 2.1) { as_key(AS_KEY_B, now, 0.1); as.phase = 4; }
        else if (as.phase == 4 && c >= 3) { as.tries++; as.phase = 0; }
        return;
    }
    case AS_WALK: {   /* through the doorway: DmF_SY_SmallDungeon2Go takes him onto the lift and into the shrine's map */
        uint32_t seen = as.seq_inside;   /* AS_LOADING looks for the map files from the same point */
        if (atomic_load(&as_seq_inside) != seen || (have && landmark_distance(at, s->start) < 60)) { as_step(AS_LOADING, now); as.seq_inside = seen; return; }
        float dx = have ? at[0] - s->lift_floor[0] : 99, dz = have ? at[2] - s->lift_floor[2] : 99;
        if (dx * dx + dz * dz < 1.5f * 1.5f) { fprintf(stderr, "[autoshrine] on the entrance lift after %.1f s\n", t); as_step(AS_LOADING, now); as.seq_inside = seen; return; }
        if (t > 8) {
            char why[160]; snprintf(why, sizeof why, "walked toward the entrance for 8 s and the way in did not start (%.1f m from the lift)", sqrtf(dx * dx + dz * dz));
            as_fail(why, now); return;
        }
        as_walk(actor, s->lift_floor);
        return;
    }
    case AS_LOADING:   /* 2Go: the fade, then the shrine's map */
        if (atomic_load(&as_seq_inside) != as.seq_inside || as.event) {
            int got = atomic_load(&as_inside_index);
            if (!as.event && got != as.k) { char why[80]; snprintf(why, sizeof why, "the game loaded Dungeon%03d instead", got); as_fail(why, now); return; }
            as.event = 1;
        }
        if (have && landmark_distance(at, s->start) < 80) {   /* he is in the shrine's own coordinates (444 m or more from any entrance) */
            fprintf(stderr, "[autoshrine] inside at %.1f,%.1f,%.1f%s\n", at[0], at[1], at[2], as.event ? "" : " (no map file seen)");
            as.have_safe = 0; as.inside = 1; as_step(AS_INSIDE, now); return;
        }
        if (t > (as.event ? 60 : 25)) as_fail(as.event ? "the shrine's map loaded but Link did not appear in it" : "the way in started but the shrine did not load", now);
        return;
    case AS_INSIDE:   /* 3Arrive (and on Great Sky Island the ability's scene): skipped, then on */
        as_scene(AS_INSIDE, 0, now);
        return;
    case AS_CHEST:   /* A, then B for the item's screen; no signal says it opened: best effort */
        if (as.phase == 0) { as_key(AS_KEY_A, now, 0.15); fprintf(stderr, "[autoshrine] chest %d of %d: A\n", as.chest + 1, s->nchests); as.phase = 1; }
        else if (as.phase < 4 && t >= 3.5 + (as.phase - 1) * 1.5) { as_key(AS_KEY_B, now, 0.1); as.phase++; }
        else if (as.phase == 4 && t >= 8) { as.chest++; as_after_inside(now); }
        return;
    case AS_GOAL:   /* A at the altar: DmF_SY_SmallDungeonGoal (warps him to it: he moves without the stick) */
        if (as.phase == 0 && !as.tries) as.save_time = as_save_time();
        if (atomic_load(&as_seq_field) != as.seq_field) { as.back = 0; as.have_safe = 0; as.inside = 0; as_step(AS_SURFACE, now); return; }
        if (!as.event && atomic_load(&as_seq_goal) != as.seq_goal) { as.event = 1; fprintf(stderr, "[autoshrine] the goal's event file opened\n"); }
        if (as.phase == 3 && have && !as.have_last) { memcpy(as.last, at, 12); as.have_last = 1; }   /* where he stood at the A */
        if (as.event || (as.phase == 3 && as.have_last && have && landmark_distance(at, as.last) > 0.6f)) {
            fprintf(stderr, "[autoshrine] the altar answered\n"); as_scene(AS_BACK, 1, now); return;
        }
        if (as.phase == 0) as.have_last = 0;
        if (!as_press_cycle(actor, s->goal_exit, now, 6, 4)) as_fail("the altar did not answer A (4 tries)", now);
        return;
    case AS_BACK: {   /* walk into the start lift: DmF_SY_SmallDungeon4Return */
        if (atomic_load(&as_seq_field) != as.seq_field || (have && landmark_distance(at, s->entrance) < 200)) { as.back = 1; as.have_safe = 0; as.inside = 0; as_step(AS_SURFACE, now); return; }
        float dx = have ? at[0] - s->back_floor[0] : 99, dz = have ? at[2] - s->back_floor[2] : 99;
        if (dx * dx + dz * dz < 1.5f * 1.5f || as.event) { as.event = 1; as_walk(0, NULL); if (t > 30) as_fail("on the start lift but the shrine did not let Link out", now); return; }
        if (t > 8) { as_fail("walked toward the start lift for 8 s without leaving", now); return; }
        as_walk(actor, s->back);
        return;
    }
    case AS_SURFACE:   /* DmF_SY_SmallDungeon5Out (or the island's story scenes): once he is back in the surface's coordinates */
        if (have && (landmark_distance(at, s->entrance) < 200 || landmark_distance(at, s->start) > 1000)) {
            fprintf(stderr, "[autoshrine] outside at %.1f,%.1f,%.1f, %.0f m from the entrance\n", at[0], at[1], at[2], landmark_distance(at, s->entrance));
            memcpy(as.safe, at, 12); as.have_safe = 1; as_scene(AS_SAVED, 0, now); return;
        }
        if (t > 120) as_fail("Link did not appear outside the shrine within 2 minutes", now);
        return;
    case AS_SAVED: {   /* the autosave after leaving: it must say Clear (a save from before the goal may still come first) */
        if (fmod(t, 1.0) >= dt && t < 40) return;
        double saved = as_save_time();
        const char *name = as_mark[LANDMARK_FIRST_SHRINE + as.k].name;
        if (saved > as.save_time) {
            static Landmark lm[LANDMARKS]; as_read_save(lm); as.save_time = saved;
            fprintf(stderr, "[autoshrine] autosave: DungeonState.Dungeon%03d = %s\n", as.k, as.state[as.k] ? SHRINE_STATE_NAMES[as.state[as.k]] : "(not in it)");
            as.event = as.state[as.k] ? as.state[as.k] : -1;   /* event: the last save's state of this shrine */
            if (as.state[as.k] == SHRINE_STATE_CLEAR) fprintf(stderr, "[autoshrine] done: %s (Dungeon%03d): the autosave says Clear\n", name, as.k);
            else if (t < 40) return;
        }
        if (as.event != SHRINE_STATE_CLEAR) {
            if (t < 40) return;
            if (as.event) {
                char why[120]; snprintf(why, sizeof why, "the autosave says %s, not Clear%s", as.event > 0 ? SHRINE_STATE_NAMES[as.event] : "nothing", as.back ? " (left by the start lift)" : "");
                as_fail(why, now); return;
            }
            if (as.back) { as_fail("left by the start lift and no autosave followed", now); return; }
            fprintf(stderr, "[autoshrine] done: %s (Dungeon%03d): no autosave within 40 s, so not confirmed\n", name, as.k);
        }
        as.run[as.k] = SHRINE_SKIP_DONE; as.done++;
        as_step(AS_PICK, now);
        return;
    }
    case AS_STOPPING:   /* back to where he last stood, then down */
        if (t < 60 && atomic_load(&noclip_wanted)) {
            pthread_mutex_lock(&noclip_goto_lock); int pending = noclip_goto.pending; pthread_mutex_unlock(&noclip_goto_lock);
            if (pending || noclip_flight.on || !noclip.holding) return;
            float d = landmark_distance(noclip.xyz, as.stand), step = 30 * (float)dt;
            if (d > step) { for (int c = 0; c < 3; c++) noclip.xyz[c] += (as.stand[c] - noclip.xyz[c]) * step / d; return; }
        }
        if (atomic_load(&noclip_wanted)) runtime_noclip_set(0);
        dm_gfx_camera_want(0);
        fprintf(stderr, "[autoshrine] set down\n");
        as.step = AS_OFF; atomic_store(&as_active, 0); as_say();
        return;
    }
}
/* Reads the shrines (and their names and story places) from the RomFS once, then asks the frame-end thread to start. */
static void *as_start_thread(void *arg) {
    int one = (int)(intptr_t)arg, expected = 0;
    if (atomic_compare_exchange_strong(&as_loaded, &expected, 1)) {
        LandmarkRomfs rom = {runtime_romfs_file_count, runtime_romfs_file_name, runtime_romfs_host_read, runtime_romfs_zs_decompress};
        double t0 = now_seconds();
        int places = landmarks_load(&rom, as_mark), ready = places ? shrines_load(&rom, as_shrine) : 0;
        fprintf(stderr, "[autoshrine] read %d shrines from the game's files (%.2f s)\n", ready, now_seconds() - t0);
        atomic_store(&as_loaded, ready ? 2 : 3);
    }
    while (atomic_load(&as_loaded) == 1) usleep(50000);
    if (atomic_load(&as_loaded) != 2) {
        fprintf(stderr, "[autoshrine] not started: the shrines are not in this game's files\n");
        pthread_mutex_lock(&as_status_lock); snprintf(as_status, sizeof as_status, "Not available: the shrines are not in this game's files"); pthread_mutex_unlock(&as_status_lock);
        atomic_store(&as_active, 0); return NULL;
    }
    atomic_store(&as_request, one ? 2 : 1);
    atomic_store_explicit(&runtime_survey_pending, 1, memory_order_release);
    return NULL;
}
int runtime_autoshrine_finish(int shrine) {
    if (shrine < 0 || shrine >= SHRINES || atomic_load(&as_active)) return 0;
    atomic_store(&as_want, shrine);
    if (runtime_autoshrine_start(1)) return 1;
    atomic_store(&as_want, -1); return 0;
}
int runtime_autoshrine_start(int one) {
    if (!noclip_force) { fprintf(stderr, "[autoshrine] not started: needs noclip (1.0.0 or 1.4.3)\n"); return 0; }
    int idle = 0;
    if (!atomic_compare_exchange_strong(&as_active, &idle, 1)) { fprintf(stderr, "[autoshrine] already running\n"); return 0; }
    pthread_mutex_lock(&as_status_lock); snprintf(as_status, sizeof as_status, "Starting: reading the shrines…"); pthread_mutex_unlock(&as_status_lock);
    pthread_t t;
    if (pthread_create(&t, NULL, as_start_thread, (void *)(intptr_t)one)) { atomic_store(&as_active, 0); return 0; }
    pthread_detach(t);
    return 1;
}
void runtime_autoshrine_stop(void) {
    if (!atomic_load(&as_active)) return;
    atomic_store(&as_request, 3); atomic_store_explicit(&runtime_survey_pending, 1, memory_order_release);
}
void runtime_autoshrine_set_chests(int on) { atomic_store(&as_chests, !!on); fprintf(stderr, "[autoshrine] chests %s\n", on ? "on" : "off"); }
int runtime_autoshrine_chests(void) { return atomic_load(&as_chests); }
int runtime_autoshrine_available(void) { return noclip_force != NULL; }
int runtime_autoshrine_status(char *text, size_t size) {
    pthread_mutex_lock(&as_status_lock); snprintf(text, size, "%s", as_status); pthread_mutex_unlock(&as_status_lock);
    return atomic_load(&as_active);
}
int runtime_autoshrine_command(const char *line) {
    if (strncmp(line, "autoshrine ", 11)) return 0;
    char word[16] = "", arg[8] = ""; sscanf(line + 11, "%15s %7s", word, arg);
    if (!strcmp(word, "start")) runtime_autoshrine_start(0);
    else if (!strcmp(word, "one")) runtime_autoshrine_start(1);
    else if (!strcmp(word, "stop")) runtime_autoshrine_stop();
    else if (!strcmp(word, "finish") && arg[0]) runtime_autoshrine_finish(atoi(arg));   /* "autoshrine finish N": Dungeon N */
    else if (!strcmp(word, "chests") && (!strcmp(arg, "on") || !strcmp(arg, "off"))) runtime_autoshrine_set_chests(arg[1] == 'n');
    else if (!strcmp(word, "skip") && (!strcmp(arg, "plus") || !strcmp(arg, "x") || !strcmp(arg, "none"))) {
        atomic_store(&as_skip_key, arg[0] == 'p' ? AS_KEY_PLUS : arg[0] == 'x' ? AS_KEY_X : 0); fprintf(stderr, "[autoshrine] skip button: %s\n", arg);
    }
    else if (!strcmp(word, "status")) { char s[600]; runtime_autoshrine_status(s, sizeof s); fprintf(stderr, "[autoshrine] status: %s\n", s); }
    else fprintf(stderr, "[autoshrine] rejected: expected start, one, finish N, stop, status, chests on|off or skip plus|x|none\n");
    return 1;
}
/* romfs.c, every file the game opens (path relative to the RomFS): which map is loading, and the goal's event. */
void runtime_autoshrine_romfs(const char *path) {
    if (!atomic_load_explicit(&as_active, memory_order_relaxed) || !path) return;
    if (!strncmp(path, "Banc/SmallDungeon/Dungeon", 25) && landmark_ends(path, "_Static.bcett.byml.zs")) {
        atomic_store(&as_inside_index, atoi(path + 25)); atomic_fetch_add(&as_seq_inside, 1);
        fprintf(stderr, "[autoshrine] the game opens %s\n", path);
    } else if (!strncmp(path, "Banc/MainField/", 15)) {
        atomic_fetch_add(&as_seq_field, 1);
        if (atomic_exchange(&as_watch_field, 0)) fprintf(stderr, "[autoshrine] the game opens %s (the first of the surface's map)\n", path);
    } else if (!strncmp(path, "Event/EventFlow/", 16)) {   /* a cutscene's flow (the first time it plays this run; later ones may be cached) */
        if (!strncmp(path + 16, "DmF_SY_SmallDungeonGoal", 23)) atomic_fetch_add(&as_seq_goal, 1);
        atomic_fetch_add(&as_seq_event, 1);
        fprintf(stderr, "[autoshrine] the game opens %s\n", path);
    }
}
void runtime_autoshrine_cancel_user(void) { if (atomic_load(&as_active)) atomic_store(&as_cancel, 1); }
void runtime_survey_tick(uint64_t frame) {
    if (in_tick || !atomic_load_explicit(&runtime_survey_pending, memory_order_acquire)) return;
    in_tick = 1;
    pthread_mutex_lock(&survey_lock);
    uintptr_t actor = player_actor();
    if (!request.kind && !hold_left) {
        /* only spawns waiting (actor_spawn.c) */
    } else if (!actor) {
        report(request.kind ? request.tag : hold_tag, "no_player", frame, 0);
        request.kind = 0; hold_left = 0; route_on = 0; cam_off();
    } else if (request.kind == 1) {
        report(request.tag, "position", frame, actor); request.kind = 0;
    } else {
        float drop[3] = {route_start[0], route_start[1] + 5, route_start[2]}, at[3];
        if (request.kind == 4) {   // route (UltraCam's a0b30): time of day, Link 5 m above the start, the camera at the first keyframe
            memcpy(route, route_next, sizeof route); route_n = route_next_n; memcpy(route_start, route_next_start, sizeof route_start);
            route_hour = route_next_hour; memcpy(route_freeze, route_next_freeze, sizeof route_freeze); route_freezes = route_next_freezes;
            route_pose = 0; route_held = 0; route_frozen = 0; route_holdfor = route_next_holdfor; drop[0] = route_start[0]; drop[1] = route_start[1] + 5; drop[2] = route_start[2];
            strcpy(hold_tag, request.tag); request.kind = 0; route_on = 1; route_wait = 10; route_last = now_seconds();
            if (route_hour >= 0) { if (clock_state == 3) clock_write(route_hour); else fprintf(stderr, "[survey] time of day not set: survey clockfind first\n"); }
            if (!force_at(actor, drop)) { report(hold_tag, "invalid_rotation", frame, actor); route_on = 0; }
            else report(hold_tag, "route_begin", frame, actor);
        }
        if (route_on) {
            double now = now_seconds(); float dt = (float)(now - route_last); route_last = now;
            if (route_on == 1) {   // wait 10 s at the first keyframe once Link has landed (UltraCam's bench+0xa4)
                cam_set(&route[0].f);
                if (read_mem(actor + ACTOR_POS, at, sizeof at) && valid_xyz(at)) {
                    float dx = at[0] - route_start[0], dy = at[1] - route_start[1], dz = at[2] - route_start[2];
                    if (dx * dx + dy * dy + dz * dz > 25 * 25) force_at(actor, drop);
                    if (at[1] > route_start[1] + 2) route_wait = 10;
                    else route_wait -= dt;
                }
                if (route_wait <= 0) {
                    route_on = 2; route_t = 0; report(hold_tag, "route_measure", frame, actor);
                    route_clock0 = route_hour >= 0 && clock_n ? *(volatile float *)clock_at[0] : -1;
                }
            } else {
                route_t += dt;
                if (route_pose < route_freezes && route_t >= route_freeze[route_pose]) {
                    route_t = route_freeze[route_pose]; route_held += dt;
                    if (route_held >= 3 && !route_frozen) {
                        route_frozen = 1;
                        fprintf(stderr, "[survey] tag=%s status=route_frozen pose=%u frame=%llu\n", hold_tag, route_pose, (unsigned long long)frame);
                    }
                    if (route_held >= route_holdfor) { route_pose++; route_held = 0; route_frozen = 0; }
                }
                /* The time of day follows the flight's own clock (a game minute per second, as an uninterrupted flight
                   has it), so a pose held for its screenshots shows the light of that moment of the flight. */
                if (route_clock0 >= 0)
                    for (unsigned i = 0; i < clock_n; i++) *(volatile float *)clock_at[i] = route_clock0 + 60 * route_t;
                if (route_t >= route_length()) { route_on = 0; cam_off(); report(hold_tag, "route_done", frame, actor); }
                else { CamFrame c = route_sample(route_t); cam_set(&c); }
            }
        }
        if (request.kind == 2) {
            memcpy(hold_xyz, request.xyz, sizeof hold_xyz); strcpy(hold_tag, request.tag);
            hold_left = request.hold; request.kind = 0; fly_total = 0;
            report(hold_tag, "move_begin", frame, actor);
        } else if (request.kind == 3) {   // fly: start where the player is now
            if (!read_mem(actor + ACTOR_POS, fly_from, sizeof fly_from) || !valid_xyz(fly_from)) memcpy(fly_from, request.xyz, sizeof fly_from);
            memcpy(fly_to, request.xyz, sizeof fly_to); strcpy(hold_tag, request.tag);
            hold_left = fly_total = request.hold; request.kind = 0;
            report(hold_tag, "fly_begin", frame, actor);
        }
        if (hold_left && fly_total) {   // this frame's point on the path (smoothstep: no jolt at either end)
            float t = 1.0f - (float)(hold_left - 1) / (float)fly_total, e = t * t * (3.0f - 2.0f * t);
            for (int i = 0; i < 3; i++) hold_xyz[i] = fly_from[i] + (fly_to[i] - fly_from[i]) * e;
        }
        if (hold_left) {
            if (force_at(actor, hold_xyz)) {
                if (!--hold_left) { report(hold_tag, fly_total ? "fly_done" : "move_done", frame, actor); fly_total = 0; }
            } else { report(hold_tag, "invalid_rotation", frame, actor); hold_left = 0; }
        }
    }
    int survey_left = request.kind || hold_left || route_on;
    pthread_mutex_unlock(&survey_lock);
    if (!survey_left) { noclip_tick(actor); autoshrine_tick(actor, now_seconds()); }   /* a scripted move wins */
    runtime_spawn_tick(actor, frame);   /* outside the survey lock: it calls into the game */
    /* cleared before the spawn queue is looked at again, so a spawn queued meanwhile (it sets waiting, then pending) is
       never lost */
    atomic_store(&runtime_survey_pending, survey_left);
    if (atomic_load(&runtime_spawn_waiting) || atomic_load(&noclip_wanted) || atomic_load(&as_active)) atomic_store(&runtime_survey_pending, 1);
    in_tick = 0;
}
#endif   /* GAME_BOTW */
