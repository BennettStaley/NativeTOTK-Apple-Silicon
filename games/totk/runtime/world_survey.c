/* Live map survey for the selected TOTK version. No arbitrary memory writes and no raw position pokes.
   Offset leads: aquacluck/totk-lotuskit 59dcfcc; independently checked against our main in
   analysis/continuous-survey-entry-named.c. forceSetMatrix routes through the physics component,
   or queues a next transform, unlike merely changing the actor's displayed position.
   Only called after the actual game frame-end GPU wait, never from the input file thread. */
#include "world_survey.h"
#include "actor_spawn.h"
#include "game_version.h"
#include "shrine_auto.h"
#include "korok_auto.h"
#include <ctype.h>
#include <errno.h>
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
int runtime_noclip_jump(const float xyz[3]) { (void)xyz; return 0; }
int runtime_player_pos(float xyz[3]) { (void)xyz; return 0; }
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
int runtime_autoshrine_given_up_reset(void) { return 0; }
int runtime_autoshrine_command(const char *line) {   /* "ctl state | peek | ...": TOTK only; the scripted pad (input.c) works here */
    if (strncmp(line, "ctl ", 4)) return 0;
    fprintf(stderr, "[ctl] rejected: game state and movement commands are TOTK only: %s", line); return 1;
}
void runtime_autoshrine_romfs(const char *path) { (void)path; }
int runtime_autokorok_available(void) { return 0; }   /* auto korok: the TOTK koroks only */
int runtime_autokorok_start(int one) { (void)one; return 0; }
int runtime_autokorok_only(uint64_t hash) { (void)hash; return 0; }
void runtime_autokorok_stop(void) { }
void runtime_autokorok_set_type(int type, int on) { (void)type; (void)on; }
int runtime_autokorok_type_on(int type) { (void)type; return 0; }
int runtime_autokorok_status(char *text, size_t size) { if (size) snprintf(text, size, "Not available in this game"); return 0; }
int runtime_autokorok_counts(int *found, int *total, int types) { for (int t = 0; t < types; t++) found[t] = total[t] = 0; return -1; }
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
static _Atomic uint64_t noclip_guard_until_ns;   /* noclip_guard (1.4.3); read on the input thread too: the frame tick keeps running until then */
/* After noclip: Link falls free and stays guarded until he has stopped (noclip_guard). on: not stopped yet. */
static struct { _Atomic int on; int still; float y; uint64_t t0; } noclip_settle;
static pthread_mutex_t noclip_goto_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int pending, direct; float xyz[3]; } noclip_goto;   /* a teleport for the next tick (runtime_noclip_goto) */
static _Atomic int as_active;   /* auto shrine is running (below, before runtime_survey_tick) */
static _Atomic int ak_active, ak_no_fall;   /* auto korok is running; ak_no_fall: noclip_guard leaves the falls alone (a korok's dive) */
#if defined(TOTK_GAME_V143)
static void event_init(Module *m);   /* "ctl event" (below, by the other game-control commands) */
#endif
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
#if defined(TOTK_GAME_V143)
        event_init(m);
#endif
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
void runtime_autokorok_cancel_user(void);
void runtime_survey_cancel_user(void) {
    runtime_autoshrine_cancel_user();   /* a run of auto shrine stops too (world_survey.c, below) */
    runtime_autokorok_cancel_user();    /* and of auto korok */
    if (!atomic_load_explicit(&runtime_survey_pending, memory_order_relaxed)) return;
    pthread_mutex_lock(&survey_lock);
    request.kind = 0; hold_left = 0; route_on = 0; cam_off(); atomic_store(&runtime_survey_pending, 0);
    if (atomic_load(&runtime_spawn_waiting) || atomic_load(&noclip_wanted) || noclip.holding || runtime_now_ns() < atomic_load(&noclip_guard_until_ns) || atomic_load(&as_active) || atomic_load(&ak_active)) atomic_store(&runtime_survey_pending, 1);   /* spawns, noclip (and its landing), auto shrine and auto korok (their own stops) are not cancelled here */
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
    memcpy(noclip_goto.xyz, xyz, sizeof noclip_goto.xyz); noclip_goto.pending = 1; noclip_goto.direct = 0;
    pthread_mutex_unlock(&noclip_goto_lock);
    fprintf(stderr, "[noclip] teleport to %.1f,%.1f,%.1f\n", xyz[0], xyz[1], xyz[2]);
    runtime_noclip_set(1);   /* held there while the world streams in; switching noclip off lands */
    return 1;
}
int runtime_noclip_jump(const float xyz[3]) {
    if (!noclip_force || !valid_xyz(xyz)) return 0;
    pthread_mutex_lock(&noclip_goto_lock);
    memcpy(noclip_goto.xyz, xyz, sizeof noclip_goto.xyz); noclip_goto.pending = 1; noclip_goto.direct = 1;
    pthread_mutex_unlock(&noclip_goto_lock);
    fprintf(stderr, "[noclip] teleport straight to %.1f,%.1f,%.1f\n", xyz[0], xyz[1], xyz[2]);
    runtime_noclip_set(1);
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
/* No soft landing (2026-10-28): lowering him while held never saw him stand (the hold leaves physics nothing to act on)
   and pushed him into the ground three times. Switched off, he is let go where he is and falls as the game has him fall;
   noclip_guard keeps him alive and resets the fall until he has stopped. noclip_land stays for auto korok's resets. */
static struct { int on; } noclip_land;
static void noclip_release(const char *why) {
    if (noclip.holding) fprintf(stderr, "[noclip] released at %.1f,%.1f,%.1f%s%s\n", noclip.xyz[0], noclip.xyz[1], noclip.xyz[2], why ? ": " : "", why ? why : "");
    noclip.holding = 0; noclip_land.on = 0;
}
static void noclip_tick(uintptr_t actor) {
    if (!atomic_load(&noclip_wanted) || !noclip_force || !actor) {
        noclip_release(atomic_load(&noclip_wanted) ? NULL : "falls free, guarded until he stops");
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
        if (noclip_goto.direct) {   /* straight there: held at the place from this frame (the world streams in around him) */
            memcpy(noclip.xyz, noclip_goto.xyz, sizeof noclip.xyz); noclip_flight.on = 0;
            fprintf(stderr, "[noclip] put at %.1f,%.1f,%.1f\n", noclip.xyz[0], noclip.xyz[1], noclip.xyz[2]);
        } else {
            memcpy(noclip_flight.to, noclip_goto.xyz, sizeof noclip_flight.to); noclip_flight.on = 1;
            noclip_flight.cruise = fmaxf(noclip.xyz[1], noclip_flight.to[1]) + NOCLIP_FLIGHT_CLEAR;
        }
        noclip_goto.pending = 0;
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
   first, in the player's eight steps (below, before AS_OFF). Noclip only puts Link at the shrine's travel point (a direct
   move, 2026-10-29: there in one frame and held while the ground streams in, never the 120 m/s flight) and, inside, moves
   him to 2.5 m in front of the altar; every prompt is reached by walking (the left stick) and pressed with A, as a player
   does: the gate's activation, the doorway (walked through), the altar. Cutscenes are skipped where the game allows it
   (X shows "Skip +", then +; B for messages). Every press is a leased synthetic key (input.c) that lets go by itself and
   never counts as the player's own input. Readiness: the shrine's and the surface's map files and the goal's event file
   opening (romfs.c calls runtime_autoshrine_romfs), Link's position, and whether he answers the stick (a short push that
   moves him: he is in control; none: a cutscene, a message or a load holds him). A shrine is done only when the autosave
   says Clear or the altar's event was seen and took him out; only then is the next one chosen. Each step has a time
   limit; a miss the recoveries below do not fix gives that shrine up ("giving up on <name>: step N, ...") and the run
   goes on; when the run ends every key is let go, noclip goes off with Link set down on loaded ground, a sound plays. A press of the player's own (input.c user_input) stops it at once. Frame-end thread. ---- */
size_t runtime_romfs_file_count(void);
const char *runtime_romfs_file_name(size_t i);
uint8_t *runtime_romfs_host_read(const char *path, size_t *n);
uint8_t *runtime_romfs_zs_decompress(const uint8_t *src, size_t n, size_t *out);
void runtime_input_key_synthetic(unsigned code, int down);
int settings_get(const char *key);
enum { AS_KEY_A = 38, AS_KEY_B = 40, AS_KEY_X = 34, AS_KEY_ZL = 58, AS_KEY_UP = 13, AS_KEY_DOWN = 1, AS_KEY_LEFT = 0, AS_KEY_RIGHT = 2 };   /* Mac key codes (input.c button(); WASD is the left stick) */
/* Skipping: the shrine scenes show "Skip X" (EventSkip_00, lower right; "Skip Y" with Xbox prompts, the same button), and
   X skips (in-game test 2026-10-28, the gate's scene). + is never pressed: once a scene is over it opens the pause menu.
   X in play only jumps; a B follows every other try (it advances a message). as_skip_reveal: the skip button
   ("autoshrine skip x | b | none"; none: no skip at all, cutscenes are waited out). */
static _Atomic int as_skip_reveal = AS_KEY_X;
static ShrineInfo as_shrine[SHRINES];
static Landmark as_mark[LANDMARKS];   /* names, Great Sky Island and the Depths for the story lock (landmarks.h) */
static _Atomic int as_loaded;         /* 0 not read, 1 reading, 2 read, 3 not this game */
static _Atomic int as_request;        /* 1 run, 2 one shrine, 3 stop: for the frame-end thread */
static _Atomic int as_want = -1;      /* with 2: that shrine (runtime_autoshrine_finish), -1 the nearest */
static _Atomic int as_chests, as_cancel;
static _Atomic uint32_t as_seq_inside, as_seq_field, as_seq_goal, as_seq_event;   /* RomFS opens: a shrine's map, the surface's map, the goal's event, any event flow */
static _Atomic int as_inside_index = -1, as_watch_field;   /* watch_field: log the next surface map file */
static pthread_mutex_t as_status_lock = PTHREAD_MUTEX_INITIALIZER;
static char as_status[1600] = "Not started";
/* The player's eight steps (2026-10-28) name where the run is, in the status line, the log and every reason:
   1 go to location, 2 activate location, 3 enter location, 4 go to location exit, 5 exit location, 6 wait for the location
   exit to complete or skip it, 7 exit the location by walking, 8 go to the next location.
   Under them a state machine with recovery (2026-10-28). Each working state declares the outcome it waits for and a short
   time limit; when the outcome does not come it hands over to AS_DIAGNOSE (never straight to a failure), which reads the
   live signals (does Link move under a short push of the stick, did an event file or a map file open lately, did the
   game-over event open, where is he against where the state put him, is he at a prompt or on a lift) and picks a
   recovery: AS_R_MENU (B, up to 3), a move back with a longer hold (RETURNED, 4 s, once), AS_R_PROMPT (back off 2 m,
   from straight on then 30 degrees to either side, walk up, face, A; up to 3 inside, once outside), the lift's A again
   (ON_LIFT_NO_LOAD, up to 3 inside, once outside), AS_GAMEOVER (A on "Continue", the reload), AS_R_UNKNOWN (keys let go,
   B twice, noclip off, look again once). A recovery returns to the state that called it; when its count is used up the
   shrine is abandoned: before the entrance the next shrine is chosen, inside Link first walks out by the start lift
   (STUCK_INSIDE).
   Giving up fast and remembering it (2026-10-29, user: "give up on those that are closed or inaccessible"; some have
   sludge on them): outside, a blocked entrance gets one retry, a landing the game takes Link back from (it fades out
   and puts him where he stood) none; every shrine given up is written to autoshrine-given-up.txt (shrine_auto.h) and
   later runs leave it out until its DungeonState changes or the list is reset (Places › Shrines). The run goes on to
   the next nearest until none is left; only AS_FAILS_ALIKE give-ups in a row with the same reason (something wrong
   in general, not with one shrine), no player, or a game over that does not go away stop it. */
enum { AS_OFF, AS_PICK, AS_MOVE, AS_GATE, AS_SCENE, AS_WALK, AS_LIFT, AS_LOADING, AS_INSIDE, AS_CHEST, AS_GOAL, AS_LEAVE, AS_BACK,
       AS_SURFACE, AS_SAVED, AS_DIAGNOSE, AS_R_MENU, AS_R_PROMPT, AS_R_UNKNOWN, AS_GAMEOVER, AS_STOPPING, AS_STATES };
static const char *const AS_STEP_TEXT[AS_STATES] = {"OFF", "PICK", "MOVE", "GATE", "SCENE", "WALK", "LIFT", "LOADING", "INSIDE", "CHEST", "GOAL",
                                                    "LEAVE", "BACK", "SURFACE", "SAVED", "DIAGNOSE", "R_MENU", "R_PROMPT", "R_UNKNOWN", "GAMEOVER", "STOPPING"};
static const char *const AS_STAGE_TEXT[] = {"", "go to location", "activate location", "enter location", "go to location exit", "exit location",
                                            "wait for the location exit to complete, or skip it", "exit the location by walking", "go to the next location"};
enum { REC_MENU, REC_RETURNED, REC_PROMPT, REC_LIFT, REC_UNKNOWN, RECS };
static const char *const AS_REC_TEXT[RECS] = {"MENU_OPEN", "RETURNED", "PROMPT_NO_RESPONSE", "ON_LIFT_NO_LOAD", "UNKNOWN"};
static const int AS_REC_MAX[RECS] = {3, 1, 3, 3, 1};
enum { AS_FAILS_ALIKE = 10 };   /* give-ups in a row with the same reason that stop a run */
#define AS_STREAM_MIN_S 2.0     /* a direct move: held at least this long, then until the map is stable */
#define AS_STREAM_MAX_S 20.0    /* and set down after this whatever the map says */
static struct {
    int step, stage, one, target, k, chest, done, after, tries, event, back, flight, phase, have_last, have_safe, inside;   /* inside: in the shrine's own map */
    int walk, until_field, skips, approached, wd_have, goal_ok, moving, held, listed;   /* held: the last stick probe found Link held by a scene; listed: the given-up shrines logged */   /* walk: the WASD keys held (SHRINE_STICK_...); until_field / skips /
        moving: AS_SCENE's; approached: walked up to the prompt (a recovery did it); goal_ok: the altar's event was seen to start */
    int ret, rec[RECS], rec_kind, menu_fixing, unknown_seen, recovering, resume, fails_alike, skipped, verify_k, gate_needed, settle;
        /* ret: the state a recovery returns to; rec: each recovery's count for this shrine; recovering: walking out of an
           abandoned shrine; resume: AS_STOPPING goes on to the next shrine; gate_needed: the shrine was not activated;
           fails_alike: give-ups in a row with fail_sig's reason; settle: AS_PICK first waits for Link to stand still
           (after a give-up: the game may still be fading him back) */
    char fail_sig[80];
    double started, t0, phase_t, last_tick, save_time, wd_lost, stall_t, hold;
    float stand[3], face[3], last[3], safe[3], probe[3], wd_pos[3], stall_pos[3], a_pos[3], expect[3];   /* expect: where the state put him */
    float p_floor[3], p_face[3];   /* the prompt AS_R_PROMPT works on: where to stand, what to face */
    uint32_t seq_inside, seq_field, seq_goal, event_seq, seq_gameover;   /* the RomFS counters when the state began */
    uint8_t state[SHRINES], run[SHRINES];
    char left_out[320], why[300], skipped_list[900];
} as;
static ShrineGivenUp as_given;   /* autoshrine-given-up.txt as AS_PICK last read it (frame-end thread) */
static int as_sludge_gone = -1;  /* the newest save: the Zora region's sludge is gone (shrine_sludge_gone) */
static int ak_stable(void);      /* QueryBancLoadMgrIsStable (auto korok's, below): 1, 0, -1 not known */
static _Atomic uint32_t as_seq_gameover;           /* DmF_SY_GameOver's flow opened */
static _Atomic uint64_t as_event_ns, as_map_ns;    /* when an event file, a map file last opened (runtime_now_ns) */
/* input.c: a press that lets go by itself (ms 0: at once), and letting go of all of them. Every key the run presses is one:
   no key stays held when the run stops ticking (a load, a cutscene without a player, a stop). */
void runtime_input_lease(unsigned code, unsigned ms);
void runtime_input_lease_release_all(void);
/* frontend.m (Mac): a short sound when a run ends, ok or not; elsewhere nothing. */
__attribute__((weak)) void runtime_front_run_sound(int ok) { (void)ok; }
/* TOTK_AUTOSHRINE_SHOTS=<folder> (testing): a screenshot at every state change and once a second while landing or in a
   cutscene, named <n>-<state>.png, so a run can be judged afterwards without anyone watching it. */
void dm_gfx_shot_at(long frames, const char *path);
static void as_shot(const char *what) {
    static const char *dir; static int checked; static unsigned n;
    if (!checked) { checked = 1; dir = getenv("TOTK_AUTOSHRINE_SHOTS"); if (dir && !*dir) dir = NULL; }
    if (!dir) return;
    char path[1200]; snprintf(path, sizeof path, "%s/%04u-%s.png", dir, n++, what);
    for (char *c = path + strlen(dir) + 1; *c; c++) if (*c == ' ' || *c == '/' || *c == ',' || *c == '"') *c = '_';
    dm_gfx_shot_at(2, path);
}
static double as_ago(_Atomic uint64_t *ns) { uint64_t v = atomic_load(ns); return v ? (double)(runtime_now_ns() - v) * 1e-9 : 1e9; }
/* A recovery's count before the shrine is given up: outside, a prompt or the lift that does not answer gets one more try
   (a closed, blocked or sludge-covered entrance answers no more on the third; 2026-10-29) */
static int as_rec_max(int kind) { return !as.inside && (kind == REC_PROMPT || kind == REC_LIFT) ? 1 : AS_REC_MAX[kind]; }
static void as_say(void) {
    char line[1600]; const char *name = as.k >= 0 ? as_mark[LANDMARK_FIRST_SHRINE + as.k].name : "";
    char skipped[960] = "", retry[64] = "";
    if (as.skipped) snprintf(skipped, sizeof skipped, " · %d skipped (%s)", as.skipped, as.skipped_list);
    if (as.step >= AS_DIAGNOSE && as.step <= AS_GAMEOVER && as.rec_kind >= 0) snprintf(retry, sizeof retry, " (%s, try %d of %d)", AS_REC_TEXT[as.rec_kind], as.rec[as.rec_kind], as_rec_max(as.rec_kind));
    if (as.step != AS_OFF) snprintf(line, sizeof line, "Running: %s%sstep %d, %s · state %s%s · %d done%s", name, *name ? " · " : "", as.stage, AS_STAGE_TEXT[as.stage],
                                    AS_STEP_TEXT[as.step], retry, as.done, skipped);
    else snprintf(line, sizeof line, "%s · %d done%s%s%s", as.why[0] ? as.why : "Stopped", as.done, skipped, as.left_out[0] ? " · left out: " : "", as.left_out);
    pthread_mutex_lock(&as_status_lock); snprintf(as_status, sizeof as_status, "%s", line); pthread_mutex_unlock(&as_status_lock);
}
static void as_stage(int stage) {
    if (stage == as.stage) return;
    as.stage = stage;
    fprintf(stderr, "[autoshrine] step %d, %s%s%s\n", stage, AS_STAGE_TEXT[stage], as.k >= 0 ? ": " : "", as.k >= 0 ? as_mark[LANDMARK_FIRST_SHRINE + as.k].name : "");
    as_say();
}
static void as_key(unsigned code, double seconds) { runtime_input_lease(code, (unsigned)(seconds * 1000)); }
static int as_pos(uintptr_t actor, float xyz[3]) { return actor && read_mem(actor + ACTOR_POS, xyz, 12) && valid_xyz(xyz); }
/* A loading save reads back as the origin (0, -0.15, 0): no place then (see as_pick). */
int runtime_player_pos(float xyz[3]) {
    return survey_main && as_pos(player_actor(), xyz) && (fabsf(xyz[0]) >= 1 || fabsf(xyz[2]) >= 1);
}
static float as_xz(const float a[3], const float b[3]) { float x = a[0] - b[0], z = a[2] - b[2]; return sqrtf(x * x + z * z); }
/* Link at xyz facing face (level): forceSetMatrix with a turn about the vertical; his local +z (the matrix's third column) is
   where he faces, as noclip reads it. */
static void as_force_facing(uintptr_t actor, const float xyz[3], const float face[3]) {
    float dx = face[0] - xyz[0], dz = face[2] - xyz[2], l = sqrtf(dx * dx + dz * dz), s = l > 1e-3f ? dx / l : 0, c = l > 1e-3f ? dz / l : 1;
    float m[12] = {c, 0, s, xyz[0], 0, 1, 0, xyz[1], -s, 0, c, xyz[2]};
    if (noclip_force && actor) noclip_force((void *)actor, m, 0);
}
/* The WASD keys that walk Link toward to from the camera's view (no camera: ahead, as he was turned). */
static int as_stick_toward(uintptr_t actor, const float to[3]) {
    float cam[3], right[3], at[3], fwd[2], rt[2], dir[2], cl; uint64_t frame;
    if (!to || !as_pos(actor, at)) return 0;
    shrine_unit_xz(at, to, dir);
    if (!dm_gfx_camera(cam, right, &frame) || (cl = sqrtf(right[0] * right[0] + right[2] * right[2])) < 1e-3f) return SHRINE_STICK_UP;
    shrine_unit_xz(cam, at, fwd); rt[0] = right[0] / cl; rt[1] = right[2] / cl;
    return shrine_stick(fwd, rt, dir);
}
static const unsigned AS_STICK_KEY[4] = {AS_KEY_UP, AS_KEY_DOWN, AS_KEY_LEFT, AS_KEY_RIGHT};
/* The left stick toward to: each held key is leased for 0.3 s and renewed every frame while the state wants it, so it lets
   go by itself when the run stops calling; to NULL lets go at once. */
static void as_walk(uintptr_t actor, const float to[3]) {
    int want = to ? as_stick_toward(actor, to) : 0;
    for (int i = 0; i < 4; i++)
        if (want & (1 << i)) runtime_input_lease(AS_STICK_KEY[i], 300);
        else if (as.walk & (1 << i)) runtime_input_lease(AS_STICK_KEY[i], 0);
    as.walk = want;
}
/* Walking up to a point: 1 once he is within 0.8 m of it, stopped (0.5 s without moving after the first second) or the
   time is up; the keys are let go then. */
static int as_walk_up(uintptr_t actor, const float to[3], double t, double now, double limit) {
    float at[3]; int have = as_pos(actor, at);
    float d = have ? as_xz(at, to) : 99;
    int there = d < 0.8f, stalled = 0;
    if (have && (now - as.stall_t >= 0.5 || t < 0.05)) { stalled = t > 1 && landmark_distance(at, as.stall_pos) < 0.1f; memcpy(as.stall_pos, at, 12); as.stall_t = now; }
    if (there || stalled || t > limit) {
        fprintf(stderr, "[autoshrine] walked up in %.1f s, %.1f m from the point%s\n", t, d, there ? "" : stalled ? " (stopped by something)" : " (time up)");
        as_walk(0, NULL); return 1;
    }
    as_walk(actor, to);
    return 0;
}
/* Before every A: Link turned to face the prompt (forceSetMatrix where he stands) and a 0.12 s tap of the stick toward it. */
static void as_face(uintptr_t actor, const float to[3]) {
    float at[3];
    if (!as_pos(actor, at)) return;
    as_force_facing(actor, at, to);
    int keys = as_stick_toward(actor, to);
    for (int i = 0; i < 4; i++) if (keys & (1 << i)) runtime_input_lease(AS_STICK_KEY[i], 120);
}
static double as_save_time(void) {
    const char *profile = getenv("TOTK_PROFILE_DIR"); char path[1800]; struct stat st;
    if (!profile) return 0;
    map_pins_newest_save(profile, path, sizeof path);
    return path[0] && !stat(path, &st) ? st.st_mtimespec.tv_sec + st.st_mtimespec.tv_nsec * 1e-9 : 0;
}
/* Each shrine's DungeonState from the newest save, whether the Zora region's sludge is gone, and the story lock into lm;
   0 when there is no save. */
static int as_read_save(Landmark *lm) {
    const char *profile = getenv("TOTK_PROFILE_DIR"); char path[1800]; int got = 0;
    memset(as.state, SHRINE_STATE_UNKNOWN, sizeof as.state); as_sludge_gone = -1;
    memcpy(lm, as_mark, sizeof as_mark);
    LandmarkProgress progress = {0, 0, 0};
    if (profile && landmarks_state_read(profile, lm, &progress) >= 0) {
        map_pins_newest_save(profile, path, sizeof path);
        FILE *f = path[0] ? fopen(path, "rb") : NULL;
        uint8_t *b = f ? malloc(4 << 20) : NULL; size_t n = b ? fread(b, 1, 4 << 20, f) : 0;
        if (f) fclose(f);
        got = b ? shrine_states_parse(b, n, as.state) : 0;
        as_sludge_gone = b ? shrine_sludge_gone(b, n) : -1;
        free(b);
    }
    int lock = settings_get("teleport.story_lock");
    landmarks_lock(lm, &progress, lock != 0);   /* on unless switched off (-1: no such setting) */
    return got;
}
/* Every transition: "[autoshrine] state X -> Y: reason", a screenshot (TOTK_AUTOSHRINE_SHOTS), the status line. */
static void as_go(int step, double now, const char *reason) {
    as_walk(0, NULL); runtime_input_lease_release_all();   /* a new state starts with no key held */
    if (step != as.step || reason) fprintf(stderr, "[autoshrine] state %s -> %s: %s\n", AS_STEP_TEXT[as.step], AS_STEP_TEXT[step], reason ? reason : "next");
    as.step = step; as.t0 = now; as.phase = 0; as.tries = 0; as.have_last = 0; as.event = 0; as.approached = 0;
    as.seq_inside = atomic_load(&as_seq_inside); as.seq_field = atomic_load(&as_seq_field); as.seq_goal = atomic_load(&as_seq_goal);
    as.event_seq = atomic_load(&as_seq_event); as.seq_gameover = atomic_load(&as_seq_gameover);
    atomic_store(&as_watch_field, step == AS_GOAL || step == AS_BACK || step == AS_LEAVE);
    if (step != AS_OFF) as_shot(AS_STEP_TEXT[step]);
    as_say();
}
static void as_move(const float stand[3], const float face[3], int after, double hold, double now, const char *reason) {
    memcpy(as.stand, stand, 12); memcpy(as.face, face, 12); as.after = after; as.flight = 0; as.hold = hold;
    as_go(AS_MOVE, now, reason);
}
/* The run ends: ok (the last one done, or nothing left) or why it stopped. Every key is let go; noclip ends with Link on
   his feet: when it is on, he is first taken back to where he last stood, unless the player stopped it; then noclip off.
   A summary and a sound. */
static void as_end(int ok, const char *why, double now, int by_player) {
    as_walk(0, NULL); runtime_input_lease_release_all(); (void)by_player;   /* a stop by the player sets Link down safely too */
    snprintf(as.why, sizeof as.why, "%s%s", ok ? "Finished: " : "", why);
    fprintf(stderr, "[autoshrine] %s\n", as.why);
    fprintf(stderr, "[autoshrine] summary: %d done, %d skipped%s%s%s\n", as.done, as.skipped, as.skipped ? " (" : "", as.skipped_list, as.skipped ? ")" : "");
    {   /* the progress is in the game's own autosave after each shrine (5Out asks for one); the menu's manual save is not driven */
        double saved = as_save_time(); struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        if (saved > 0) fprintf(stderr, "[autoshrine] the newest save is %.0f s old (the game's autosave)\n", ts.tv_sec + ts.tv_nsec * 1e-9 - saved);
        else fprintf(stderr, "[autoshrine] no save found in the profile\n");
    }
    runtime_front_run_sound(ok);
    if (atomic_load(&noclip_wanted) && (noclip_flight.on || !as.have_safe)) {   /* in the air (a flight; stopped by the player too):
        the flight's own end is on the ground; down there first, then noclip off (2026-10-28: a stop mid-flight dropped Link 60 m) */
        float at[3]; pthread_mutex_lock(&noclip_goto_lock); int pending = noclip_goto.pending, direct = noclip_goto.direct; pthread_mutex_unlock(&noclip_goto_lock);
        if (noclip_flight.on || pending) {
            memcpy(at, noclip_flight.on ? noclip_flight.to : noclip_goto.xyz, 12); memcpy(as.stand, at, 12); as.flight = pending && direct;
            as_go(AS_STOPPING, now, "setting Link down where the move lands"); return;
        }
    }
    if (atomic_load(&noclip_wanted) && as.step == AS_MOVE && as.flight && noclip.holding) {   /* held after a direct move: down there once the ground has streamed in */
        memcpy(as.stand, noclip.xyz, 12); as_go(AS_STOPPING, now, "setting Link down where he is held, once the ground has streamed in"); return;
    }
    if (atomic_load(&noclip_wanted) && as.have_safe) {   /* far: a direct move back, held there until the ground has streamed in */
        float at[3] = {as.safe[0], as.safe[1] + 0.5f, as.safe[2]};
        as.flight = !as.inside && noclip.holding && landmark_distance(noclip.xyz, at) > 40;
        if (as.flight) runtime_noclip_jump(at);
        memcpy(as.stand, at, 12); as_go(AS_STOPPING, now, "setting Link down"); return;
    }
    if (atomic_load(&noclip_wanted)) runtime_noclip_set(0);   /* stopped by the player too: never left flying */
    if (!atomic_load(&noclip_wanted)) dm_gfx_camera_want(0);
    as_go(AS_OFF, now, why); atomic_store(&as_active, 0); as_say();
}
/* Shrine k given up for reason: added to autoshrine-given-up.txt (read again first: the menu may have reset it) with the
   save's DungeonState now, so later runs leave it out until that changes. */
static void as_given_up_record(int k, const char *reason) {
    if (k < 0 || k >= SHRINES) return;
    shrine_given_up_load(&as_given);
    shrine_given_up_add(&as_given, k, reason, as.state[k], time(NULL));
    char path[1024]; shrine_given_up_path(path, sizeof path);
    if (shrine_given_up_save(&as_given)) fprintf(stderr, "[autoshrine] [red] could not write %s\n", path[0] ? path : "autoshrine-given-up.txt (no profile folder)");
    else fprintf(stderr, "[autoshrine] given up for later runs too: Dungeon%03d in %s\n", k, path);
}
/* What a reason says without its numbers (places, distances, times): give-ups in a row with the same one look like one
   problem in general. */
static void as_fail_sig(const char *what, char *out, size_t n) {
    size_t o = 0;
    for (const char *c = what; *c && o + 1 < n; c++) if (!isdigit((unsigned char)*c) && *c != '.' && *c != ',' && *c != '-') out[o++] = *c;
    if (n) out[o] = 0;
}
/* A shrine given up (2026-10-29: at once, no long retries): recorded with the step and the reason, in this run and in
   autoshrine-given-up.txt; inside, Link first walks out by the start lift; then the next nearest shrine, from where he
   is (still held by noclip: the next direct move starts from there; let go: once he stands still, since the game may
   be taking him back). In a run of one shrine the run ends here; a long run ends only after AS_FAILS_ALIKE give-ups in
   a row with the same reason. */
static void as_abandon(const char *what, double now) {
    char why[400]; const char *name = as.k >= 0 ? as_mark[LANDMARK_FIRST_SHRINE + as.k].name : "";
    snprintf(why, sizeof why, "Stopped at step %d, %s: %s%s%s%s", as.stage, AS_STAGE_TEXT[as.stage], what, *name ? " (" : "", name, *name ? ")" : "");
    if (as.k < 0) { as_end(0, why, now, 0); return; }
    if (as.recovering) {   /* the walk out failed: nothing left to try */
        char w2[460]; snprintf(w2, sizeof w2, "%s, while leaving a shrine given up", why); as_end(0, w2, now, 0); return;
    }
    as.skipped++; as.run[as.k] = SHRINE_SKIP_FAILED;
    size_t o = strlen(as.skipped_list);
    snprintf(as.skipped_list + o, sizeof as.skipped_list - o, "%s%s: step %d, %s", o ? "; " : "", name, as.stage, what);
    fprintf(stderr, "[autoshrine] giving up on %s (Dungeon%03d): step %d, %s: %s\n", name, as.k, as.stage, AS_STAGE_TEXT[as.stage], what);
    char reason[300]; snprintf(reason, sizeof reason, "step %d, %s: %s", as.stage, AS_STAGE_TEXT[as.stage], what);
    as_given_up_record(as.k, reason);
    char sig[80]; as_fail_sig(reason, sig, sizeof sig);
    as.fails_alike = strcmp(sig, as.fail_sig) ? 1 : as.fails_alike + 1; snprintf(as.fail_sig, sizeof as.fail_sig, "%s", sig);
    if (as.inside) {   /* STUCK_INSIDE: out by the start lift first */
        float stand[3], face[3]; shrine_stand(&as_shrine[as.k], SHRINE_AT_BACK_APPROACH, 0, stand, face);
        as.recovering = 1; as_stage(7); as_move(stand, face, AS_BACK, 0.6, now, "STUCK_INSIDE: walking out by the start lift"); return;
    }
    if (as.one || as.target >= 0) { as_end(0, why, now, 0); return; }
    if (as.fails_alike >= AS_FAILS_ALIKE) { char w2[460]; snprintf(w2, sizeof w2, "%s; %d shrines in a row given up for the same reason", why, as.fails_alike); as_end(0, w2, now, 0); return; }
    as.settle = !(atomic_load(&noclip_wanted) && noclip.holding);
    as_stage(8); as_go(AS_PICK, now, as.settle ? "the next shrine, once Link stands still" : "the next shrine, from where noclip holds Link");
}
/* A state's outcome did not come: AS_DIAGNOSE looks at what is going on and picks a recovery that returns to ret. */
static void as_diagnose(int ret, double now, const char *why) {
    char r[200]; snprintf(r, sizeof r, "%s (state %s)", why, AS_STEP_TEXT[ret]);
    as.ret = ret; as_go(AS_DIAGNOSE, now, r);
}
/* Starts recovery kind (counted); 0 when its count is used up (the shrine is then given up). */
static int as_recover(int kind, double now) {
    as.rec_kind = kind;
    if (++as.rec[kind] > as_rec_max(kind)) {
        char why[160]; snprintf(why, sizeof why, "%s after %d recover%s (state %s)", AS_REC_TEXT[kind], as_rec_max(kind), as_rec_max(kind) == 1 ? "y" : "ies", AS_STEP_TEXT[as.ret]);
        as_abandon(why, now); return 0;
    }
    (void)now; return 1;
}
/* The prompt a state works on, for AS_R_PROMPT: the gate (stand at AuthenticationPos's walk point, face in), the altar
   (stand on SmallDungeonExit, face the altar), the start lift (its floor, face its prompt). 0: no prompt in that state. */
static int as_prompt_of(int st) {
    const ShrineInfo *s = &as_shrine[as.k]; float stand[3], face[3];
    if (st == AS_GATE || st == AS_WALK) { shrine_stand(s, SHRINE_AT_ACTIVATE, 0, stand, face); memcpy(as.p_floor, stand, 12); memcpy(as.p_face, face, 12); return AS_GATE; }
    if (st == AS_GOAL || st == AS_LEAVE) { memcpy(as.p_floor, s->goal_floor, 12); memcpy(as.p_face, s->goal, 12); return st; }
    if (st == AS_BACK) { memcpy(as.p_floor, s->back_floor, 12); memcpy(as.p_face, s->back, 12); return AS_BACK; }
    return 0;
}
/* The next shrine: the nearest one left from where Link is now (re-read at every choice, after a shrine done or given up;
   with direct moves the way there costs about the same near or far, so nearest-first is kept as it is). */
static void as_pick(uintptr_t actor, double now) {
    float at[3];
    /* While a save loads the player reads back as the origin (0, -0.15, 0): wait for his real place, as for no player */
    if (!as_pos(actor, at) || (fabsf(at[0]) < 1 && fabsf(at[2]) < 1)) { if (now - as.t0 > 120) as_end(0, "Stopped: no player to start from (120 s)", now, 0); return; }
    if (as.settle) {   /* after a give-up with noclip off: where the game leaves him (a return fades him back) once he stands still for 1 s (8 s at most) */
        if (!as.have_last || landmark_distance(at, as.last) > 0.3f) { memcpy(as.last, at, 12); as.have_last = 1; as.phase_t = now; }
        if (now - as.phase_t < 1.0 && now - as.t0 < 8) return;
        as.settle = 0; as.have_last = 0;
    }
    static Landmark lm[LANDMARKS]; int skip[SHRINES], count[SHRINE_SKIPS] = {0};
    int found = as_read_save(lm);
    if (as.verify_k >= 0 && as.verify_k < SHRINES) {   /* the shrine done last: what the save says now (read here, not waited for) */
        fprintf(stderr, "[autoshrine] the save now says DungeonState.Dungeon%03d = %s\n", as.verify_k, as.state[as.verify_k] ? SHRINE_STATE_NAMES[as.state[as.verify_k]] : "(not in it)");
        as.verify_k = -1;
    }
    /* Given up earlier: kept out while the save still says what it said then; a change (opened, done) drops the line */
    int given = shrine_given_up_load(&as_given), dropped = 0;
    for (int k = 0; k < SHRINES; k++)
        if (as_given.on[k] && found && !shrine_given_up_holds(&as_given, k, as.state[k])) {
            fprintf(stderr, "[autoshrine] Dungeon%03d (%s) is tried again: the save says %s now, %s when it was given up\n", k, as_mark[LANDMARK_FIRST_SHRINE + k].name,
                    as.state[k] ? SHRINE_STATE_NAMES[as.state[k]] : "nothing", SHRINE_STATE_NAMES[as_given.state[k]]);
            as_given.on[k] = 0; dropped++;
        }
    if (dropped && shrine_given_up_save(&as_given)) fprintf(stderr, "[autoshrine] [red] could not rewrite autoshrine-given-up.txt\n");
    if (!as.listed) {   /* once a run */
        as.listed = 1;
        fprintf(stderr, "[autoshrine] the Zora region's sludge (Step_SageOfZora): %s; %d shrine%s given up earlier (autoshrine-given-up.txt)\n",
                as_sludge_gone > 0 ? "gone" : as_sludge_gone == 0 ? "still there (a shrine it covers is left out)" : "the save does not say", given, given == 1 ? "" : "s");
        for (int k = 0; k < SHRINES; k++)
            if (as_given.on[k]) fprintf(stderr, "[autoshrine] given up earlier: %s (Dungeon%03d), %s: %s\n", as_mark[LANDMARK_FIRST_SHRINE + k].name, k, as_given.date[k], as_given.reason[k]);
    }
    int sludge_on = as_sludge_gone == 0;   /* not known: tried, and given up fast if the sludge is there */
    for (int k = 0; k < SHRINES; k++) {
        int flags = k == as.target ? 0 : (as_given.on[k] ? SHRINE_GIVEN_UP : 0) | (sludge_on ? SHRINE_SLUDGE_ON : 0);   /* a shrine chosen on the tab is tried anyway (the sludge may be washed off) */
        count[skip[k] = shrine_skip(&as_shrine[k], as.state[k], lm[LANDMARK_FIRST_SHRINE + k].lock, as.run[k], flags)]++;
    }
    size_t o = 0; as.left_out[0] = 0;
    for (int r = 1; r < SHRINE_SKIPS && o < sizeof as.left_out - 1; r++)
        if (count[r]) o += (size_t)snprintf(as.left_out + o, sizeof as.left_out - o, "%s%d %s", o ? ", " : "", count[r], SHRINE_SKIP_TEXT[r]);
    fprintf(stderr, "[autoshrine] %d shrines to do; %d DungeonState flags in the save; left out: %s\n", count[SHRINE_GO], found, as.left_out[0] ? as.left_out : "none");
    if (as.one && as.done) { as_end(1, "the shrine is done", now, 0); return; }
    if (as.target >= SHRINES) as.target = -1;
    if (as.target >= 0 && skip[as.target] != SHRINE_GO) {   /* the shrine chosen on the Shrines tab, which the run cannot do */
        char why[200]; snprintf(why, sizeof why, "Refused: %s is left out: %s", as_mark[LANDMARK_FIRST_SHRINE + as.target].name, SHRINE_SKIP_TEXT[skip[as.target]]);
        as.k = -1; as_end(0, why, now, 0); return;
    }
    int k = as.target >= 0 ? as.target : shrine_next(as_shrine, skip, at);
    if (k < 0) { as.k = -1; as_end(1, "no shrine left to do", now, 0); return; }
    as.k = k; as.stage = 0; as.goal_ok = 0; as.back = 0; as.inside = 0; as.recovering = 0; as.rec_kind = -1;
    memset(as.rec, 0, sizeof as.rec);
    as.save_time = as_save_time();   /* AS_SAVED reads only saves newer than this */
    as_stage(1);
    const ShrineInfo *s = &as_shrine[k];
    fprintf(stderr, "[autoshrine] next: %s (Dungeon%03d, %s, %.0f m away, save says %s, %d chest%s)\n", as_mark[LANDMARK_FIRST_SHRINE + k].name, k,
            SHRINE_KIND_TEXT[s->kind], landmark_distance(s->entrance, at), as.state[k] ? SHRINE_STATE_NAMES[as.state[k]] : "nothing", s->nchests, s->nchests == 1 ? "" : "s");
    float stand[3], face[3];
    as.gate_needed = as.state[k] == SHRINE_STATE_APPEAR || as.state[k] == SHRINE_STATE_UNKNOWN;   /* not activated yet: the gate's A first */
    shrine_stand(s, SHRINE_AT_TRAVEL, 0, stand, face);   /* where the Shrines tab's Go lands: never at the closed entrance */
    as_move(stand, face, as.gate_needed ? AS_GATE : AS_WALK, 2.0, now, as.gate_needed ? "to the travel point; not activated yet" : "to the travel point; activated already");
}
/* A cutscene (or a message): skipped at once and again every 0.8 s; until_field: the altar's (it ends with the surface's map). */
static void as_scene(int after, int until_field, double now, const char *why) {
    as.after = after; as.until_field = until_field; as.skips = 0; as.moving = 0; as.held = 0;
    as_go(AS_SCENE, now, why);
}
static void as_after_inside(double now) {   /* step 4: the chests if asked, then onto SmallDungeonExit, by the altar's prompt */
    const ShrineInfo *s = &as_shrine[as.k]; float stand[3], face[3];
    as_stage(4);
    if (atomic_load(&as_chests) && as.chest < s->nchests) { shrine_stand(s, SHRINE_AT_CHEST, as.chest, stand, face); as_move(stand, face, AS_CHEST, 0.6, now, "to a chest"); return; }
    shrine_stand(s, SHRINE_AT_GOAL, 0, stand, face); as_move(stand, face, AS_GOAL, 0.6, now, "onto SmallDungeonExit, by the altar's prompt");
}
static void as_go_on(double now) {   /* AS_SCENE is over */
    if (as.after == AS_INSIDE) { as.chest = 0; as_after_inside(now); return; }
    if (as.after == AS_WALK) as_stage(3);
    as_go(as.after, now, "the scene is over");
}
/* Back from a recovery to the state that called it. A prompt state starts at its A when the recovery walked him up. */
static void as_return(double now, const char *why) {
    int st = as.ret, appr = as.approached;
    if (st == AS_MOVE) { as_move(as.stand, as.face, as.after, as.hold, now, why); return; }
    as_go(st, now, why);
    as.approached = appr;
}
/* After a direct move: 1 once the ground around Link has streamed in, so that he lands on it (2026-10-29: set down where
   the map is still loading he falls through it and the game fades him back to where he last stood, "the game returned
   Link"; the flight's fixed 2 s on the ground did that to three shrines in a row). held: seconds held there, min_hold at
   least. Ready: the map loader's stable bit (QueryBancLoadMgrIsStable, as auto korok waits for it; 1.4.3) and no map file
   opened for 1 s; without the bit (1.0.0) 3 s more and the quiet; AS_STREAM_MAX_S at most. */
static int as_streamed(double held, double min_hold) {
    if (held < min_hold) return 0;
    int stable = ak_stable(); double quiet = as_ago(&as_map_ns);
    int ready = (stable == 1 || (stable < 0 && held >= min_hold + 3)) && quiet >= 1.0;
    if (!ready && held < AS_STREAM_MAX_S) return 0;
    fprintf(stderr, "[autoshrine] %s after %.1f s held there (map stable=%d, last map file %.1f s ago)\n", ready ? "the ground has streamed in" : "[red] still streaming in; set down anyway",
            held, stable, quiet < 1e8 ? quiet : -1.0);
    return 1;
}
static void as_tick_move(uintptr_t actor, double now, double dt) {
    float at[3] = {0, 0, 0}; int have = as_pos(actor, at);
    if (as.phase == 0) {   /* noclip on: on the surface a direct move when far (there at once, held while the ground streams in;
                              2026-10-29, no flight), else (and inside a shrine) a straight line at 30 m/s */
        if (!have) { if (now - as.t0 > 20) as_diagnose(AS_MOVE, now, "no player to move (20 s)"); return; }
        runtime_input_lease_release_all();   /* no key of ours held: noclip reads WASD and ZL as the player steering */
        float d = landmark_distance(at, as.stand); as.flight = d > 40 && !as.inside;   /* flight: a direct move */
        if (as.flight) runtime_noclip_jump(as.stand); else runtime_noclip_set(1);
        as.phase_t = now + (as.flight ? 0 : d / 30) + 15; as.phase = 1;   /* phase_t: the move's time limit */
        fprintf(stderr, "[autoshrine] %s to %.1f,%.1f,%.1f (%.0f m), then held there %s\n", as.flight ? "direct move" : "straight", as.stand[0], as.stand[1], as.stand[2], d,
                as.flight ? "until the ground has streamed in" : "a moment");
        return;
    }
    if (as.phase == 1) {
        if (now > as.phase_t) { as_diagnose(AS_MOVE, now, "the move did not arrive in time"); return; }
        if (!noclip.holding) return;
        if (as.flight) {
            pthread_mutex_lock(&noclip_goto_lock); int pending = noclip_goto.pending; pthread_mutex_unlock(&noclip_goto_lock);
            if (pending || noclip_flight.on) return;
        } else {
            float d = landmark_distance(noclip.xyz, as.stand), step = 30 * (float)dt;
            if (d > step) { for (int c = 0; c < 3; c++) noclip.xyz[c] += (as.stand[c] - noclip.xyz[c]) * step / d; return; }
        }
        memcpy(noclip.xyz, as.stand, 12); as.phase = 2; as.phase_t = now;
        return;
    }
    if (as.phase == 2) {   /* held there while the place loads around him (a direct move: until it has streamed in), turned to
                              face the prompt, then set down; noclip_guard keeps him alive and resets the fall until he stands */
        memcpy(noclip.xyz, as.stand, 12); as_force_facing(actor, as.stand, as.face);
        double held = now - as.phase_t;
        if (as.flight ? as_streamed(held, fmax(as.hold, AS_STREAM_MIN_S)) : held >= as.hold) { runtime_noclip_set(0); dm_gfx_camera_want(1); as.phase = 3; as.phase_t = now; }
        return;
    }
    if (as.phase == 3) {   /* landed: on his feet near the place, and still there (the game returns a player it finds stuck) */
        { static double last; if (now - last >= 1) { last = now; as_shot("landing"); } }
        memcpy(as.expect, as.stand, 12);
        if (have && as_xz(at, as.stand) > 8) {   /* RETURNED (2026-10-29): given up at once, no second landing */
            char why[260]; snprintf(why, sizeof why, "the game returned Link from the landing spot %.1f,%.1f,%.1f (it faded out and put him back at %.1f,%.1f,%.1f)",
                                    as.stand[0], as.stand[1], as.stand[2], at[0], at[1], at[2]);
            as_abandon(why, now); return;
        }
        if (now - as.phase_t < (as.inside ? 1.0 : 2.5)) return;
        if (!have || as_xz(at, as.stand) > 4 || at[1] < as.stand[1] - 6) { as_diagnose(AS_MOVE, now, "Link did not stay where he was set down"); return; }
        fprintf(stderr, "[autoshrine] landed and stayed at %.1f,%.1f,%.1f\n", at[0], at[1], at[2]);
        memcpy(as.safe, at, 12); as.have_safe = 1;
        as_key(AS_KEY_ZL, 0.15);   /* the camera behind him (it can stay behind a wall after a straight move) */
        as.phase = 4; as.phase_t = now;
        return;
    }
    if (as.phase == 4 && now - as.phase_t >= 0.5) { as_key(AS_KEY_ZL, 0.15); as.phase = 5; return; }   /* once more: the camera on Link */
    if (as.phase == 5 && now - as.phase_t >= 0.9) {
        if (as.after == AS_GATE) as_stage(2);
        else if (as.after == AS_WALK) as_stage(3);
        else if (as.after == AS_BACK) as_stage(7);
        as_go(as.after, now, "landed");
    }
}
/* AS_DIAGNOSE: a push of the stick (back, 0.35 s), then the signals decide. */
static void as_tick_diagnose(uintptr_t actor, double now, double t) {
    float at[3]; int have = as_pos(actor, at);
    const ShrineInfo *s = &as_shrine[as.k];
    if (atomic_load(&as_seq_gameover) != as.seq_gameover) { as.rec_kind = -1; as_go(AS_GAMEOVER, now, "the game-over event opened"); return; }
    if (as.phase == 0) {
        if (!have) {   /* no player: a load, or the game-over screen */
            if (as_ago(&as_map_ns) < 5 && t < 30) return;
            if (t > 4) { as_go(AS_GAMEOVER, now, "no player for 4 s and no map loading (game over?)"); }
            return;
        }
        memcpy(as.probe, at, 12); as_key(AS_KEY_DOWN, 0.35); as.phase = 1; as.phase_t = now;
        return;
    }
    if (now - as.phase_t < 0.8) return;
    float moved = have ? landmark_distance(at, as.probe) : 0;
    int recent_event = as_ago(&as_event_ns) < 5, loading = as_ago(&as_map_ns) < 3;
    fprintf(stderr, "[autoshrine] diagnose: moved %.2f m under the stick, event file %s, map file %s, %.1f m from where state %s put him\n",
            moved, recent_event ? "lately" : "not lately", loading ? "loading" : "not loading", have ? as_xz(at, as.expect) : -1.0f, AS_STEP_TEXT[as.ret]);
    if (moved < 0.15f) {
        if (loading) { if (as.ret == AS_WALK || as.ret == AS_LIFT) { as_go(AS_LOADING, now, "a map is loading"); return; } as.phase = 0; return; }
        if (recent_event) { as_scene(as.ret, 0, now, "a scene holds him: skipping it, then back"); return; }
        if (as_recover(REC_MENU, now)) { as.menu_fixing = 1; as_go(AS_R_MENU, now, "MENU_OPEN: he does not move and nothing plays"); }
        return;
    }
    if (as.menu_fixing) { as.menu_fixing = 0; as_return(now, "the menu is closed"); return; }
    if (as.ret == AS_MOVE || (have && as_xz(at, as.expect) > 15 && as.ret != AS_SCENE)) {   /* RETURNED: moved there again, held longer (once) */
        if (!as_recover(REC_RETURNED, now)) return;
        double hold = as.rec[REC_RETURNED] == 1 ? 4 : 8;
        as_move(as.stand, as.face, as.ret == AS_MOVE ? as.after : as.ret, hold, now, "RETURNED: the game put him back; moving him there again, held longer"); return;
    }
    int on_lift = have && ((!as.inside && as_xz(at, s->lift_floor) < 2.5f) || (as.inside && as_xz(at, s->back_floor) < 2.5f));
    if (as.ret == AS_LIFT || (on_lift && (as.ret == AS_WALK || as.ret == AS_BACK))) {
        if (!as_recover(REC_LIFT, now)) return;
        as_go(as.inside ? AS_BACK : AS_LIFT, now, "ON_LIFT_NO_LOAD: the lift's A again"); return;
    }
    if (as_prompt_of(as.ret)) {
        if (!as_recover(REC_PROMPT, now)) return;
        if (as.ret == AS_WALK) as.ret = AS_GATE;   /* the way in did not start: the gate may not be activated */
        as_go(AS_R_PROMPT, now, "PROMPT_NO_RESPONSE: back off, face it, walk up, A"); return;
    }
    if (as.unknown_seen) { as.unknown_seen = 0; as_abandon("UNKNOWN twice: no recovery fits", now); return; }
    if (as_recover(REC_UNKNOWN, now)) as_go(AS_R_UNKNOWN, now, "UNKNOWN: keys let go, B twice, noclip off, look again");
}
static void as_script_tick(uintptr_t actor, double now);   /* script mode (below) */
static void autoshrine_tick(uintptr_t actor, double now) {
    int req = atomic_exchange(&as_request, 0);
    if (req == 3 && as.step != AS_OFF && as.step != AS_STOPPING) { as_end(0, "Stopped: from the menu or the input file", now, 0); return; }
    if ((req == 1 || req == 2) && as.step == AS_OFF) {
        int chests = atomic_load(&as_chests); memset(&as, 0, sizeof as); as.k = -1; as.verify_k = -1; as.rec_kind = -1;
        as.one = req == 2; as.target = atomic_exchange(&as_want, -1); as.started = now; as.last_tick = now;
        atomic_store(&as_cancel, 0); dm_gfx_camera_want(1); runtime_input_lease_release_all();
        fprintf(stderr, "[autoshrine] start: %s, chests %s\n", as.target >= 0 ? "one chosen shrine" : as.one ? "the nearest shrine" : "every shrine it can do, nearest first", chests ? "on" : "off");
        as_go(AS_PICK, now, "start");
    }
    if (as.step == AS_OFF) { as_script_tick(actor, now); return; }
    double dt = now - as.last_tick; as.last_tick = now;
    if (dt > 0.1) dt = 0.1;
    if (atomic_exchange(&as_cancel, 0) && now - as.started > 2 && as.step != AS_STOPPING) {
        char why[200]; snprintf(why, sizeof why, "Stopped at step %d, %s: your own input (a key, a button or the stick)", as.stage, AS_STAGE_TEXT[as.stage]);
        as_end(0, why, now, 1); return;
    }
    double t = now - as.t0;
    const ShrineInfo *s = as.k >= 0 ? &as_shrine[as.k] : NULL;
    float at[3]; int have = as_pos(actor, at);
    /* The game-over event and the watchdog, in the states where Link stands in one map: a jump of 30 m at once that is not
       into the shrine (outside) or out to its entrance (inside), or no player for 6 s with no map loading. */
    if (s && as.step != AS_GAMEOVER && as.step != AS_STOPPING && atomic_load(&as_seq_gameover) != as.seq_gameover) { as.rec_kind = -1; as_go(AS_GAMEOVER, now, "the game-over event opened"); return; }
    int watch = as.step == AS_GATE || as.step == AS_WALK || as.step == AS_LIFT || as.step == AS_CHEST || as.step == AS_GOAL || as.step == AS_LEAVE
             || (as.step == AS_SCENE && !as.until_field && as.after != AS_SAVED);
    if (watch && s) {
        if (have) {
            int expected = as.inside ? landmark_distance(at, s->entrance) < 200 : landmark_distance(at, s->start) < 80;
            if (as.wd_have && landmark_distance(at, as.wd_pos) > 30 && !expected) {
                char why[200]; snprintf(why, sizeof why, "the game moved Link %.0f m at once", landmark_distance(at, as.wd_pos));
                as_diagnose(as.step, now, why); return;
            }
            as.wd_lost = now;
        } else if (as.wd_have && now - as.wd_lost > 6 && as_ago(&as_map_ns) > 6) { as.rec_kind = -1; as_go(AS_GAMEOVER, now, "no player for 6 s and no map loading (game over?)"); return; }
    }
    if (have) { memcpy(as.wd_pos, at, 12); if (!as.wd_have) as.wd_lost = now; as.wd_have = 1; }
    switch (as.step) {
    case AS_PICK: as_pick(actor, now); return;
    case AS_MOVE: as_tick_move(actor, now, dt); return;
    case AS_DIAGNOSE: as_tick_diagnose(actor, now, t); return;
    case AS_GATE: {   /* step 2: walk up to the gate, face it, ZL, A: DmF_SY_SmallDungeon1Open activates the shrine; its scene is skipped at once */
        float to[3], face[3]; shrine_stand(s, SHRINE_AT_ACTIVATE, 0, to, face);
        if (!as.approached) { if (as_walk_up(actor, to, t, now, 4)) { as.approached = 1; as.phase_t = now; as.phase = 0; } return; }
        double c = now - as.phase_t;
        if (as.phase == 0) { as_face(actor, face); as.phase = 1; }
        else if (as.phase == 1 && c >= 0.3) { runtime_input_lease_release_all(); as.phase = 2; }   /* nothing held before A: a clean press */
        else if (as.phase == 2 && c >= 0.8) { as_key(AS_KEY_A, 0.25); fprintf(stderr, "[autoshrine] A at the gate\n"); as.phase = 3; }
        else if (as.phase == 3 && c >= 0.9) as_scene(AS_WALK, 0, now, "after the gate's A: skipping its scene");
        return;
    }
    case AS_SCENE: {   /* X (the scenes' own "Skip X"), again 0.25 s later, B every other time for a message; every 0.8 s, the stick
                          pushed all along (back, away from the altar, at the end). Over when Link moves under it two cycles running,
                          the next map loads, or (at the altar) the surface's map loads. */
        { static double last; if (now - last >= 1) { last = now; as_shot("scene"); } }
        if (as.until_field && (atomic_load(&as_seq_field) != as.seq_field || (have && landmark_distance(at, s->entrance) < 200))) {
            fprintf(stderr, "[autoshrine] the altar's scene took Link out after %.0f s (%d skip tries)\n", t, as.skips);
            as.back = 0; as.have_safe = 0; as.inside = 0; as_go(AS_SURFACE, now, "the surface's map is loading"); return;
        }
        if (as.after == AS_WALK && (atomic_load(&as_seq_inside) != as.seq_inside || (have && landmark_distance(at, s->start) < 60))) {
            uint32_t seen = as.seq_inside; as_stage(3); as_go(AS_LOADING, now, "the way in started during the gate's scene"); as.seq_inside = seen; return;
        }
        if (as.until_field && !as.goal_ok) {   /* the altar's event moves him (in front of the monk; 9.7 m in a test, 2026-10-28):
            nothing is pressed until it is seen, as X would jump and the stick walk him away while nothing plays */
            if (atomic_load(&as_seq_goal) != as.seq_goal) { as.goal_ok = 1; fprintf(stderr, "[autoshrine] the goal's event file opened\n"); }
            else if (have && landmark_distance(at, as.a_pos) > 1.5f) { as.goal_ok = 1; fprintf(stderr, "[autoshrine] the altar's event moved Link %.1f m: the blessing has started\n", landmark_distance(at, as.a_pos)); }
            else if (t > 6) { as_diagnose(AS_GOAL, now, "the altar's A started nothing (Link not moved by an event in 6 s)"); return; }
            else {   /* A again at 2 s and 4 s, each a clean press after a quiet moment (in a test a second press did it) */
                if ((fmod(t, 2.0) >= 1.6 && fmod(t, 2.0) < 1.6 + dt)) runtime_input_lease_release_all();
                if (t > 1.9 && fmod(t, 2.0) < dt) { as_key(AS_KEY_A, 0.25); fprintf(stderr, "[autoshrine] A at the altar's prompt again\n"); }
                return;
            }
            as.t0 = now; as.tries = 0; as.phase = 0; as.moving = 0; as.held = 1; t = 0;   /* the skipping starts now: he is held by it */
        }
        if (t > 120) { as_diagnose(as.after, now, "a scene did not end within 2 minutes"); return; }
        if (have) as_key(as.until_field ? AS_KEY_DOWN : AS_KEY_UP, 0.3);
        double c = t - 0.8 * as.tries; int reveal = atomic_load(&as_skip_reveal);
        /* X (Switch X, the top button: "Skip X", shown as "Skip Y" with Xbox labels) only while the last probe found Link held:
           in play the same button jumps. The first cycle only probes. */
        if (as.phase == 0) {
            if (have) memcpy(as.probe, at, 12);
            if (reveal && as.held) { as_key((unsigned)reveal, 0.1); as.skips++; }
            as.phase = 1;
        } else if (as.phase == 1 && c >= 0.25) { if (reveal && as.held) as_key((unsigned)reveal, 0.1); as.phase = 2; }
        else if (as.phase == 2 && c >= 0.5) { if ((as.tries & 1) && as.held) as_key(AS_KEY_B, 0.1); as.phase = 3; }   /* B: a message or the item's Get screen */
        else if (as.phase == 3 && c >= 0.8) {
            float moved = have ? landmark_distance(at, as.probe) : 0;
            as.moving = moved > 0.3f ? as.moving + 1 : 0; as.held = moved <= 0.3f;
            as.tries++; as.phase = 0;
            if (as.moving >= 2 && (!as.until_field || t > 6)) {
                char why[120]; snprintf(why, sizeof why, "Link walks again after %.1f s (%d skip tries)", t, as.skips);
                if (as.until_field && !as.goal_ok) { as_diagnose(AS_GOAL, now, "the altar's A started nothing"); return; }
                if (as.until_field) {   /* the altar's scene is over and he is still inside: ask the altar to leave */
                    float stand[3], face[3]; shrine_stand(s, SHRINE_AT_GOAL, 0, stand, face);
                    as_move(stand, face, AS_LEAVE, 0.6, now, "still inside after the altar: A for \"Do you want to leave?\""); return;
                }
                fprintf(stderr, "[autoshrine] %s\n", why);
                as_go_on(now); return;
            }
        }
        return;
    }
    case AS_WALK: {   /* step 3: noclip off, Link walks to the entrance lift (through the doorway); outcome: on the lift or a map opens (8 s) */
        { static double last; if (now - last >= 1) { last = now; as_shot("walking"); } }
        uint32_t seen = as.seq_inside;
        float d = have ? as_xz(at, s->lift_floor) : 99;
        if (atomic_load(&as_seq_inside) != seen || (have && landmark_distance(at, s->start) < 60)) { as_go(AS_LOADING, now, "the shrine's map opened while walking in"); as.seq_inside = seen; return; }
        if (d < 1.2f) { as_go(AS_LIFT, now, "on the entrance lift"); return; }
        /* Blocked (2026-10-29): walking on the spot for 1.5 s after the first 2 s is a closed door, sludge or rubble: diagnosed
           then, not after the full 8 s */
        if (have && (now - as.stall_t >= 1.5 || t < 0.05)) {
            int stuck = t > 2 && landmark_distance(at, as.stall_pos) < 0.2f; memcpy(as.stall_pos, at, 12); as.stall_t = now;
            if (stuck) { char why[160]; snprintf(why, sizeof why, "stopped %.1f m short of the entrance lift: the way in is blocked (closed, sludge or rubble?)", d); as_diagnose(AS_WALK, now, why); return; }
        }
        if (t > 8) { char why[120]; snprintf(why, sizeof why, "did not reach the entrance lift in 8 s (%.1f m from it)", d); as_diagnose(AS_WALK, now, why); return; }
        as_walk(actor, s->lift_floor);
        return;
    }
    case AS_LIFT: {   /* step 3: the lift is an A prompt (FldObj_EnterAttetionActor, 2Go): face it, ZL, A; outcome: the shrine's map opens (3 s) */
        uint32_t seen = as.seq_inside;
        if (atomic_load(&as_seq_inside) != seen || (have && landmark_distance(at, s->start) < 60)) { as_go(AS_LOADING, now, "the shrine's map opened"); as.seq_inside = seen; return; }
        if (as.phase == 0) { as_face(actor, s->lift); as.phase = 1; }
        else if (as.phase == 1 && t >= 0.3) { runtime_input_lease_release_all(); as.phase = 2; }   /* nothing held before A: a clean press */
        else if (as.phase == 2 && t >= 0.8) { as_key(AS_KEY_A, 0.25); fprintf(stderr, "[autoshrine] A at the entrance lift\n"); as.phase = 3; as.phase_t = now; }
        else if (as.phase == 3 && now - as.phase_t > 5 && as_ago(&as_map_ns) > 5) as_diagnose(AS_LIFT, now, "no map load 5 s after the lift's A");
        return;
    }
    case AS_LOADING:   /* outcome: Link stands in the shrine's own coordinates for 2 s (30 s from the map's opening) */
        if (!as.event && atomic_load(&as_seq_inside) != as.seq_inside) {
            int got = atomic_load(&as_inside_index);
            if (got != as.k) { char text[80]; snprintf(text, sizeof text, "the game loaded Dungeon%03d instead", got); as_abandon(text, now); return; }
            as.event = 1; fprintf(stderr, "[autoshrine] the shrine's map is loading\n");
        }
        if (have && landmark_distance(at, s->start) < 80) {
            if (!as.have_last || landmark_distance(at, as.last) > 5) { memcpy(as.last, at, 12); as.have_last = 1; as.phase_t = now; }
            if (now - as.phase_t >= 2) {
                fprintf(stderr, "[autoshrine] inside after %.0f s at %.1f,%.1f,%.1f\n", t, at[0], at[1], at[2]);
                as.have_safe = 0; as.inside = 1; memcpy(as.expect, at, 12); as_go(AS_INSIDE, now, "inside the shrine"); return;
            }
        } else as.have_last = 0;
        if (t > 30) as_diagnose(as.event ? AS_LOADING : AS_LIFT, now, as.event ? "the shrine's map opened but Link was not in it within 30 s" : "no shrine map within 30 s");
        return;
    case AS_INSIDE:   /* 3Arrive (and on Great Sky Island the ability's scene): skipped, then step 4 */
        as_scene(AS_INSIDE, 0, now, "the arrival scene");
        return;
    case AS_CHEST:   /* A, then B for the item's screen; no signal says it opened: best effort */
        if (as.phase == 0) { as_face(actor, s->chest[as.chest]); as.phase = 1; }
        else if (as.phase == 1 && t >= 0.3) { as_key(AS_KEY_A, 0.15); fprintf(stderr, "[autoshrine] chest %d of %d: A\n", as.chest + 1, s->nchests); as.phase = 2; }
        else if (as.phase >= 2 && as.phase < 5 && t >= 3.5 + (as.phase - 2) * 1.5) { as_key(AS_KEY_B, 0.1); as.phase++; }
        else if (as.phase == 5 && t >= 8) { as.chest++; as_after_inside(now); }
        return;
    case AS_GOAL: {   /* step 5: on SmallDungeonExit (0.75 m behind DgnObj_GoalExit, the prompt), face the altar, ZL, A; the skipping starts as A
                         lands, and AS_SCENE checks that the event began (its file opens or he is pulled to the altar) */
        if (atomic_load(&as_seq_field) != as.seq_field) { as.back = 0; as.have_safe = 0; as.inside = 0; as_stage(6); as_go(AS_SURFACE, now, "the surface's map is loading"); return; }
        if (!as.approached) {
            if (have && as_xz(at, s->goal_floor) > 1.5f) {   /* not by the prompt: walk up (a recovery may have moved him) */
                if (as_walk_up(actor, s->goal_floor, t, now, 4) && as_xz(at, s->goal_floor) > 1.5f) { as_diagnose(AS_GOAL, now, "could not stand by the altar's prompt"); return; }
                if (as_xz(at, s->goal_floor) > 1.5f) return;
            }
            as.approached = 1; as.phase = 0; as.phase_t = now; as_stage(5);
        }
        double c = now - as.phase_t;
        if (as.phase == 0) { as_face(actor, s->goal); as.phase = 1; }
        else if (as.phase == 1 && c >= 0.3) { runtime_input_lease_release_all(); as.phase = 2; }   /* nothing held before A: a clean press */
        else if (as.phase == 2 && c >= 0.8) {
            as_key(AS_KEY_A, 0.25); fprintf(stderr, "[autoshrine] A at the altar's prompt\n");
            if (have) memcpy(as.a_pos, at, 12);
            as_stage(6); as_scene(AS_GOAL, 1, now, "the altar's A: skipping its scene from the start");
        }
        return;
    }
    case AS_LEAVE:   /* step 6: a cleared shrine's altar asks "Do you want to leave?": "I'm ready to go." is the default and what B picks
                        (ChoiceDefaultNo 0, ChoiceCancelNo 0): face the altar, ZL, A, A; outcome: the surface's map (8 s) */
        if (atomic_load(&as_seq_field) != as.seq_field || (have && landmark_distance(at, s->entrance) < 200)) { as.back = 0; as.have_safe = 0; as.inside = 0; as_go(AS_SURFACE, now, "the surface's map is loading"); return; }
        if (as.phase == 0) { as_face(actor, s->goal); as.phase = 1; }
        else if (as.phase == 1 && t >= 0.3) { runtime_input_lease_release_all(); as.phase = 2; }   /* nothing held before A: a clean press */
        else if (as.phase == 2 && t >= 0.8) { as_key(AS_KEY_A, 0.25); as.phase = 3; }
        else if (as.phase == 3 && t >= 2.0) { as_key(AS_KEY_A, 0.15); fprintf(stderr, "[autoshrine] A, A at the altar (\"I'm ready to go.\")\n"); as.phase = 4; }
        else if (as.phase == 4 && t >= 8) as_diagnose(AS_LEAVE, now, "the altar did not take Link out");
        return;
    case AS_BACK: {   /* step 7: walk onto the start lift, face its prompt, A (every 2 s); outcome: the surface's map (6 s on the lift) */
        if (atomic_load(&as_seq_field) != as.seq_field || (have && landmark_distance(at, s->entrance) < 200)) { as.back = 1; as.have_safe = 0; as.inside = 0; as_stage(6); as_go(AS_SURFACE, now, "out by the start lift"); return; }
        float d = have ? as_xz(at, s->back_floor) : 99;
        if (as.phase == 0) {
            if (d < 1.2f || t > 8) { as.phase = 1; as.phase_t = now; if (d >= 1.2f) { as_diagnose(AS_BACK, now, "did not reach the start lift in 8 s"); return; } }
            else { as_walk(actor, s->back_floor); return; }
        }
        double c = now - as.phase_t;
        if (fmod(c, 2.0) < dt) { as_face(actor, s->back); as.event++; }
        if (fmod(c, 2.0) >= 0.4 && fmod(c, 2.0) < 0.4 + dt) { as_key(AS_KEY_A, 0.15); fprintf(stderr, "[autoshrine] A at the start lift\n"); }
        if (c > 6 && as_ago(&as_map_ns) > 3) as_diagnose(AS_BACK, now, "no map load 6 s on the start lift");
        return;
    }
    case AS_SURFACE:   /* step 6: outcome: Link back in the surface's coordinates (30 s); then the exit scene, skipped */
        if (have && (landmark_distance(at, s->entrance) < 200 || landmark_distance(at, s->start) > 1000)) {
            fprintf(stderr, "[autoshrine] outside at %.1f,%.1f,%.1f, %.0f m from the entrance\n", at[0], at[1], at[2], landmark_distance(at, s->entrance));
            memcpy(as.safe, at, 12); as.have_safe = 1; memcpy(as.expect, at, 12); as_scene(AS_SAVED, 0, now, "outside: the exit scene"); return;
        }
        if (t > 30) as_diagnose(AS_SURFACE, now, "Link did not appear outside within 30 s");
        return;
    case AS_SAVED: {   /* step 6 ends: done when the altar's event was seen and took him out (its branch always sets Clear before it
                          does; the save is read at the next choice); after the start lift, up to 10 s for a save that says Clear */
        const char *name = as_mark[LANDMARK_FIRST_SHRINE + as.k].name;
        if (as.recovering) {   /* out of a shrine given up: on to the next */
            as.recovering = 0; as.inside = 0;
            if (as.one || as.target >= 0) { as_end(0, as.skipped_list, now, 0); return; }
            if (as.fails_alike >= AS_FAILS_ALIKE) { as_end(0, "Stopped: shrines given up in a row for the same reason", now, 0); return; }
            as_stage(8); as_go(AS_PICK, now, "out of the shrine given up; the next one"); return;
        }
        if (!as.goal_ok || as.back) {
            if (fmod(t, 1.0) >= dt && t < 10) return;
            double saved = as_save_time();
            if (saved > as.save_time) { static Landmark lm[LANDMARKS]; as_read_save(lm); as.save_time = saved; }
            if (as.state[as.k] != SHRINE_STATE_CLEAR) {
                if (t < 10) return;
                as.inside = 0; as_abandon(as.back ? "left by the start lift and no save says Clear" : "the altar's event was not seen and no save says Clear", now); return;
            }
            fprintf(stderr, "[autoshrine] done: %s (Dungeon%03d): the save says Clear\n", name, as.k);
        } else fprintf(stderr, "[autoshrine] done: %s (Dungeon%03d): the altar's event took Link out\n", name, as.k);
        as.run[as.k] = SHRINE_SKIP_DONE; as.done++; as.fails_alike = 0; as.fail_sig[0] = 0; as.verify_k = as.k;
        as_stage(8);
        as_go(AS_PICK, now, "done; the next shrine");
        return;
    }
    case AS_R_MENU:   /* B closes the pause menu (or a message); then look again. The third time A instead: the Game Over
                         screen's Continue (its event file was read once at start, so a game over shows only as Link held still) */
        if (as.phase == 0) {
            if (as.rec[REC_MENU] >= 3) { as_key(AS_KEY_A, 0.25); fprintf(stderr, "[autoshrine] A (Continue, if this is the Game Over screen)\n"); as.phase = 2; as.phase_t = now; if (have) memcpy(as.probe, at, 12); }
            else { as_key(AS_KEY_B, 0.12); fprintf(stderr, "[autoshrine] B (menu)\n"); as.phase = 1; }
        }
        else if (as.phase == 1 && t >= 0.8) as_go(AS_DIAGNOSE, now, "look again after B");
        else if (as.phase == 2) {   /* a game over reloads the last save: a map load, or Link somewhere else */
            if (as_ago(&as_map_ns) < 2 || !have || (have && landmark_distance(at, as.probe) > 15)) { as_go(AS_GAMEOVER, now, "GAME_OVER: Continue reloaded the save"); return; }
            if (now - as.phase_t > 4) as_go(AS_DIAGNOSE, now, "look again after A");
        }
        return;
    case AS_R_PROMPT: {   /* back off 2 m (straight back, then 30 degrees to one side, then the other), walk up to the prompt, back to the state */
        if (!as_prompt_of(as.ret)) { as_go(AS_R_UNKNOWN, now, "no prompt to work on"); return; }
        if (as.phase == 0) {
            float out[2]; shrine_unit_xz(as.p_face, as.p_floor, out);   /* from the prompt out through its floor point */
            float ang = as.rec[REC_PROMPT] == 2 ? 0.5236f : as.rec[REC_PROMPT] == 3 ? -0.5236f : 0, ca = cosf(ang), sa = sinf(ang);
            float ox = out[0] * ca - out[1] * sa, oz = out[0] * sa + out[1] * ca;
            as.probe[0] = as.p_floor[0] + 2 * ox; as.probe[1] = as.p_floor[1]; as.probe[2] = as.p_floor[2] + 2 * oz;
            as.phase = 1; as.phase_t = now;
            fprintf(stderr, "[autoshrine] backing off to %.1f,%.1f,%.1f (%.0f degrees)\n", as.probe[0], as.probe[1], as.probe[2], ang * 57.3f);
        }
        if (as.phase == 1) { if (as_walk_up(actor, as.probe, now - as.phase_t, now, 3)) { as.phase = 2; as.phase_t = now; } return; }
        if (as.phase == 2) { as_face(actor, as.p_face); as.phase = 3; as.phase_t = now; return; }
        if (as.phase == 3) {
            if (now - as.phase_t < 0.3) return;
            if (as_walk_up(actor, as.p_floor, now - as.phase_t - 0.3, now, 4)) { as.approached = 1; as_return(now, "walked up to the prompt again"); }
            return;
        }
        return;
    }
    case AS_R_UNKNOWN:   /* keys let go, B twice, noclip off, then look once more */
        if (as.phase == 0) { runtime_input_lease_release_all(); if (atomic_load(&noclip_wanted)) runtime_noclip_set(0); as_key(AS_KEY_B, 0.12); as.phase = 1; }
        else if (as.phase == 1 && t >= 0.6) { as_key(AS_KEY_B, 0.12); as.phase = 2; }
        else if (as.phase == 2 && t >= 1.5) { as.unknown_seen = 1; as_go(AS_DIAGNOSE, now, "look again"); }
        return;
    case AS_GAMEOVER:   /* the Game Over screen (LayoutMsg/GameOver_00: "Continue" first, "Quit"): A on Continue every 2 s until he
                           stands again (60 s); then the shrine is given up, Link's place re-read, the next one chosen */
        if (have && t > 3 && !(fabsf(at[0]) < 1 && fabsf(at[2]) < 1)) {
            if (!as.have_last || landmark_distance(at, as.last) > 0.5f) { memcpy(as.last, at, 12); as.have_last = 1; as.phase_t = now; }
            if (now - as.phase_t >= 3) {
                static Landmark lm[LANDMARKS]; as_read_save(lm);
                fprintf(stderr, "[autoshrine] back after the game over at %.1f,%.1f,%.1f\n", at[0], at[1], at[2]);
                as.inside = landmark_distance(at, s->start) < 600 && landmark_distance(at, s->entrance) > 300;   /* reloaded inside the shrine? */
                as.have_safe = 1; memcpy(as.safe, at, 12);
                as_abandon("Link died (game over)", now); return;
            }
        }
        if (fmod(t, 2.0) < dt) { as_key(AS_KEY_A, 0.15); fprintf(stderr, "[autoshrine] A (Continue)\n"); }
        if (t > 60) as_end(0, "Stopped: the game over screen did not go away (60 s)", now, 0);
        return;
    case AS_STOPPING:   /* back to where he last stood (a direct move when far: held there until the ground has streamed in), then down */
        if (t < 60 && atomic_load(&noclip_wanted)) {
            pthread_mutex_lock(&noclip_goto_lock); int pending = noclip_goto.pending; pthread_mutex_unlock(&noclip_goto_lock);
            if (pending || noclip_flight.on || !noclip.holding) return;
            float d = landmark_distance(noclip.xyz, as.stand), step = 30 * (float)dt;
            if (d > step) { for (int c = 0; c < 3; c++) noclip.xyz[c] += (as.stand[c] - noclip.xyz[c]) * step / d; return; }
            memcpy(noclip.xyz, as.stand, 12);
            if (as.flight) {
                if (!as.phase) { as.phase = 1; as.phase_t = now; }
                if (!as_streamed(now - as.phase_t, 1.0)) return;
                as.flight = 0;
            }
        }
        if (atomic_load(&noclip_wanted)) runtime_noclip_set(0);
        fprintf(stderr, "[autoshrine] set down\n");
        if (as.resume) { as.resume = 0; dm_gfx_camera_want(1); as_stage(8); as_go(AS_PICK, now, "the next shrine"); return; }
        dm_gfx_camera_want(0);
        as_go(AS_OFF, now, "set down"); atomic_store(&as_active, 0); as_say();
        return;
    }
}
/* Reads the shrines (and their names and story places) from the RomFS once, then asks the frame-end thread to start. */
/* Script mode (scripts/autoshrine.py drives the game through the input file; the flow is edited there, no rebuild):
   "autoshrine info N" prints shrine N's places, "autoshrine face X Y Z" turns Link to a point, "autoshrine walk X Y Z MS"
   walks him toward it (WASD from the camera) for MS milliseconds, "autoshrine watch on|off" logs the map and event files
   the game opens ("[as-file] ..."). The frame-end thread carries out face and walk. */
static _Atomic int as_script_watch;
static pthread_mutex_t as_cmd_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int kind; float to[3]; double until; } as_cmd;   /* kind 1 face, 2 walk */
static int as_load_sync(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&as_loaded, &expected, 1)) {
        LandmarkRomfs rom = {runtime_romfs_file_count, runtime_romfs_file_name, runtime_romfs_host_read, runtime_romfs_zs_decompress};
        int places = landmarks_load(&rom, as_mark), ready = places ? shrines_load(&rom, as_shrine) : 0;
        atomic_store(&as_loaded, ready ? 2 : 3);
    }
    while (atomic_load(&as_loaded) == 1) usleep(50000);
    return atomic_load(&as_loaded) == 2;
}
static void as_script_tick(uintptr_t actor, double now) {
    pthread_mutex_lock(&as_cmd_lock); int kind = as_cmd.kind; float to[3]; memcpy(to, as_cmd.to, 12); double until = as_cmd.until;
    if (kind == 1 || kind == 3 || (kind == 2 && now >= until)) as_cmd.kind = 0;
    pthread_mutex_unlock(&as_cmd_lock);
    if (!kind) return;
    if (kind == 1) { as_face(actor, to); fprintf(stderr, "[as-done] face\n"); return; }
    if (kind == 3) {
        float at[3] = {0}, rot[9] = {0}, cam[3] = {0}, right[3] = {0}; uint64_t fr = 0;
        int have = as_pos(actor, at) && read_mem(actor + ACTOR_ROT, rot, sizeof rot), camok = dm_gfx_camera(cam, right, &fr);
        fprintf(stderr, "[as-pose] %s pos=%.2f,%.2f,%.2f fwd=%.3f,%.3f cam=%.2f,%.2f,%.2f right=%.3f,%.3f%s\n", have ? "ok" : "none", at[0], at[1], at[2],
                rot[2], rot[8], cam[0], cam[1], cam[2], right[0], right[2], camok ? "" : " nocam");
        return;
    }
    if (now >= until) { as_walk(0, NULL); float at[3]; if (as_pos(actor, at)) fprintf(stderr, "[as-done] walk at %.2f,%.2f,%.2f\n", at[0], at[1], at[2]); else fprintf(stderr, "[as-done] walk\n"); return; }
    as_walk(actor, to); atomic_store_explicit(&runtime_survey_pending, 1, memory_order_release);
}
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
    if (atomic_load(&ak_active)) { fprintf(stderr, "[autoshrine] not started: auto korok is running\n"); return 0; }
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
/* Places › Shrines' "Retry given-up shrines" (and "autoshrine givenup reset"): autoshrine-given-up.txt removed; a run on now
   reads it again at its next choice. Returns how many shrines it named. */
int runtime_autoshrine_given_up_reset(void) {
    ShrineGivenUp *g = calloc(1, sizeof *g); if (!g) return 0;
    int n = shrine_given_up_load(g); char path[1024]; int have = shrine_given_up_path(path, sizeof path);
    int gone = have && (remove(path) == 0 || errno == ENOENT);
    free(g);
    fprintf(stderr, "[autoshrine] given-up shrines reset: %d shrine%s will be tried again%s%s\n", n, n == 1 ? "" : "s", gone ? "" : "; [red] could not remove ", gone ? "" : have ? path : "the file (no profile folder)");
    return n;
}
void runtime_autoshrine_set_chests(int on) { atomic_store(&as_chests, !!on); fprintf(stderr, "[autoshrine] chests %s\n", on ? "on" : "off"); }
int runtime_autoshrine_chests(void) { return atomic_load(&as_chests); }
int runtime_autoshrine_available(void) { return noclip_force != NULL; }
int runtime_autoshrine_status(char *text, size_t size) {
    pthread_mutex_lock(&as_status_lock); snprintf(text, size, "%s", as_status); pthread_mutex_unlock(&as_status_lock);
    return atomic_load(&as_active);
}
/* Game control for scripts (the input file; scripts/autoshrine.py is one user, any task can be driven the same way). Every
   line also works as "autoshrine <rest>"; answers carry [ctl-...] (or [as-...] for the autoshrine spelling).
     input   ctl press <buttons> [ms] | hold <buttons> | release <buttons>|all | stick l|r <x> <y> [ms]   (input.c)
     move    ctl face X Y Z | walk X Y Z MS (Link turned / walked toward a point) | noclip on|off | goto X Y Z (teleport)
     read    ctl state (one line, below) | peek <base> <offsets...> <len> | info N | watch on|off (shrine data, files)
     game    ctl event <EventName> (1.4.3: the game's EventMgr::requestEvent, see ctl_event below)
   "ctl state" (1.4.3): [ctl-state] event=0|1|? evname=<name>|- map=<current map>|? stable=0|1|? hp=<cur>/<max>|?
   prompt=<type>:<actor>|none|? attn=<state>/<count> pos=x,y,z face=x,z cam=x,y,z camright=x,z|cam=?
   Where each value comes from (1.4.3 main, static analysis; the tag after it says how sure):
     event   [EventMgr::sInstance]+0x58 != 0: exactly what the AI query QueryIsPlayingMainEvent returns (its doQuery
             0x2686e10; 1.0.0's 0x29555e4 reads 0x464a988 = totk-lotuskit's EventMgr::sInstance). Certain.
     evname  EventMgr+0xb0, a FixedSafeString<128> (mCurrentEventName in totk-lotuskit's EventMgr layout). TOTK's sead
             strings have no vtable: requestEvent's worker 0xcac698 reads the text pointer at +0xb0 and the buffer size
             at +0xb8, so the text is at +0xbc; printed only when the pointer points there ("-" otherwise). Candidate.
     map     QueryGetCurrentNormalizedBancName's reader (0x1995210): o = [[banc]+0x28], i = [o+0x150], ready when byte
             i+0x224 bit 0 is set, the name at [i+0x158] ("MainField", "MinusField", "SmallDungeon/Dungeon009"-like); the
             game falls back to the path at [banc]+0x30, printed as-is. Static, verify.
     stable  QueryBancLoadMgrIsStable (0x25e18b4): bit 2 of u32 +0xed30 in scene component 6 (scene as for the player:
             [[main+SCENE_SLOT]+0x1e8], components [scene+0x58]). 0 while the map streams in. Static, verify.
     hp      Life component: [[player+0x228]+0x2f8] (component 0x5f, the game's own getter 0x4f8d78), current life at
             [life+0x1830]+0x8 (QueryGetLife 0x2659c7c); max at +0x18 of that object (the cheat list's Inf. Health copies
             +0x18 into +0x8). Quarter hearts. Current certain, max candidate.
     prompt  the player's attention component [[player+0x228]+0x128] (0x194f714, AttentionReceiverRef 0x25 in lotuskit's
             enum) as the DoAction lookup 0x5e2d54 reads it: state u32 +0xe8 (2 or 3: picking), u32 +0x58 entries,
             entry [[+0x60]] -> client [entry]: action type u32 +0x34, actor [client+0x8]. Printed as type:actor name.
             Candidate: which type is Examine / Talk / Open is to be learnt live.
     face    the actor's rotation (ACTOR_ROT) third column, as noclip walks; cam: the G-buffer pass's camera
             (dm_gfx_camera, turned on at the first state line, so the first one may say cam=?).
   "ctl peek <base> <off>... <len>": base main | player | event | banc | a hex address; every offset but the last is added
   and then read as a pointer, the last only added; up to 256 bytes printed as hex, u32 and float (and the text). A chain
   that breaks says where. Reads only (mach_vm_read): a bad address is reported, never touched. */
#if defined(TOTK_GAME_V143)
enum { STATE_EVENT_MGR = GAME_STATE_EVENT_MGR, STATE_BANC = GAME_STATE_BANC };
#endif
enum { ACTOR_COMPONENTS = 0x228, COMPONENT_LIFE = 0x5f, COMPONENT_ATTENTION = 0x25, SCENE_BANC_LOAD = 6 };
/* A C string at p (up to size - 1 bytes, read in pieces so a string near the end of a mapping still reads); 0 if none. */
static int ctl_text(uintptr_t p, char *out, size_t size) {
    size_t n = 0;
    out[0] = 0;
    while (p && n + 1 < size) {
        char piece[16]; size_t want = sizeof piece < size - 1 - n ? sizeof piece : size - 1 - n;
        if (!read_mem(p + n, piece, want)) break;
        size_t k = 0;
        while (k < want && piece[k]) k++;
        memcpy(out + n, piece, k); n += k; out[n] = 0;
        if (k < want) break;
    }
    for (size_t i = 0; i < n; i++) if ((unsigned char)out[i] < 0x21 || (unsigned char)out[i] > 0x7e) out[i] = '_';   /* one word in the line */
    return n > 0;
}
static uintptr_t ctl_component(uintptr_t actor, unsigned index) {
    uintptr_t list = actor ? read_ptr(actor + ACTOR_COMPONENTS) : 0;
    return list ? read_ptr(list + index * 8u) : 0;
}
#if defined(TOTK_GAME_V143)
/* Noclip's own god mode (2026-10-28): a player held in the air by a teleport was killed there (an enemy, the cold or
   the heat), with no health cheat on. While noclip is on, and for NOCLIP_GUARD_S seconds after it goes off (the landing),
   Link's life is set back to his maximum whenever it drops, as the cheat list's Inf. Health does (current +0x8 = max
   +0x18 of the life object, see ctl state's hp). A single blow larger than all his hearts can still kill between two
   frames: the god mode patch (noclip_godmode, below) drops such damage before the game counts it; the refill stays as a
   second line. Writes go through mach_vm_write, so a stale pointer fails instead of faulting.
   Fall damage: a teleport's flight climbs, and touching the ground at the target counted as a fall from the top of it
   (a 260 m "fall" killed at once, 2026-10-28; high falls kill whatever the hearts). The game keeps where the fall began
   in the player's fall calculator: 1.4.3 0x1cd8500 (called as player component + 0x1218 from 0x301260) reads its actor at
   +0x8 and the start height at +0x560 and stores the fall so far, start minus his height (actor +0x2b8), at +0x564.
   While guarded the start is set to where he is and the fall to 0 every frame, as afiaka87/totk-mods self-recall does
   for its own teleports (MIT; RecallMovement.cpp FallHeightUpdateHook, its 1.4.3 address of the same function). The
   component holding it: self-recall's [actor+0x228]+0x3a8 (index 0x75) was not it in 1.4.3 ("not found", 2026-10-28),
   so the player's components are searched for the one whose +0x1218 calculator names this actor at +0x8 (the game's own
   code reads his height and components through it), and that index is kept.
   Until he stops (2026-10-28, user: "until link actually lands"; killed three times switching noclip off just above the
   ground): the guard lasts from noclip on until NOCLIP_STILL_FRAMES frames in a row without his height changing by
   NOCLIP_STILL_M (stood, or holding on), then NOCLIP_GUARD_S seconds more; no fixed time, NOCLIP_SETTLE_S at most. */
enum { NOCLIP_GUARD_S = 2, NOCLIP_STILL_FRAMES = 20, NOCLIP_SETTLE_S = 120, PLAYER_FALL = 0x1218, FALL_ACTOR = 0x8, FALL_START = 0x560, FALL_SO_FAR = 0x564,
       PLAYER_COMPONENTS_SEARCHED = 0x200 };
#define NOCLIP_STILL_M 0.01f
static uintptr_t noclip_fall_calculator(uintptr_t actor) {
    static int index = -1, said; static unsigned looks;
    if (index >= 0) {
        uintptr_t c = ctl_component(actor, (unsigned)index);
        if (c && read_ptr(c + PLAYER_FALL + FALL_ACTOR) == actor) return c + PLAYER_FALL;
        index = -1;
    }
    if (looks++ % 30) return 0;   /* a search about twice a second until found */
    for (unsigned i = 0; i < PLAYER_COMPONENTS_SEARCHED; i++) {
        uintptr_t c = ctl_component(actor, i); float start;
        if (!c || read_ptr(c + PLAYER_FALL + FALL_ACTOR) != actor || !read_mem(c + PLAYER_FALL + FALL_START, &start, 4) || !isfinite(start)) continue;
        index = (int)i;
        fprintf(stderr, "[noclip] fall calculator: component %#x + %#x (%#llx), fall start %.1f\n", i, PLAYER_FALL, (unsigned long long)(c + PLAYER_FALL), start);
        return c + PLAYER_FALL;
    }
    if (!said) { said = 1; fprintf(stderr, "[noclip] [red] fall calculator not found in the player's first %d components: falls are not reset\n", PLAYER_COMPONENTS_SEARCHED); }
    return 0;
}
static void noclip_no_fall(uintptr_t actor) {
    uintptr_t fall = noclip_fall_calculator(actor);
    float y; struct { float start, so_far; } reset;
    if (!fall || !read_mem(actor + ACTOR_POS + 4, &y, 4) || !isfinite(y)) return;
    reset.start = y; reset.so_far = 0;
    mach_vm_write(mach_task_self(), fall + FALL_START, (vm_offset_t)&reset, sizeof reset);
}
/* God mode while guarded (game_patches_v143.c patch_noclip_damage, Mac): the player's actor on the data page makes the game
   drop every damage packet registered for him, whatever its size (a fall from any height, a blow); 0 lets damage through
   again. Kept through a frame without a player while the guard is on; cleared on the tick the guard ends (the tick keeps
   running until then, runtime_survey_tick). The fall reset and the life refill below stay as a second line. */
static void noclip_godmode(uintptr_t actor, int on, uint64_t now) {
    _Atomic uint64_t *block = runtime_noclip_damage_block;
    static uint32_t seen; static uint64_t said_ns;
    if (!block || (on && !actor)) return;
    uint64_t want = on ? actor : 0, was = atomic_load(block);
    if (was != want) {
        atomic_store(block, want);
        if (!was || !want) fprintf(stderr, "[noclip] god mode %s\n", want ? "on: Link takes no damage until the guard ends" : "off: the guard is over");
    }
    const volatile uint32_t *drop = (const volatile uint32_t *)(const void *)((const uint8_t *)block + 8);   /* amount, type, count */
    uint32_t count = drop[2];
    if (count != seen && now - said_ns >= 1000000000ull) {
        fprintf(stderr, "[noclip] damage dropped: type %u amount %d (guard %s; %u dropped so far)\n", drop[1], (int32_t)drop[0],
                on ? "on" : "ending", count);
        seen = count; said_ns = now;
    }
}
static void noclip_guard(uintptr_t actor) {
    static unsigned refills; uint64_t now = runtime_now_ns();
    /* auto korok keeps Link guarded through each korok's steps (walking, a lift, the discovery event), not only in noclip */
    int flying = atomic_load(&noclip_wanted) || noclip.holding;
    if (flying) { atomic_store(&noclip_settle.on, 1); noclip_settle.still = 0; noclip_settle.y = NAN; noclip_settle.t0 = now; }
    else if (atomic_load(&noclip_settle.on) && actor) {   /* let go: guarded until his height stays put */
        float y;
        if (read_mem(actor + ACTOR_POS + 4, &y, 4) && isfinite(y)) {
            noclip_settle.still = isfinite(noclip_settle.y) && fabsf(y - noclip_settle.y) < NOCLIP_STILL_M ? noclip_settle.still + 1 : 0;
            noclip_settle.y = y;
        }
        int timeout = now - noclip_settle.t0 > NOCLIP_SETTLE_S * 1000000000ull;
        if (noclip_settle.still >= NOCLIP_STILL_FRAMES || timeout) {
            atomic_store(&noclip_settle.on, 0);
            fprintf(stderr, "[noclip] %s at height %.1f, %.1f s after noclip: guarded %d s more\n", timeout ? "still moving" : "Link stopped", noclip_settle.y,
                    (double)(now - noclip_settle.t0) * 1e-9, NOCLIP_GUARD_S);
        }
    }
    if (flying || atomic_load(&noclip_settle.on) || atomic_load(&ak_active)) atomic_store(&noclip_guard_until_ns, now + NOCLIP_GUARD_S * 1000000000ull);
    int guarded = now < atomic_load(&noclip_guard_until_ns);
    noclip_godmode(actor, guarded, now);
    if (!guarded || !actor) return;
    if (!atomic_load(&ak_no_fall)) noclip_no_fall(actor);
    uintptr_t life_component = ctl_component(actor, COMPONENT_LIFE), life = life_component ? read_ptr(life_component + 0x1830) : 0;
    int32_t hp[2];
    if (!life || !read_mem(life + 0x8, &hp[0], 4) || !read_mem(life + 0x18, &hp[1], 4)) return;
    if (hp[1] < 4 || hp[1] > 400 || hp[0] >= hp[1] || hp[0] < 0) return;   /* quarter hearts: 1 to 100 hearts */
    if (mach_vm_write(mach_task_self(), life + 0x8, (vm_offset_t)&hp[1], 4) == KERN_SUCCESS && (!refills++ || refills % 50 == 0))
        fprintf(stderr, "[noclip] life %d -> %d quarter hearts (%s; %u refills)\n", hp[0], hp[1], atomic_load(&noclip_wanted) ? "noclip on" : "landing", refills);
}
#endif
static void ctl_state(const char *tag) {
    char line[900]; size_t o = 0;
    #define CTL_ADD(...) do { if (o < sizeof line) o += (size_t)snprintf(line + o, sizeof line - o, __VA_ARGS__); } while (0)
    uintptr_t actor = survey_main ? player_actor() : 0;
#if defined(TOTK_GAME_V143)
    uintptr_t mgr = survey_main ? read_ptr(survey_main + STATE_EVENT_MGR) : 0, current = 0;
    if (mgr && read_mem(mgr + 0x58, &current, sizeof current)) {
        char name[132] = "-";
        if (current && read_ptr(mgr + 0xb0) == mgr + 0xbc && !ctl_text(mgr + 0xbc, name, sizeof name)) snprintf(name, sizeof name, "-");
        CTL_ADD(" event=%d evname=%s", current != 0, name);
    } else CTL_ADD(" event=? evname=-");
    uintptr_t banc = survey_main ? read_ptr(survey_main + STATE_BANC) : 0, holder = banc ? read_ptr(banc + 0x28) : 0;
    uintptr_t info = holder ? read_ptr(holder + 0x150) : 0;
    uint8_t ready = 0; char map[160] = "";
    if (info && read_mem(info + 0x224, &ready, 1) && (ready & 1)) ctl_text(read_ptr(info + 0x158), map, sizeof map);
    if (!map[0] && banc) ctl_text(read_ptr(banc + 0x30), map, sizeof map);
    CTL_ADD(" map=%s", map[0] ? map : "?");
    uintptr_t module = survey_main ? read_ptr(survey_main + SCENE_SLOT) : 0, scene = module ? read_ptr(module + 0x1e8) : 0;
    int count = 0; uint32_t flags = 0; uintptr_t loader = 0;
    uintptr_t components = scene && read_mem(scene + 0x50, &count, 4) && count > SCENE_BANC_LOAD && count <= 256 ? read_ptr(scene + 0x58) : 0;
    if (components) loader = read_ptr(components + SCENE_BANC_LOAD * 8u);
    if (loader && read_mem(loader + 0xed30, &flags, 4)) CTL_ADD(" stable=%u", flags >> 2 & 1); else CTL_ADD(" stable=?");
#else
    CTL_ADD(" event=? evname=- map=? stable=?");
#endif
    uintptr_t life_component = ctl_component(actor, COMPONENT_LIFE), life = life_component ? read_ptr(life_component + 0x1830) : 0;
    int32_t hp[2] = {0, 0};   /* life object +0x8 and +0x18 */
    if (life && read_mem(life + 0x8, &hp[0], 4) && read_mem(life + 0x18, &hp[1], 4)) CTL_ADD(" hp=%d/%d", hp[0], hp[1]); else CTL_ADD(" hp=?");
    uintptr_t attention = ctl_component(actor, COMPONENT_ATTENTION);
    uint32_t attn = 0, entries = 0;
    if (attention && read_mem(attention + 0xe8, &attn, 4) && read_mem(attention + 0x58, &entries, 4)) {
        uintptr_t first = (attn & ~1u) == 2 && entries ? read_ptr(attention + 0x60) : 0;
        uintptr_t entry = first ? read_ptr(first) : 0, client = entry ? read_ptr(entry) : 0;
        uint32_t type = 0; char who[80] = "";
        if (client && read_mem(client + 0x34, &type, 4) && type) {
            uintptr_t target = read_ptr(client + 0x8);
            if (!target || !ctl_text(read_ptr(target + GAME_ACTOR_NAME), who, sizeof who)) snprintf(who, sizeof who, "?");
            CTL_ADD(" prompt=%u:%s", type, who);
        } else CTL_ADD(" prompt=none");
        CTL_ADD(" attn=%u/%u", attn, entries);
    } else CTL_ADD(" prompt=? attn=?");
    float xyz[3], rot[9];
    if (actor && read_mem(actor + ACTOR_POS, xyz, sizeof xyz) && valid_xyz(xyz)) {
        CTL_ADD(" pos=%.2f,%.2f,%.2f", xyz[0], xyz[1], xyz[2]);
        if (read_mem(actor + ACTOR_ROT, rot, sizeof rot)) CTL_ADD(" face=%.3f,%.3f", rot[2], rot[8]);
    } else CTL_ADD(" pos=?");
    float cam[3], right[3]; uint64_t frame = 0;
    dm_gfx_camera_want(1);
    if (dm_gfx_camera(cam, right, &frame)) CTL_ADD(" cam=%.2f,%.2f,%.2f camright=%.3f,%.3f", cam[0], cam[1], cam[2], right[0], right[2]);
    else CTL_ADD(" cam=?");
    #undef CTL_ADD
    fprintf(stderr, "[%s-state]%s\n", tag, line);
}
static void ctl_peek(const char *tag, const char *args) {
    char word[40]; int used = 0; uintptr_t at = 0;
    unsigned long long steps[16]; int n = 0;
    if (sscanf(args, "%39s %n", word, &used) != 1) { fprintf(stderr, "[%s-peek] rejected: peek <main|player|event|banc|hex> <offsets...> <len>\n", tag); return; }
    if (!strcmp(word, "main")) at = survey_main;
    else if (!strcmp(word, "player")) at = survey_main ? player_actor() : 0;
#if defined(TOTK_GAME_V143)
    else if (!strcmp(word, "event")) at = survey_main ? read_ptr(survey_main + STATE_EVENT_MGR) : 0;
    else if (!strcmp(word, "banc")) at = survey_main ? read_ptr(survey_main + STATE_BANC) : 0;
#endif
    else { char *end; at = (uintptr_t)strtoull(word, &end, 16); if (*end) at = 0; }
    if (!at) { fprintf(stderr, "[%s-peek] base %s: none\n", tag, word); return; }
    const char *last = NULL;   /* offsets are hex (0x optional, a leading '-' subtracts), the length decimal */
    for (const char *p = args + used; n < 16;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n' || *p == '\r') break;
        char *end; int neg = *p == '-';
        unsigned long long v = strtoull(p + neg, &end, 16);
        if (end == p + neg || (*end && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r')) {
            fprintf(stderr, "[%s-peek] rejected: bad number at \"%.20s\"\n", tag, p); return;
        }
        last = p; steps[n++] = neg ? (unsigned long long)-(long long)v : v; p = end;
    }
    if (n < 2) { fprintf(stderr, "[%s-peek] rejected: needs at least one offset and a length\n", tag); return; }
    n--;
    unsigned len = (unsigned)strtoul(last, NULL, 10);
    if (!len || len > 256) { fprintf(stderr, "[%s-peek] rejected: length 1..256 bytes\n", tag); return; }
    for (int i = 0; i < n; i++) {
        at += (uintptr_t)steps[i];
        if (i == n - 1) break;
        uintptr_t next = read_ptr(at);
        if (!next) { fprintf(stderr, "[%s-peek] chain broken at step %d: [%#llx] unreadable or null\n", tag, i + 1, (unsigned long long)at); return; }
        at = next;
    }
    uint8_t buf[256];
    if (!read_mem(at, buf, len)) { fprintf(stderr, "[%s-peek] %#llx: unreadable\n", tag, (unsigned long long)at); return; }
    char line[2200]; size_t o = 0;
    o += (size_t)snprintf(line + o, sizeof line - o, "[%s-peek] %#llx len=%u hex=", tag, (unsigned long long)at, len);
    for (unsigned i = 0; i < len && o < sizeof line; i++) o += (size_t)snprintf(line + o, sizeof line - o, "%02x", buf[i]);
    if (o < sizeof line) o += (size_t)snprintf(line + o, sizeof line - o, " u32=");
    for (unsigned i = 0; i + 4 <= len && o < sizeof line; i += 4) { uint32_t v; memcpy(&v, buf + i, 4); o += (size_t)snprintf(line + o, sizeof line - o, "%s%u", i ? "," : "", v); }
    if (o < sizeof line) o += (size_t)snprintf(line + o, sizeof line - o, " f32=");
    for (unsigned i = 0; i + 4 <= len && o < sizeof line; i += 4) { float v; memcpy(&v, buf + i, 4); o += (size_t)snprintf(line + o, sizeof line - o, "%s%g", i ? "," : "", v); }
    if (o < sizeof line) o += (size_t)snprintf(line + o, sizeof line - o, " text=");
    for (unsigned i = 0; i < len && o + 1 < sizeof line; i++) line[o++] = buf[i] >= 0x21 && buf[i] <= 0x7e ? (char)buf[i] : '.';
    if (o >= sizeof line) o = sizeof line - 1;
    line[o] = 0;
    fprintf(stderr, "%s\n", line);
}
/* "ctl event <EventName>": asks the game to start an event by name, as totk-lotuskit's event::requestSimple(name) does
   (aquacluck/totk-lotuskit 59dcfcc, src/program/util/event.cpp: an EventRequestArg with only its name set, then
   engine::event::EventMgr::requestEvent(EventMgr::sInstance, arg); lotuskit runs it from its Framework::procDraw_ hook, on
   the game's main loop). Here it runs on the frame-end thread, where forceSetMatrix and the spawner already call game code.
   1.4.3 addresses (game_version.h GAME_EVENT_*), found through the game's own routine that requests "DmF_SY_WorldReset" or
   "DmF_SY_WorldResetForMemoryShortage" (1.0.0 0x1b05d58, 1.4.3 0x21c66fc, each the one function that builds a request from
   those strings): EventRequestArg's constructor, the name copied into it, requestEvent([EventMgr::sInstance], arg), then
   both actor links torn down. Static analysis of both mains:
     requestEvent       1.0.0 0x11e5300 (lotuskit's symbol) -> 0xcac4cc: the same body (false when EventMgr+0xa8 bit 11 is
                        set; the no-starter path when arg+0x1960 == -1; bit 0x4000 of +0xa8 set on success), 11 direct
                        callers in both mains, x0 loaded from 0x3ada558 (the EventMgr::sInstance "ctl state" reads); its
                        worker 0xcac698 builds "Event/EventSetting/<name>" as 1.0.0's 0x11e5468 does.
     EventRequestArg()  1.0.0 0x101419c -> 0xce1108, called before each of 1.4.3's requestEvent calls: blackboard
                        InitInfo<32>, identity matrix, start frame -1, both links unbound (+0x1960 = -1), the strings. The
                        name is a FixedSafeString<128> without vtable: text pointer +0x1a90, size +0x1a98, text +0x1a9c.
     link teardown      [GOT 0x395d8d0] + 0x10 into +0x1968 and +0x1950, each then passed to 0x1aff9c (5004 callers).
   No entry point: lotuskit's EventRequestArg and every 1.4.3 caller set only the name; the game takes the rest from the
   event's EventSetting. No starter actor (lotuskit's optional one needs BaseProcLink::setProc, which does not map to 1.4.3
   reliably). true from requestEvent means accepted, not started: "ctl state" shows event=1 evname=<name> once it plays. */
#if defined(TOTK_GAME_V143)
enum { EVENT_ARG_SIZE = 0x1b20, EVENT_ARG_NAME = 0x1a90, EVENT_ARG_LINK1 = 0x1950, EVENT_ARG_LINK2 = 0x1968, EVENT_NAME_MAX = 127 };
static _Bool (*event_request)(void *mgr, const void *arg);
static void (*event_arg_init)(void *arg);
static void (*event_link_fini)(void *link);
static uintptr_t event_link_vtable;
static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;
static char event_want[EVENT_NAME_MAX + 1];
static _Atomic int event_waiting;
static void event_init(Module *m) {
    static const uint32_t request[] = {0xb940a808, 0x37580388, 0xd10103ff, 0xa9017bfd};
    static const uint32_t arg_init[] = {0xa9bc7bfd, 0xf9000bf7, 0xa90257f6, 0xa9034ff4};
    static const uint32_t link_fini[] = {0xa9bb7bfd, 0xf9000bf9, 0xa9025ff8, 0xa90357f6};
    const char *why = NULL; uintptr_t slot = 0;
    if (GAME_EVENT_REQUEST + sizeof request > m->text_end || memcmp(m->base + GAME_EVENT_REQUEST, request, sizeof request)) why = "requestEvent's entry differs";
    else if (GAME_EVENT_ARG_INIT + sizeof arg_init > m->text_end || memcmp(m->base + GAME_EVENT_ARG_INIT, arg_init, sizeof arg_init)) why = "EventRequestArg's constructor differs";
    else if (GAME_EVENT_LINK_FINI + sizeof link_fini > m->text_end || memcmp(m->base + GAME_EVENT_LINK_FINI, link_fini, sizeof link_fini)) why = "the link teardown differs";
    else if (GAME_EVENT_LINK_VTABLE_GOT + 8u > m->size || (memcpy(&slot, m->base + GAME_EVENT_LINK_VTABLE_GOT, sizeof slot), slot != (uintptr_t)(m->base + GAME_EVENT_LINK_VTABLE)))
        why = "the link vtable's GOT slot differs";
    if (why) { fprintf(stderr, "[event] off: %s from " TOTK_GAME_VERSION "\n", why); return; }
    event_request = (_Bool (*)(void *, const void *))(void *)(m->base + GAME_EVENT_REQUEST);
    event_arg_init = (void (*)(void *))(void *)(m->base + GAME_EVENT_ARG_INIT);
    event_link_fini = (void (*)(void *))(void *)(m->base + GAME_EVENT_LINK_FINI);
    event_link_vtable = slot + 0x10;
}
/* Frame-end thread (runtime_survey_tick). */
static void event_tick(void) {
    if (!atomic_load(&event_waiting)) return;
    static _Alignas(16) uint8_t arg[EVENT_ARG_SIZE];   /* 7 KB: the game keeps it on its stack; frame-end thread only */
    char name[EVENT_NAME_MAX + 1];
    pthread_mutex_lock(&event_lock); memcpy(name, event_want, sizeof name); pthread_mutex_unlock(&event_lock);
    void *mgr = (void *)read_ptr(survey_main + STATE_EVENT_MGR);
    uint32_t flags = 0; uintptr_t playing = 0;
    if (!mgr) fprintf(stderr, "[ctl-done] event %s refused: the game's EventMgr is not up yet\n", name);
    else {
        read_mem((uintptr_t)mgr + 0xa8, &flags, sizeof flags); read_mem((uintptr_t)mgr + 0x58, &playing, sizeof playing);
        event_arg_init(arg);
        char *text; int32_t size;
        memcpy(&text, arg + EVENT_ARG_NAME, sizeof text); memcpy(&size, arg + EVENT_ARG_NAME + 8, sizeof size);
        _Bool ok = 0; int laid_out = text == (char *)arg + EVENT_ARG_NAME + 0xc && size == EVENT_NAME_MAX + 1;
        if (laid_out) { memcpy(text, name, strlen(name) + 1); ok = event_request(mgr, arg); }
        const unsigned links[2] = {EVENT_ARG_LINK2, EVENT_ARG_LINK1};
        for (int i = 0; i < 2; i++) { memcpy(arg + links[i], &event_link_vtable, sizeof event_link_vtable); event_link_fini(arg + links[i]); }
        if (!laid_out) fprintf(stderr, "[ctl-done] event %s refused: the request's name field is not where 1.4.3 has it\n", name);
        else if (ok) fprintf(stderr, "[ctl-done] event %s\n", name);
        else fprintf(stderr, "[ctl-done] event %s refused: requestEvent returned false (EventMgr flags=%#x%s, main event playing=%d; "
                     "an unknown name, another event holding the manager, or a state that takes no requests)\n",
                     name, flags, flags & 0x801 ? ": bit 0 or 11, the ones it refuses on" : "", playing != 0);
    }
    atomic_store(&event_waiting, 0);
}
#endif
static void ctl_event(const char *args) {
    char name[160] = "", extra[64] = "";
    int n = sscanf(args, "%159s %63s", name, extra);
    if (n < 1) { fprintf(stderr, "[ctl] rejected: event <EventName>\n"); return; }
    if (n == 2) {
        fprintf(stderr, "[ctl-done] event %s refused: no entry point in TOTK's request (it carries only the name; the event's "
                "EventSetting picks the rest)\n", name);
        return;
    }
#if defined(TOTK_GAME_V143)
    size_t len = strlen(name); int valid = len <= EVENT_NAME_MAX;
    for (size_t i = 0; valid && i < len; i++) valid = isalnum((unsigned char)name[i]) || name[i] == '_' || name[i] == '.' || name[i] == '-';
    if (!valid) { fprintf(stderr, "[ctl-done] event %.40s refused: a name is up to %d letters, digits, _ . -\n", name, EVENT_NAME_MAX); return; }
    if (!event_request || !survey_main) { fprintf(stderr, "[ctl-done] event %s refused: not available (see the [event] line at startup)\n", name); return; }
    /* the frame-end tick runs when the spawner is on or with TOTK_SURVEY=1 (loader.c runtime_frame_controller_return_pc) */
    if (runtime_spawn_available() != SPAWN_OK && !survey_enabled) { fprintf(stderr, "[ctl-done] event %s refused: the frame-end tick is off\n", name); return; }
    pthread_mutex_lock(&event_lock);
    int busy = atomic_load(&event_waiting);
    if (!busy) { snprintf(event_want, sizeof event_want, "%s", name); atomic_store(&event_waiting, 1); }
    pthread_mutex_unlock(&event_lock);
    atomic_store_explicit(&runtime_survey_pending, 1, memory_order_release);   /* also wakes a waiting one a cancel left behind */
    if (busy) fprintf(stderr, "[ctl-done] event %s refused: another request is still waiting for the frame end\n", name);
#else
    fprintf(stderr, "[ctl-done] event %s refused: not available in " TOTK_GAME_VERSION " (1.4.3 only)\n", name);
#endif
}
static int autokorok_command(const char *args);   /* "autokorok ..." (auto korok, below) */
int runtime_autoshrine_command(const char *line) {
    if (!strncmp(line, "autokorok", 9) && (!line[9] || isspace((unsigned char)line[9]))) return autokorok_command(line + 9);
    if (!strncmp(line, "ctl autokorok", 13) && (!line[13] || isspace((unsigned char)line[13]))) return autokorok_command(line + 13);
    int ctl = !strncmp(line, "ctl ", 4);
    if (!ctl && strncmp(line, "autoshrine ", 11)) return 0;
    const char *tag = ctl ? "ctl" : "as", *rest = line + (ctl ? 4 : 11);
    if (!strncmp(rest, "state", 5) && (!rest[5] || rest[5] == '\n' || rest[5] == '\r' || rest[5] == ' ')) { ctl_state(tag); return 1; }
    if (!strncmp(rest, "peek ", 5)) { ctl_peek(tag, rest + 5); return 1; }
    if (ctl && !strncmp(rest, "noclip ", 7) && (!strncmp(rest + 7, "on", 2) || !strncmp(rest + 7, "off", 3))) {
        runtime_noclip_set(rest[8] == 'n'); fprintf(stderr, "[ctl-done] noclip %s\n", rest[8] == 'n' ? "on" : "off"); return 1;
    }
    if (ctl && !strncmp(rest, "goto ", 5)) {
        float xyz[3];
        if (sscanf(rest + 5, "%f %f %f", &xyz[0], &xyz[1], &xyz[2]) != 3) { fprintf(stderr, "[ctl] rejected: goto X Y Z\n"); return 1; }
        fprintf(stderr, "[ctl-done] goto %s\n", runtime_noclip_goto(xyz) ? "flying" : "refused"); return 1;
    }
    if (ctl && !strncmp(rest, "event", 5) && (!rest[5] || isspace((unsigned char)rest[5]))) { ctl_event(rest + 5); return 1; }
    if (ctl) {   /* the rest as "autoshrine <rest>": face, walk, info, watch, start, stop, ... */
        char as_line[1200]; snprintf(as_line, sizeof as_line, "autoshrine %s", rest);
        return runtime_autoshrine_command(as_line);
    }
    char word[16] = "", arg[8] = ""; sscanf(line + 11, "%15s %7s", word, arg);
    if (!strcmp(word, "start")) runtime_autoshrine_start(0);
    else if (!strcmp(word, "one")) runtime_autoshrine_start(1);
    else if (!strcmp(word, "stop")) runtime_autoshrine_stop();
    else if (!strcmp(word, "finish") && arg[0]) runtime_autoshrine_finish(atoi(arg));   /* "autoshrine finish N": Dungeon N */
    else if (!strcmp(word, "chests") && (!strcmp(arg, "on") || !strcmp(arg, "off"))) runtime_autoshrine_set_chests(arg[1] == 'n');
    else if (!strcmp(word, "skip") && (!strcmp(arg, "x") || !strcmp(arg, "b") || !strcmp(arg, "none"))) {
        atomic_store(&as_skip_reveal, arg[0] == 'x' ? AS_KEY_X : arg[0] == 'b' ? AS_KEY_B : 0); fprintf(stderr, "[autoshrine] skip: %s\n", arg[0] == 'n' ? "off" : arg[0] == 'x' ? "X" : "B");
    }
    else if (!strcmp(word, "status")) { char s[600]; runtime_autoshrine_status(s, sizeof s); fprintf(stderr, "[autoshrine] status: %s\n", s); }
    else if (!strcmp(word, "givenup") && !strcmp(arg, "reset")) runtime_autoshrine_given_up_reset();   /* "autoshrine givenup reset | list" */
    else if (!strcmp(word, "givenup")) {
        static ShrineGivenUp g; int n = shrine_given_up_load(&g); char path[1024]; shrine_given_up_path(path, sizeof path);
        fprintf(stderr, "[autoshrine] %d shrine%s given up (%s)\n", n, n == 1 ? "" : "s", path);
        for (int k = 0; k < SHRINES; k++) if (g.on[k]) fprintf(stderr, "[autoshrine]   Dungeon%03d, %s, save said %s: %s\n", k, g.date[k], g.state[k] ? SHRINE_STATE_NAMES[g.state[k]] : "nothing", g.reason[k]);
    }
    else if (!strcmp(word, "pose")) {   /* Link's place and facing (his local +z, level) and the camera's place and right vector */
        dm_gfx_camera_want(1); pthread_mutex_lock(&as_cmd_lock); as_cmd.kind = 3; pthread_mutex_unlock(&as_cmd_lock);
        atomic_store_explicit(&runtime_survey_pending, 1, memory_order_release);
    }
    else if (!strcmp(word, "watch")) { atomic_store(&as_script_watch, !strcmp(arg, "on")); fprintf(stderr, "[as-done] watch %s\n", arg); }
    else if (!strcmp(word, "info") && arg[0]) {
        int k = atoi(arg);
        if (!as_load_sync() || k < 0 || k >= SHRINES) { fprintf(stderr, "[as-info] %d none\n", k); return 1; }
        const ShrineInfo *si = &as_shrine[k]; float p[3], f[3];
        #define AS_P(name, v) fprintf(stderr, " %s=%.2f,%.2f,%.2f", name, (v)[0], (v)[1], (v)[2])
        fprintf(stderr, "[as-info] %d name=%s kind=%s chests=%d cave=%d hidden=%d crystal=%d island=%d", k, as_mark[LANDMARK_FIRST_SHRINE + k].name,
                SHRINE_KIND_TEXT[si->kind], si->nchests, si->cave, si->hidden, si->crystal, as_mark[LANDMARK_FIRST_SHRINE + k].island);
        AS_P("entrance", si->entrance); AS_P("lift", si->lift); AS_P("lift_floor", si->lift_floor); AS_P("start", si->start);
        AS_P("goal", si->goal); AS_P("goal_exit", si->goal_exit); AS_P("goal_floor", si->goal_floor); AS_P("back", si->back); AS_P("back_floor", si->back_floor);
        shrine_stand(si, SHRINE_AT_TRAVEL, 0, p, f); AS_P("travel", p);
        shrine_stand(si, SHRINE_AT_ACTIVATE, 0, p, f); AS_P("activate", p);
        for (int c = 0; c < si->nchests && c < 4; c++) { char n[16]; snprintf(n, sizeof n, "chest%d", c); shrine_stand(si, SHRINE_AT_CHEST, c, p, f); AS_P(n, p); }
        #undef AS_P
        fputc('\n', stderr);
    }
    else if ((!strcmp(word, "face") || !strcmp(word, "walk"))) {
        float x, y, z; int ms = 0;
        if (sscanf(line + 11 + strlen(word), "%f %f %f %d", &x, &y, &z, &ms) < 3) { fprintf(stderr, "[autoshrine] rejected: %s X Y Z%s\n", word, word[0] == 'w' ? " MS" : ""); return 1; }
        pthread_mutex_lock(&as_cmd_lock);
        as_cmd.kind = word[0] == 'f' ? 1 : 2; as_cmd.to[0] = x; as_cmd.to[1] = y; as_cmd.to[2] = z; as_cmd.until = now_seconds() + (ms > 0 ? ms : 1000) / 1000.0;
        pthread_mutex_unlock(&as_cmd_lock);
        atomic_store_explicit(&runtime_survey_pending, 1, memory_order_release);
    }
    else fprintf(stderr, "[autoshrine] rejected: expected state, peek, face, walk, info, watch, start, one, finish N, stop, status, chests on|off or skip x|b|none\n");
    return 1;
}
/* ---- Auto korok (Places › Koroks; korok_auto.h has what the game's files say): one korok after another, the nearest
   first, each by its kind's steps (korok_plan): noclip flies Link there and holds him in the puzzle's Areas, sets him
   down on blocks and stumps, lets him fall for a dive; a rock is walked up to and lifted with A, as a player does. The
   game finds the korok itself: a korok is found when its discovery event (DmF_SY_HideCharaDiscovery, read from the
   game's EventMgr as "ctl state" does; 1.4.3) plays; its messages are advanced with B (A would talk to the korok again
   once it is over), and a rock still held after it is put down with B. Every step is one way of solving the puzzle, and
   the log names the step the event came in ("[autokorok] discovery event in step N, OP: <method>"), which says what the
   game counts: a body held there by noclip, landed there, or walked in. No event after the last step: the korok failed
   (logged, left for this run), Link set down, the next one. Link is guarded the whole run (noclip_guard: life refilled,
   falls reset; not during a dive's fall). Any key, button or stick of the player's stops it, as auto shrine. 1.4.3 only
   (the event's name). Frame-end thread, except the loading and the commands. ---- */
static KorokInfo ak_korok[KOROKS];
static int ak_count;
static _Atomic int ak_loaded;            /* 0 not read, 1 reading, 2 read, 3 none in this game */
static _Atomic int ak_request;           /* 1 run, 2 one korok, 3 stop: for the frame-end thread */
static _Atomic int ak_want = -1;         /* with 2: that korok (index), -1 the nearest */
static _Atomic uint64_t ak_want_hash;    /* runtime_autokorok_only: the korok's hash, found once the koroks are read */
static _Atomic unsigned ak_types_on = (1u << KOROK_NTYPES) - 1;   /* KOROK_TYPES bits: every kind on at every start */
static _Atomic int ak_cancel;
static _Atomic uint32_t ak_seq_found;    /* DmF_SY_HideCharaDiscovery's flow opened (the first discovery of a session) */
static pthread_mutex_t ak_status_lock = PTHREAD_MUTEX_INITIALIZER;
static char ak_status[600] = "Not started";
enum { AK_OFF, AK_PICK, AK_STEP, AK_EVENT, AK_AFTER, AK_STOPPING };
enum { AK_FAILS_IN_A_ROW = 5 };
static struct {
    int state, one, target, k, step, phase, done, failed, failed_row, flight, flown, presses, quiet, unnamed;
    double started, t0, step_t, phase_t, last_tick, korok_t0, korok_limit, quiet_t, stall_t, run_save;
    float stall_pos[3], probe[3];
    uint32_t seq_found;
    KorokPlan plan;
    uint8_t run[KOROKS], skip[KOROKS], found[KOROKS];
    int done_k[256], ndone;   /* found in this run, for the save's confirmation */
    char why[300], failed_list[700], other_event[132];
} ak;
static const char *ak_type_name(int k) { return k >= 0 && ak_korok[k].type >= 0 ? KOROK_TYPES[ak_korok[k].type].name : "?"; }
static void ak_say(void) {
    char line[600];
    if (ak.state != AK_OFF && ak.k >= 0) {
        const KorokStep *s = ak.state == AK_STEP && ak.step < ak.plan.n ? &ak.plan.s[ak.step] : NULL;
        snprintf(line, sizeof line, "Running: %s korok, %s · %d found, %d failed this run", ak_type_name(ak.k),
                 s ? s->method : ak.state == AK_EVENT ? "its discovery (advancing the messages)" : ak.state == AK_AFTER ? "putting the rock down" : "on to the next", ak.done, ak.failed);
    } else if (ak.state != AK_OFF) snprintf(line, sizeof line, "Running: choosing the next korok · %d found, %d failed this run", ak.done, ak.failed);
    else snprintf(line, sizeof line, "%s · %d found, %d failed%s%s", ak.why[0] ? ak.why : "Stopped", ak.done, ak.failed, ak.failed ? ": " : "", ak.failed_list);
    pthread_mutex_lock(&ak_status_lock); snprintf(ak_status, sizeof ak_status, "%s", line); pthread_mutex_unlock(&ak_status_lock);
}
/* The game's current event, as "ctl state" reads it: 1 one plays (its name into name, "-" when it cannot be read), 0 none,
   -1 not known (not 1.4.3, or the EventMgr is not up). */
static int ak_event(char *name, size_t size) {
    snprintf(name, size, "-");
#if defined(TOTK_GAME_V143)
    uintptr_t mgr = survey_main ? read_ptr(survey_main + STATE_EVENT_MGR) : 0, current = 0;
    if (!mgr || !read_mem(mgr + 0x58, &current, sizeof current)) return -1;
    if (current && read_ptr(mgr + 0xb0) == mgr + 0xbc && !ctl_text(mgr + 0xbc, name, size)) snprintf(name, size, "-");
    return current != 0;
#else
    return -1;
#endif
}
/* The map the game has loaded ("MainField", "MinusField", "SmallDungeon/Dungeon009"), as "ctl state" reads it; "" unknown. */
static void ak_map(char *map, size_t size) {
    map[0] = 0;
#if defined(TOTK_GAME_V143)
    uintptr_t banc = survey_main ? read_ptr(survey_main + STATE_BANC) : 0, holder = banc ? read_ptr(banc + 0x28) : 0, info = holder ? read_ptr(holder + 0x150) : 0;
    uint8_t ready = 0;
    if (info && read_mem(info + 0x224, &ready, 1) && (ready & 1)) ctl_text(read_ptr(info + 0x158), map, size);
#else
    (void)size;
#endif
}
/* Whether the map around Link has streamed in (QueryBancLoadMgrIsStable, as "ctl state" reads it): 1, 0, or -1 not known. */
static int ak_stable(void) {
#if defined(TOTK_GAME_V143)
    uintptr_t module = survey_main ? read_ptr(survey_main + SCENE_SLOT) : 0, scene = module ? read_ptr(module + 0x1e8) : 0, components = 0, loader = 0;
    int count = 0; uint32_t flags = 0;
    if (scene && read_mem(scene + 0x50, &count, 4) && count > SCENE_BANC_LOAD && count <= 256) components = read_ptr(scene + 0x58);
    if (components) loader = read_ptr(components + SCENE_BANC_LOAD * 8u);
    if (loader && read_mem(loader + 0xed30, &flags, 4)) return flags >> 2 & 1;
#endif
    return -1;
}
static int ak_flying(void) {
    pthread_mutex_lock(&noclip_goto_lock); int pending = noclip_goto.pending; pthread_mutex_unlock(&noclip_goto_lock);
    return pending || noclip_flight.on;
}
/* Held by noclip at to: noclip on (holding him from where he is), a flight when it is more than 40 m (up, across, down:
   the world loads on the way), else a straight line at 30 m/s. 1 once he is held there. */
static int ak_hold_at(const float to[3], double dt) {
    if (!atomic_load(&noclip_wanted)) { runtime_noclip_set(1); dm_gfx_camera_want(1); ak.flight = 0; return 0; }
    if (!noclip.holding) return 0;
    if (ak.flight) { if (ak_flying()) return 0; ak.flight = 0; }
    float d = landmark_distance(noclip.xyz, to), step = 30 * (float)dt;
    if (d > 40 && !ak.flown) { runtime_noclip_goto(to); ak.flight = ak.flown = 1; return 0; }
    if (d > step) { for (int c = 0; c < 3; c++) noclip.xyz[c] += (to[c] - noclip.xyz[c]) * step / d; return 0; }
    memcpy(noclip.xyz, to, 12);
    return 1;
}
/* Let go in the air (a dive, or the discovery event, which opens the paraglider itself). */
static void ak_let_go(const char *why) {
    if (!atomic_load(&noclip_wanted) && !noclip.holding) return;
    if (atomic_load(&noclip_wanted)) runtime_noclip_set(0);
    dm_gfx_camera_want(1);
    pthread_mutex_lock(&noclip_goto_lock); noclip_goto.pending = 0; pthread_mutex_unlock(&noclip_goto_lock);
    fprintf(stderr, "[autokorok] let go at %.1f,%.1f,%.1f in the air: %s\n", noclip.xyz[0], noclip.xyz[1], noclip.xyz[2], why);
    noclip.holding = 0; noclip_land.on = 0; noclip_flight.on = 0;
}
static void ak_go(int state, double now) {
    as_walk(0, NULL); runtime_input_lease_release_all();
    ak.state = state; ak.t0 = now; ak.phase = 0; ak.phase_t = now;
    ak_say();
}
/* The run ends: every key let go; noclip off (a flight first ends on the ground; from wherever he is held he falls,
   guarded until he stops); a summary and a sound. */
static void ak_end(int ok, const char *why, double now) {
    as_walk(0, NULL); runtime_input_lease_release_all(); atomic_store(&ak_no_fall, 0);
    snprintf(ak.why, sizeof ak.why, "%s%s", ok ? "Finished: " : "", why);
    fprintf(stderr, "[autokorok] %s\n", ak.why);
    fprintf(stderr, "[autokorok] summary: %d found, %d failed%s%s%s, %.0f s\n", ak.done, ak.failed, ak.failed ? " (" : "", ak.failed_list, ak.failed ? ")" : "", now - ak.started);
    runtime_front_run_sound(ok);
    if (atomic_load(&noclip_wanted) && ak_flying()) { ak_go(AK_STOPPING, now); return; }
    if (atomic_load(&noclip_wanted)) runtime_noclip_set(0);   /* he falls, guarded until he stops (noclip_guard) */
    else dm_gfx_camera_want(0);
    ak_go(AK_OFF, now); atomic_store(&ak_active, 0); ak_say();
}
static void ak_step_begin(double now) {
    as_walk(0, NULL); runtime_input_lease_release_all();
    ak.step_t = now; ak.phase = 0; ak.phase_t = now; ak.flight = ak.flown = 0;
    const KorokStep *s = &ak.plan.s[ak.step];
    if (s->op == KS_WAIT || s->op == KS_DROP) fprintf(stderr, "[autokorok] step %d of %d, %s: %s (%.1f s)\n", ak.step + 1, ak.plan.n, KOROK_STEP_TEXT[s->op], s->method, s->secs);
    else fprintf(stderr, "[autokorok] step %d of %d, %s: %s, at %.1f,%.1f,%.1f (%.1f s)\n", ak.step + 1, ak.plan.n, KOROK_STEP_TEXT[s->op], s->method, s->at[0], s->at[1], s->at[2], s->secs);
    char shot[40]; snprintf(shot, sizeof shot, "korok-%d-%s", ak.step + 1, KOROK_STEP_TEXT[s->op]); as_shot(shot);
    ak_say();
}
/* A korok given up: logged with the step and the reason, kept out for the rest of the run; Link set down; the next one.
   AK_FAILS_IN_A_ROW in a row end the run (something is wrong in general, not with one korok). */
static void ak_fail(const char *what, double now) {
    const KorokInfo *k = &ak_korok[ak.k]; const KorokStep *s = ak.step < ak.plan.n ? &ak.plan.s[ak.step] : NULL;
    fprintf(stderr, "[autokorok] failed: %s korok %llu at %.1f,%.1f,%.1f, step %d of %d%s%s: %s\n", ak_type_name(ak.k), (unsigned long long)k->hash,
            k->pos[0], k->pos[1], k->pos[2], ak.step + 1, ak.plan.n, s ? ", " : "", s ? s->method : "", what);
    as_walk(0, NULL); runtime_input_lease_release_all(); atomic_store(&ak_no_fall, 0);
    ak.run[ak.k] = KOROK_SKIP_FAILED; ak.failed++; ak.failed_row++;
    size_t o = strlen(ak.failed_list);
    if (o + 80 < sizeof ak.failed_list) snprintf(ak.failed_list + o, sizeof ak.failed_list - o, "%s%s %llu", o ? ", " : "", ak_type_name(ak.k), (unsigned long long)k->hash);
    if (atomic_load(&noclip_wanted) && !ak_flying()) runtime_noclip_set(0);   /* set down (a guarded fall) before the next */
    if (ak.one || ak.target >= 0) { char why[300]; snprintf(why, sizeof why, "Stopped: the korok failed: %s", what); ak_end(0, why, now); return; }
    if (ak.failed_row >= AK_FAILS_IN_A_ROW) { ak_end(0, "Stopped: five koroks failed in a row", now); return; }
    ak_go(AK_PICK, now);
}
static void ak_step_next(double now) {
    if (++ak.step >= ak.plan.n) { ak_fail("no discovery event after its last step", now); return; }
    ak_step_begin(now);
}
static void ak_pick(uintptr_t actor, double now) {
    float at[3]; char map[160];
    if (!as_pos(actor, at) || (fabsf(at[0]) < 1 && fabsf(at[2]) < 1)) { if (now - ak.t0 > 60) ak_end(0, "Stopped: no player to start from (60 s)", now); return; }
    ak_map(map, sizeof map);
    if (!strncmp(map, "MinusField", 10) || strstr(map, "Dungeon")) {   /* the Depths, a shrine, a temple: the koroks are on the surface and in the sky */
        char why[260]; snprintf(why, sizeof why, "Stopped: Link is in %s; the koroks are on the surface and in the sky (go there first)", map); ak_end(0, why, now); return;
    }
    const char *profile = getenv("TOTK_PROFILE_DIR");
    int flags = korok_found_read(profile, ak_korok, ak_count, ak.found), found = 0;
    for (int i = 0; i < ak_count; i++) found += ak.found[i];
    if (ak.ndone) {   /* the koroks found in this run: what the newest save says of them (the game saves them at its next autosave) */
        int confirmed = 0; for (int i = 0; i < ak.ndone; i++) confirmed += ak.found[ak.done_k[i]];
        double saved = as_save_time();
        if (saved > ak.run_save) fprintf(stderr, "[autokorok] the newest save confirms %d of the %d koroks found in this run (IsAppearKorok)\n", confirmed, ak.ndone);
    }
    static Landmark lm[LANDMARKS]; LandmarkProgress progress = {0, 0, 0};
    int lock = settings_get("teleport.story_lock") != 0 && profile && landmarks_state_read(profile, lm, &progress) >= 0 && progress.known && !progress.island_done;
    unsigned types = ak.target >= 0 ? (1u << KOROK_NTYPES) - 1 : atomic_load(&ak_types_on);   /* a korok chosen by hash: whatever its kind's switch */
    int count[KOROK_SKIPS] = {0};
    for (int i = 0; i < ak_count; i++) count[ak.skip[i] = (uint8_t)korok_skip(&ak_korok[i], ak.found[i], types, lock && !ak_korok[i].island, ak.run[i])]++;
    char left[400] = ""; size_t o = 0;
    for (int r = 1; r < KOROK_SKIPS && o < sizeof left - 1; r++) if (count[r]) o += (size_t)snprintf(left + o, sizeof left - o, "%s%d %s", o ? ", " : "", count[r], KOROK_SKIP_TEXT[r]);
    fprintf(stderr, "[autokorok] %d koroks to do; %d IsAppearKorok flags in the save, %d found; map %s; left out: %s\n", count[KOROK_GO], flags, found, map[0] ? map : "?", left[0] ? left : "none");
    if (ak.target >= 0 && ak.target < ak_count && ak.skip[ak.target] != KOROK_GO) {
        char why[300]; snprintf(why, sizeof why, "Refused: that korok (%llu) is left out: %s", (unsigned long long)ak_korok[ak.target].hash, KOROK_SKIP_TEXT[ak.skip[ak.target]]);
        if (ak.run[ak.target] == KOROK_SKIP_DONE) ak_end(1, "the korok is found", now); else ak_end(0, why, now);
        return;
    }
    if ((ak.one || ak.target >= 0) && ak.done) { ak_end(1, "the korok is found", now); return; }
    int k = ak.target >= 0 && ak.target < ak_count ? ak.target : korok_next(ak_korok, ak_count, ak.skip, at);
    if (k < 0) { ak.k = -1; ak_end(1, "no korok left to do", now); return; }
    const KorokInfo *ko = &ak_korok[k];
    ak.k = k; ak.step = 0; korok_plan(ko, at, &ak.plan);
    if (!ak.plan.n) { ak.run[k] = KOROK_SKIP_FAILED; fprintf(stderr, "[autokorok] korok %llu has no steps; left out\n", (unsigned long long)ko->hash); ak_go(AK_PICK, now); return; }
    float d = landmark_distance(ko->pos, at);
    ak.korok_t0 = now; ak.korok_limit = 150 + d / 60; ak.seq_found = atomic_load(&ak_seq_found); ak.other_event[0] = 0; ak.unnamed = 0;
    fprintf(stderr, "[autokorok] next: %s korok %llu (HideType %d, %s%s%s) at %.1f,%.1f,%.1f, %.0f m away, %d steps\n", ak_type_name(k), (unsigned long long)ko->hash, ko->hide,
            ko->fly ? "Npc_HiddenKorokFly" : "Npc_HiddenKorokGround", ko->sky ? ", a sky island" : "", ko->island ? ", Great Sky Island" : "", ko->pos[0], ko->pos[1], ko->pos[2], d, ak.plan.n);
    if (ko->time_limit > 0) fprintf(stderr, "[autokorok] the race's time limit: %.0f s\n", ko->time_limit);
    ak_go(AK_STEP, now); ak_step_begin(now);
}
/* The discovery event (or its flow opening) since the korok was chosen; another event is logged once. */
static int ak_discovery(double now) {
    char name[132]; int e = ak_event(name, sizeof name);
    if (atomic_load(&ak_seq_found) != ak.seq_found) { fprintf(stderr, "[autokorok] DmF_SY_HideCharaDiscovery's flow opened\n"); return 1; }
    if (e != 1) return 0;
    if (!strcmp(name, "DmF_SY_HideCharaDiscovery")) return 1;
    if (!strcmp(name, "-")) {   /* the name did not read: two seconds of an event no step starts is taken as the discovery */
        if (!ak.unnamed) { ak.unnamed = 1; ak.quiet_t = now; fprintf(stderr, "[autokorok] an event plays and its name does not read\n"); }
        return now - ak.quiet_t >= 2;
    }
    if (strcmp(name, ak.other_event)) { snprintf(ak.other_event, sizeof ak.other_event, "%s", name); fprintf(stderr, "[autokorok] another event plays: %s (not the discovery; the steps go on)\n", name); }
    return 0;
}
static void ak_tick_step(uintptr_t actor, double now, double dt) {
    const KorokStep *s = &ak.plan.s[ak.step]; float at[3]; int have = as_pos(actor, at);
    double t = now - ak.step_t, c = now - ak.phase_t;
    if (ak_discovery(now)) {
        fprintf(stderr, "[autokorok] discovery event in step %d of %d, %s: %s (%.1f s into it)%s%.1f,%.1f,%.1f\n", ak.step + 1, ak.plan.n, KOROK_STEP_TEXT[s->op], s->method, t,
                have ? "; Link at " : "", have ? at[0] : 0, have ? at[1] : 0, have ? at[2] : 0);
        atomic_store(&ak_no_fall, 0); ak.quiet = 0; ak.presses = 0;
        if (atomic_load(&noclip_wanted) || noclip.holding) ak_let_go("the discovery event takes over");
        ak_go(AK_EVENT, now); return;
    }
    if (now - ak.korok_t0 > ak.korok_limit) { char why[120]; snprintf(why, sizeof why, "took longer than %.0f s", ak.korok_limit); ak_fail(why, now); return; }
    switch (s->op) {
    case KS_FLY: case KS_HOLD: case KS_SNAP:
        if (ak.phase == 0) {
            if (s->op == KS_SNAP) {   /* from wherever he is (standing, too): noclip on, then there in one frame */
                if (!atomic_load(&noclip_wanted)) { runtime_noclip_set(1); dm_gfx_camera_want(1); return; }
                if (!noclip.holding) return;
                fprintf(stderr, "[autokorok] put at %.1f,%.1f,%.1f at once, from %.1f,%.1f,%.1f\n", s->at[0], s->at[1], s->at[2], noclip.xyz[0], noclip.xyz[1], noclip.xyz[2]);
                memcpy(noclip.xyz, s->at, 12);
            } else if (!ak_hold_at(s->at, dt)) { if (t > 30 + landmark_distance(noclip.xyz, s->at) / 60) ak_fail("the flight did not arrive", now); return; }
            ak.phase = 1; ak.phase_t = now; return;
        }
        memcpy(noclip.xyz, s->at, 12);
        if (c < s->secs) return;
        if (s->op == KS_FLY && s->secs >= 1.5f) {   /* a far place: also until the map around him has streamed in (8 s more at most) */
            int stable = ak_stable();
            if (stable == 0 && c < s->secs + 8) return;
            if (stable == 0) fprintf(stderr, "[autokorok] the map still streams in after %.1f s; going on\n", c);
        }
        ak_step_next(now); return;
    case KS_SWEEP:
        if (ak.phase == 0) { if (!ak_hold_at(s->at, dt)) return; ak.phase = 1; return; }
        { float d = landmark_distance(noclip.xyz, s->to), step = 3 * (float)dt;
          if (d > step) { for (int i = 0; i < 3; i++) noclip.xyz[i] += (s->to[i] - noclip.xyz[i]) * step / d; return; }
          memcpy(noclip.xyz, s->to, 12); }
        ak_step_next(now); return;
    case KS_LAND:
        if (ak.phase == 0) { if (!ak_hold_at(s->at, dt)) return; runtime_noclip_set(0); dm_gfx_camera_want(1); ak.phase = 1; ak.phase_t = now; return; }
        if (ak.phase == 1) {   /* noclip off: he falls, guarded, until his height stops changing (noclip_guard) */
            int down = !noclip.holding && !atomic_load(&noclip_settle.on);   /* noclip_guard: his height has stopped changing */
            if (!down && c < 30) return;
            fprintf(stderr, "[autokorok] %s at %.1f,%.1f,%.1f after %.1f s (%.1f m from the step's place)\n", down ? "standing" : "still not standing", at[0], at[1], at[2], c,
                    have ? landmark_distance(at, s->at) : -1.0f);
            ak.phase = 2; ak.phase_t = now; return;
        }
        if (c >= s->secs) ak_step_next(now);
        return;
    case KS_WALK: {   /* the left stick toward the point, from the camera's view (as_walk), until within to[0] m, stopped or the time is up */
        if (atomic_load(&noclip_wanted) || noclip.holding) { if (atomic_load(&noclip_wanted)) { runtime_noclip_set(0); dm_gfx_camera_want(1); } ak.step_t = now; return; }
        float d = have ? as_xz(at, s->at) : 99; int stalled = 0;
        if (have && (now - ak.stall_t >= 0.5 || t < 0.05)) { stalled = t > 1 && landmark_distance(at, ak.stall_pos) < 0.1f; memcpy(ak.stall_pos, at, 12); ak.stall_t = now; }
        if (d < s->to[0] || stalled || t > s->secs) {
            fprintf(stderr, "[autokorok] walked %.1f s, now %.1f m from the point%s\n", t, d, d < s->to[0] ? "" : stalled ? " (stopped by something)" : " (time up)");
            as_walk(0, NULL); ak_step_next(now); return;
        }
        as_walk(actor, s->at); return;
    }
    case KS_LIFT:   /* the camera behind him (ZL), a short push toward the rock (he turns to it), nothing held, then A held */
        if (atomic_load(&noclip_wanted) || noclip.holding) { if (atomic_load(&noclip_wanted)) { runtime_noclip_set(0); dm_gfx_camera_want(1); } ak.step_t = ak.phase_t = now; return; }
        if (ak.phase == 0) { as_key(AS_KEY_ZL, 0.15); ak.phase = 1; }
        else if (ak.phase == 1 && c >= 0.5) { as_face(actor, s->at); ak.phase = 2; }
        else if (ak.phase == 2 && c >= 0.9) { runtime_input_lease_release_all(); ak.phase = 3; }
        else if (ak.phase == 3 && c >= 1.1) {
            float rot[9] = {0}, dir[2] = {0, 0}, off = -1;
            if (have && read_mem(actor + ACTOR_ROT, rot, sizeof rot)) {
                shrine_unit_xz(at, s->at, dir); float fl = sqrtf(rot[2] * rot[2] + rot[8] * rot[8]);
                if (fl > 1e-3f) off = acosf(fmaxf(-1, fminf(1, (rot[2] * dir[0] + rot[8] * dir[1]) / fl))) * 57.2958f;
            }
            as_key(AS_KEY_A, s->secs);
            fprintf(stderr, "[autokorok] A for %.1f s at the rock, %.2f m away, facing %.0f degrees off it\n", s->secs, have ? as_xz(at, s->at) : -1.0f, off);
            ak.phase = 4; ak.phase_t = now;
        } else if (ak.phase == 4 && c >= s->secs + 0.2) ak_step_next(now);
        return;
    case KS_DROP:   /* let go in the air, the falls not reset (a dive is a long fall into water); until he stops falling */
        if (ak.phase == 0) { atomic_store(&ak_no_fall, 1); ak_let_go("a dive"); ak.phase = 1; ak.phase_t = now; if (have) memcpy(ak.probe, at, 12); return; }
        if (c >= 0.5 && have) {
            float fell = ak.probe[1] - at[1]; memcpy(ak.probe, at, 12); ak.phase_t = now;
            if ((t > 1.5 && fabsf(fell) < 0.05f) || t > s->secs) {
                atomic_store(&ak_no_fall, 0);
                fprintf(stderr, "[autokorok] the fall ended at %.1f,%.1f,%.1f after %.1f s\n", at[0], at[1], at[2], t);
                ak_step_next(now);
            }
        } else if (t > s->secs) { atomic_store(&ak_no_fall, 0); ak_step_next(now); }
        return;
    case KS_WAIT: default:
        if (t >= s->secs) ak_step_next(now);
        return;
    }
}
static void autokorok_tick(uintptr_t actor, double now) {
    int req = atomic_exchange(&ak_request, 0);
    if (req == 3 && ak.state != AK_OFF && ak.state != AK_STOPPING) { ak_end(0, "Stopped: from the menu or the input file", now); return; }
    if ((req == 1 || req == 2) && ak.state == AK_OFF) {
        memset(&ak, 0, sizeof ak); ak.k = -1;
        ak.one = req == 2; ak.target = atomic_exchange(&ak_want, -1); ak.started = now; ak.last_tick = now; ak.run_save = as_save_time();
        atomic_store(&ak_cancel, 0); atomic_store(&ak_no_fall, 0); dm_gfx_camera_want(1); runtime_input_lease_release_all();
        unsigned types = atomic_load(&ak_types_on); char on[200] = ""; size_t o = 0;
        for (int t = 0; t < KOROK_NTYPES; t++) if (types >> t & 1) o += (size_t)snprintf(on + o, sizeof on - o, "%s%s", o ? ", " : "", KOROK_TYPES[t].word);
        fprintf(stderr, "[autokorok] start: %s; kinds on: %s\n", ak.target >= 0 ? "one chosen korok" : ak.one ? "the nearest korok" : "every korok it can do, nearest first", on[0] ? on : "none");
        ak_go(AK_PICK, now);
    }
    if (ak.state == AK_OFF) return;
    double dt = now - ak.last_tick; ak.last_tick = now;
    if (dt > 0.1) dt = 0.1;
    if (atomic_exchange(&ak_cancel, 0) && now - ak.started > 2 && ak.state != AK_STOPPING) { ak_end(0, "Stopped: your own input (a key, a button or the stick)", now); return; }
    double t = now - ak.t0;
    switch (ak.state) {
    case AK_PICK: ak_pick(actor, now); return;
    case AK_STEP: ak_tick_step(actor, now, dt); return;
    case AK_EVENT: {   /* the korok appears and talks: B every 0.8 s after the first 2.5 s; over after 1.5 s without an event */
        char name[132]; int e = ak_event(name, sizeof name);
        if (e == 1) ak.quiet = 0;
        else if (!ak.quiet) { ak.quiet = 1; ak.quiet_t = now; }
        if ((ak.quiet && now - ak.quiet_t >= 1.5 && t > 3) || (e < 0 && t > 20) || t > 90) {
            fprintf(stderr, "[autokorok] the discovery is over after %.1f s (%d B presses)%s\n", t, ak.presses, t > 90 ? ": still playing after 90 s, going on" : "");
            ak_go(AK_AFTER, now); return;
        }
        if (e == 1 && t > 2.5 && now - ak.phase_t >= 0.8) { as_key(AS_KEY_B, 0.1); ak.presses++; ak.phase_t = now; }
        return;
    }
    case AK_AFTER: {   /* a rock is still lifted after the event: put down with B (then once more in case the first one came early) */
        const KorokInfo *k = &ak_korok[ak.k]; int rock = k->hide == KOROK_HIDE_ROCK;
        if (rock && ak.phase == 0 && t >= 0.6) { as_key(AS_KEY_B, 0.1); fprintf(stderr, "[autokorok] B to put the rock down\n"); ak.phase = 1; }
        else if (rock && ak.phase == 1 && t >= 1.6) { as_key(AS_KEY_B, 0.1); ak.phase = 2; }
        else if ((!rock && t >= 1) || (rock && ak.phase == 2 && t >= 2.6)) {
            ak.run[ak.k] = KOROK_SKIP_DONE; ak.done++; ak.failed_row = 0;
            if (ak.ndone < (int)(sizeof ak.done_k / sizeof *ak.done_k)) ak.done_k[ak.ndone++] = ak.k;
            fprintf(stderr, "[autokorok] found: %s korok %llu in %.0f s (%d found in this run)\n", ak_type_name(ak.k), (unsigned long long)k->hash, now - ak.korok_t0, ak.done);
            if (ak.one || ak.target >= 0) { ak_end(1, "the korok is found", now); return; }
            ak_go(AK_PICK, now);
        }
        return;
    }
    case AK_STOPPING:   /* a flight first lands where it was going, then noclip off (a guarded fall) */
        if (t < 60 && ak_flying()) return;
        if (atomic_load(&noclip_wanted)) runtime_noclip_set(0);
        fprintf(stderr, "[autokorok] set down\n");
        ak_go(AK_OFF, now); atomic_store(&ak_active, 0); ak_say();
        return;
    }
}
/* The koroks from the RomFS, once (a few hundred map files, about a second); 1 when there are some. */
static int ak_load_sync(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&ak_loaded, &expected, 1)) {
        LandmarkRomfs rom = {runtime_romfs_file_count, runtime_romfs_file_name, runtime_romfs_host_read, runtime_romfs_zs_decompress};
        double t0 = now_seconds();
        ak_count = koroks_load(&rom, ak_korok, KOROKS);
        int go[KOROK_NTYPES] = {0};
        for (int i = 0; i < ak_count; i++) if (ak_korok[i].type >= 0 && korok_skip(&ak_korok[i], 0, ~0u, 0, 0) == KOROK_GO) go[ak_korok[i].type]++;
        char line[300]; size_t o = 0;
        for (int t = 0; t < KOROK_NTYPES; t++) o += (size_t)snprintf(line + o, sizeof line - o, "%s%d %s", t ? ", " : "", go[t], KOROK_TYPES[t].word);
        fprintf(stderr, "[autokorok] read %d koroks from the game's files (%.2f s); the run can do %s\n", ak_count, now_seconds() - t0, line);
        atomic_store(&ak_loaded, ak_count ? 2 : 3);
    }
    while (atomic_load(&ak_loaded) == 1) usleep(50000);
    return atomic_load(&ak_loaded) == 2;
}
static int ak_index(uint64_t hash) { for (int i = 0; i < ak_count; i++) if (ak_korok[i].hash == hash) return i; return -1; }
static void *ak_start_thread(void *arg) {
    int one = (int)(intptr_t)arg;
    if (!ak_load_sync()) {
        fprintf(stderr, "[autokorok] not started: the koroks are not in this game's files\n");
        pthread_mutex_lock(&ak_status_lock); snprintf(ak_status, sizeof ak_status, "Not available: the koroks are not in this game's files"); pthread_mutex_unlock(&ak_status_lock);
        atomic_store(&ak_active, 0); return NULL;
    }
    uint64_t hash = atomic_exchange(&ak_want_hash, 0);
    if (hash) {
        int k = ak_index(hash);
        if (k < 0) {
            fprintf(stderr, "[autokorok] not started: no korok %llu in the game's files\n", (unsigned long long)hash);
            pthread_mutex_lock(&ak_status_lock); snprintf(ak_status, sizeof ak_status, "Not started: no korok %llu", (unsigned long long)hash); pthread_mutex_unlock(&ak_status_lock);
            atomic_store(&ak_active, 0); return NULL;
        }
        atomic_store(&ak_want, k);
    }
    atomic_store(&ak_request, one ? 2 : 1);
    atomic_store_explicit(&runtime_survey_pending, 1, memory_order_release);
    return NULL;
}
int runtime_autokorok_available(void) {
#if defined(TOTK_GAME_V143)
    return noclip_force != NULL && survey_main;
#else
    return 0;
#endif
}
int runtime_autokorok_start(int one) {
    if (!runtime_autokorok_available()) { fprintf(stderr, "[autokorok] not started: needs noclip and the game's event state (1.4.3)\n"); return 0; }
    if (atomic_load(&as_active)) { fprintf(stderr, "[autokorok] not started: auto shrine is running\n"); return 0; }
    int idle = 0;
    if (!atomic_compare_exchange_strong(&ak_active, &idle, 1)) { fprintf(stderr, "[autokorok] already running\n"); return 0; }
    pthread_mutex_lock(&ak_status_lock); snprintf(ak_status, sizeof ak_status, "Starting: reading the koroks…"); pthread_mutex_unlock(&ak_status_lock);
    pthread_t t;
    if (pthread_create(&t, NULL, ak_start_thread, (void *)(intptr_t)one)) { atomic_store(&ak_active, 0); return 0; }
    pthread_detach(t);
    return 1;
}
int runtime_autokorok_only(uint64_t hash) {
    if (!hash || atomic_load(&ak_active)) return 0;
    atomic_store(&ak_want_hash, hash);
    if (runtime_autokorok_start(1)) return 1;
    atomic_store(&ak_want_hash, 0); return 0;
}
void runtime_autokorok_stop(void) {
    if (!atomic_load(&ak_active)) return;
    atomic_store(&ak_request, 3); atomic_store_explicit(&runtime_survey_pending, 1, memory_order_release);
}
void runtime_autokorok_set_type(int type, int on) {
    if (type < 0 || type >= KOROK_NTYPES) return;
    if (on) atomic_fetch_or(&ak_types_on, 1u << type); else atomic_fetch_and(&ak_types_on, ~(1u << type));
    fprintf(stderr, "[autokorok] %s koroks %s\n", KOROK_TYPES[type].word, on ? "on" : "off");
}
int runtime_autokorok_type_on(int type) { return type >= 0 && type < KOROK_NTYPES && (atomic_load(&ak_types_on) >> type & 1); }
int runtime_autokorok_status(char *text, size_t size) {
    pthread_mutex_lock(&ak_status_lock); snprintf(text, size, "%s", ak_status); pthread_mutex_unlock(&ak_status_lock);
    return atomic_load(&ak_active);
}
static void *ak_load_thread(void *arg) { (void)arg; ak_load_sync(); return NULL; }
/* Per kind (KOROK_TYPES, then one more for every other kind): found in the newest save and how many there are. 1 when the
   koroks are read, 0 while they are being read (the first call starts that), -1 when this game has none. */
int runtime_autokorok_counts(int *found, int *total, int types) {
    for (int t = 0; t < types; t++) found[t] = total[t] = 0;
    int state = atomic_load(&ak_loaded);
    if (!state) { pthread_t t; if (!pthread_create(&t, NULL, ak_load_thread, NULL)) pthread_detach(t); return 0; }
    if (state == 1) return 0;
    if (state == 3) return -1;
    static uint8_t got[KOROKS]; static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    korok_found_read(getenv("TOTK_PROFILE_DIR"), ak_korok, ak_count, got);
    for (int i = 0; i < ak_count; i++) {
        int t = ak_korok[i].type >= 0 ? ak_korok[i].type : KOROK_NTYPES;
        if (t < types) { total[t]++; found[t] += got[i]; }
    }
    pthread_mutex_unlock(&lock);
    return 1;
}
void runtime_autokorok_cancel_user(void) { if (atomic_load(&ak_active)) atomic_store(&ak_cancel, 1); }
/* romfs.c's file opens: the discovery's flow (the first korok of a session; the game keeps it after), and every event
   flow while a run is on, for the log. */
static void autokorok_romfs(const char *path) {
    if (!atomic_load_explicit(&ak_active, memory_order_relaxed) || strncmp(path, "Event/EventFlow/", 16)) return;
    if (!strncmp(path + 16, "DmF_SY_HideCharaDiscovery", 25)) atomic_fetch_add(&ak_seq_found, 1);
    fprintf(stderr, "[autokorok] the game opens %s\n", path);
}
/* The input file's "autokorok ..." (also "ctl autokorok ..."):
     start | one [<hash>] | stop | status      a run; one: the nearest korok, or that one (its kind's switch aside)
     type <word> on|off | types                the kinds (rock leaf dive flowers ring jump stickwall ceiling)
     list [<word>] [<n>]                       the n nearest koroks (of a kind) not found yet, with their hashes
     info <hash>                               a korok's parts and steps from where Link is */
static int autokorok_command(const char *args) {
    char word[16] = "", a1[32] = "", a2[16] = ""; sscanf(args, "%15s %31s %15s", word, a1, a2);
    if (!strcmp(word, "start")) runtime_autokorok_start(0);
    else if (!strcmp(word, "one")) { if (a1[0]) runtime_autokorok_only(strtoull(a1, NULL, 10)); else runtime_autokorok_start(1); }
    else if (!strcmp(word, "stop")) runtime_autokorok_stop();
    else if (!strcmp(word, "status")) { char s[600]; runtime_autokorok_status(s, sizeof s); fprintf(stderr, "[autokorok] status: %s\n", s); }
    else if (!strcmp(word, "type") && korok_type_word(a1) >= 0 && (!strcmp(a2, "on") || !strcmp(a2, "off"))) runtime_autokorok_set_type(korok_type_word(a1), a2[1] == 'n');
    else if (!strcmp(word, "types")) for (int t = 0; t < KOROK_NTYPES; t++) fprintf(stderr, "[autokorok] %s (%s): %s\n", KOROK_TYPES[t].word, KOROK_TYPES[t].name, runtime_autokorok_type_on(t) ? "on" : "off");
    else if (!strcmp(word, "list") || !strcmp(word, "info")) {
        if (!ak_load_sync()) { fprintf(stderr, "[autokorok] no koroks in this game's files\n"); return 1; }
        float at[3] = {0, 0, 0}; int have = runtime_player_pos(at);
        static uint8_t got[KOROKS]; korok_found_read(getenv("TOTK_PROFILE_DIR"), ak_korok, ak_count, got);
        if (!strcmp(word, "info")) {
            int k = ak_index(strtoull(a1, NULL, 10));
            if (k < 0) { fprintf(stderr, "[autokorok] info: no korok %s\n", a1); return 1; }
            const KorokInfo *ko = &ak_korok[k]; KorokPlan plan; korok_plan(ko, at, &plan);
            fprintf(stderr, "[autokorok] info %llu: %s (HideType %d) at %.1f,%.1f,%.1f%s%s%s, %s; skip: %s\n", (unsigned long long)ko->hash, ak_type_name(k), ko->hide, ko->pos[0], ko->pos[1], ko->pos[2],
                    ko->sky ? ", a sky island" : "", ko->island ? ", Great Sky Island" : "", ko->blocked ? ", blocked" : "", got[k] ? "found" : "not found",
                    KOROK_SKIP_TEXT[korok_skip(ko, got[k], ~0u, 0, 0)][0] ? KOROK_SKIP_TEXT[korok_skip(ko, got[k], ~0u, 0, 0)] : "none");
            for (int i = 0; i < ko->nparts; i++) fprintf(stderr, "[autokorok]   part %s%s%s at %.1f,%.1f,%.1f scale %.2f,%.2f,%.2f shape %d\n", ko->part[i].kind == KOROK_PART_ROCK ? "rock" : "Area",
                    ko->part[i].role ? " " : "", KOROK_ROLE_TEXT[ko->part[i].role], ko->part[i].pos[0], ko->part[i].pos[1], ko->part[i].pos[2], ko->part[i].scale[0], ko->part[i].scale[1], ko->part[i].scale[2], ko->part[i].shape);
            for (int i = 0; i < plan.n; i++) fprintf(stderr, "[autokorok]   step %d %s: %s at %.1f,%.1f,%.1f (%.1f s)\n", i + 1, KOROK_STEP_TEXT[plan.s[i].op], plan.s[i].method, plan.s[i].at[0], plan.s[i].at[1], plan.s[i].at[2], plan.s[i].secs);
            return 1;
        }
        int type = korok_type_word(a1), n = atoi(type >= 0 ? a2 : a1); if (n <= 0) n = 5; if (n > 50) n = 50;
        static uint8_t skip[KOROKS];
        for (int i = 0; i < ak_count; i++) skip[i] = (uint8_t)korok_skip(&ak_korok[i], got[i], type >= 0 ? 1u << type : (1u << KOROK_NTYPES) - 1, 0, 0);
        fprintf(stderr, "[autokorok] list: the %d nearest %s%skoroks not found yet%s\n", n, type >= 0 ? KOROK_TYPES[type].word : "", type >= 0 ? " " : "", have ? "" : " (no player: from the origin)");
        for (int j = 0; j < n; j++) {
            int k = korok_next(ak_korok, ak_count, skip, at);
            if (k < 0) break;
            skip[k] = KOROK_SKIP_DONE;
            fprintf(stderr, "[autokorok]   %llu %s at %.1f,%.1f,%.1f, %.0f m away\n", (unsigned long long)ak_korok[k].hash, ak_type_name(k), ak_korok[k].pos[0], ak_korok[k].pos[1], ak_korok[k].pos[2], landmark_distance(ak_korok[k].pos, at));
        }
    }
    else fprintf(stderr, "[autokorok] rejected: expected start, one [hash], stop, status, type <kind> on|off, types, list [kind] [n] or info <hash>\n");
    return 1;
}
/* romfs.c, every file the game opens (path relative to the RomFS): which map is loading, and the goal's event. */
void runtime_autoshrine_romfs(const char *path) {
    if (!path) return;
    if (atomic_load_explicit(&as_script_watch, memory_order_relaxed)) {   /* script mode: every shrine map, the first surface map
        file after a shrine's, and every event flow */
        static int field_logged;
        if (!strncmp(path, "Banc/SmallDungeon/", 18)) { field_logged = 0; fprintf(stderr, "[as-file] %s\n", path); }
        else if (!strncmp(path, "Banc/MainField/", 15) && !field_logged) { field_logged = 1; fprintf(stderr, "[as-file] %s\n", path); }
        else if (!strncmp(path, "Event/EventFlow/", 16)) fprintf(stderr, "[as-file] %s\n", path);
    }
    autokorok_romfs(path);
    if (!atomic_load_explicit(&as_active, memory_order_relaxed)) return;
    if (!strncmp(path, "Banc/", 5)) atomic_store(&as_map_ns, runtime_now_ns());   /* any map file: a load is going on */
    if (!strncmp(path, "Banc/SmallDungeon/Dungeon", 25) && landmark_ends(path, "_Static.bcett.byml.zs")) {
        atomic_store(&as_inside_index, atoi(path + 25)); atomic_fetch_add(&as_seq_inside, 1);
        fprintf(stderr, "[autoshrine] the game opens %s\n", path);
    } else if (!strncmp(path, "Banc/MainField/", 15)) {
        atomic_fetch_add(&as_seq_field, 1);
        if (atomic_exchange(&as_watch_field, 0)) fprintf(stderr, "[autoshrine] the game opens %s (the first of the surface's map)\n", path);
    } else if (!strncmp(path, "Event/EventFlow/", 16)) {   /* a cutscene's flow (the first time it plays this run; later ones may be cached) */
        if (!strncmp(path + 16, "DmF_SY_SmallDungeonGoal", 23)) atomic_fetch_add(&as_seq_goal, 1);
        if (!strncmp(path + 16, "DmF_SY_GameOver", 15)) atomic_fetch_add(&as_seq_gameover, 1);   /* the Game Over screen */
        atomic_fetch_add(&as_seq_event, 1); atomic_store(&as_event_ns, runtime_now_ns());
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
    if (!survey_left) { noclip_tick(actor); autoshrine_tick(actor, now_seconds()); autokorok_tick(actor, now_seconds()); }   /* a scripted move wins */
#if defined(TOTK_GAME_V143)
    event_tick();   /* "ctl event": outside the survey lock, it calls into the game */
    noclip_guard(actor);
#endif
    runtime_spawn_tick(actor, frame);   /* outside the survey lock: it calls into the game */
    /* cleared before the spawn queue is looked at again, so a spawn queued meanwhile (it sets waiting, then pending) is
       never lost */
    atomic_store(&runtime_survey_pending, survey_left);
    if (atomic_load(&runtime_spawn_waiting) || atomic_load(&noclip_wanted) || atomic_load(&as_active) || atomic_load(&ak_active)) atomic_store(&runtime_survey_pending, 1);
    /* noclip let go: the tick goes on until the guard is over (noclip_guard: Link stopped + NOCLIP_GUARD_S), or the fall
       reset, the life refill and the god mode stopped the frame after noclip went off (2026-10-30: died landing from 2103 m
       with no "Link stopped" line) and the god mode was never cleared */
    if (noclip.holding || atomic_load(&noclip_settle.on) || runtime_now_ns() < atomic_load(&noclip_guard_until_ns)) atomic_store(&runtime_survey_pending, 1);
#if defined(TOTK_GAME_V143)
    if (atomic_load(&event_waiting)) atomic_store(&runtime_survey_pending, 1);   /* queued while this tick ran */
#endif
    in_tick = 0;
}
#endif   /* GAME_BOTW */
