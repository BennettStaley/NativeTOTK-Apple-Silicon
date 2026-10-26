/* Unit test for third_party/d2r_gfx/gfx/nvn/fps_overlay.c: glyph pixels of the FPS and screen-size text against hand-written patterns, box
   bounds for every value, screen size and scale, frame-rate measurement, and a negative control
   proving the pattern check detects a wrong pixel.
   Prints one JSON line; exit 1 on failure. */
#include "fps_overlay.h"
#include "totk_sysload.h"
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static const uint8_t TEXT[4] = {120, 255, 80, 255}, BOX[4] = {12, 12, 12, 255};
static uint8_t px[FPS_OVERLAY_TEX_W * FPS_OVERLAY_TEX_H * 4];

/* "FPS 60" at scale 1, written out pixel by pixel (glyph columns separated by one blank column). */
static const char *const FPS60[7] = {
    "11111 11110 01111 00000 00110 01110",
    "10000 10001 10000 00000 01000 10001",
    "10000 10001 10000 00000 10000 10011",
    "11110 11110 01110 00000 11110 10101",
    "10000 10000 00001 00000 10001 11001",
    "10000 10000 00001 00000 10001 10001",
    "10000 10000 11110 00000 01110 01110",
};

static _Atomic int g_spin_stop;
static void *spin_main(void *arg) { (void)arg; pthread_setname_np("probe-spin"); volatile unsigned long x = 0; while (!atomic_load(&g_spin_stop)) x++; return NULL; }

static int pixel_is(int x, int y, const uint8_t want[4]) { return !memcmp(px + 4 * ((size_t)y * FPS_OVERLAY_TEX_W + x), want, 4); }

/* "8x9" after "FPS 60  " (8 characters: x = 2 + 48 at scale 1) (the box ends right after it). */
static const char *const LINE2[7] = {
    "01110 00000 01110",
    "10001 00000 10001",
    "10001 10001 10001",
    "01110 01010 01111",
    "10001 00100 00001",
    "10001 01010 00010",
    "01110 10001 01100",
};

static int pattern_at(const char *const rows[7], int x0) {   /* text area starts at y = 2 at scale 1 */
    int bad = 0;
    for (int r = 0; r < 7; r++)
        for (int c = 0; rows[r][c]; c++) {
            int on = rows[r][c] == '1';
            if (on ? !pixel_is(x0 + c, 2 + r, TEXT) : !pixel_is(x0 + c, 2 + r, BOX)) bad++;
        }
    return bad;
}
static int pattern_mismatches(const char *const rows[7]) { return pattern_at(rows, 2); }

/* The system-font lines (CoreText, antialiased): a pixel counts as text when it is clearly green / yellow. */
static const uint8_t *at(int x, int y) { return px + 4 * ((size_t)y * FPS_OVERLAY_TEX_W + x); }
static int is_green(int x, int y) { const uint8_t *p = at(x, y); return p[1] > 170 && p[2] < 150; }                  /* B G R A: text {120,255,80} */
static int is_yellow(int x, int y) { const uint8_t *p = at(x, y); return p[2] > 180 && p[1] > 150 && p[0] < 110; }  /* highlight {40,230,255} */
static int outside_dirty(int w, int h) {   /* pixels outside the box that are not fully transparent */
    int n = 0;
    for (int y = 0; y < FPS_OVERLAY_TEX_H; y++)
        for (int x = 0; x < FPS_OVERLAY_TEX_W; x++)
            if ((x >= w || y >= h) && memcmp(at(x, y), (const uint8_t[4]){0, 0, 0, 0}, 4)) n++;
    return n;
}
static void colours(int w, int h, int *green, int *yellow) {
    *green = *yellow = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { *green += is_green(x, y); *yellow += is_yellow(x, y); }
}

int main(void) {
    int w, h, green, yellow;
    /* The 5x7 pixel font (lines of ASCII only: the bottom-right load line): glyph pixels against the written patterns. */
    fps_overlay_raster_line("FPS 60  8x9", 0, 1, px, &w, &h);
    CHECK(w == 11 * 6 - 1 + 4 && h == 11, "box size %dx%d (\"FPS 60  8x9\" is 11 characters, one line)", w, h);
    CHECK(pattern_mismatches(FPS60) == 0, "FPS 60 glyph pixels differ from the written pattern (%d)", pattern_mismatches(FPS60));
    CHECK(pattern_at(LINE2, 2 + 8 * 6) == 0, "resolution glyph pixels differ from the written pattern (%d)", pattern_at(LINE2, 2 + 8 * 6));
    char broken[7][40];   /* negative control: one flipped pixel must be detected */
    const char *rows[7];
    for (int r = 0; r < 7; r++) { strcpy(broken[r], FPS60[r]); rows[r] = broken[r]; }
    broken[3][26] = broken[3][26] == '1' ? '0' : '1';
    CHECK(pattern_mismatches(rows) == 1, "negative control: flipped pixel not detected exactly once");

    /* The FPS box: the system font (it has "MetalFX on", which the pixel font cannot draw), one line on the dark box; the
       box grows with what is shown. */
    int base, grown;
    fps_overlay_raster(&(fps_overlay_info){.fps = 97, .screen_w = 2560, .screen_h = 1440}, 2, px, &base, &h);
    CHECK(base > 40 && h == 11 * 2, "\"FPS 97  2560x1440\" at scale 2: box %dx%d", base, h);
    fps_overlay_raster(&(fps_overlay_info){.fps = 97, .screen_w = 2560, .screen_h = 1440, .mfx = 1}, 2, px, &grown, &h);
    CHECK(grown > base + 40, "MetalFX on: \"  MetalFX on\" after the size (box %d, without it %d)", grown, base);
    fps_overlay_raster(&(fps_overlay_info){.fps = 50, .screen_w = 2560, .screen_h = 1440, .gen = 50}, 2, px, &grown, &h);
    CHECK(grown > base, "frame interpolation on: \"+50\" after the fps (box %d, without it %d)", grown, base);
    fps_overlay_raster(&(fps_overlay_info){.fps = 97, .screen_w = 2560, .screen_h = 1440, .big = 1}, 2, px, &grown, &h);
    CHECK(grown > base, "true 2K: \"  2K\" at the end (box %d, without it %d)", grown, base);
    fps_overlay_raster(&(fps_overlay_info){.fps = 999, .screen_w = 99999, .screen_h = 99999, .mfx = 1, .gen = 5000, .big = 1,
                                           .scene_w = 99999, .scene_h = 99999}, FPS_OVERLAY_MAX_SCALE, px, &w, &h);   /* longest line: fits, or drops a scale */
    CHECK(w > 0 && w <= FPS_OVERLAY_TEX_W && h <= FPS_OVERLAY_TEX_H, "longest FPS line %dx%d outside the texture", w, h);

    const int values[] = {-1, 0, 7, 30, 60, 120, 999, 5000};
    const int sizes[][2] = {{0, 0}, {1920, 1080}, {3440, 1440}, {99999, 99999}};
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++)
        for (size_t z = 0; z < sizeof sizes / sizeof sizes[0]; z++)
        for (int mfx = 0; mfx <= 1; mfx++)
        for (int s = 1; s <= FPS_OVERLAY_MAX_SCALE; s++) {
            fps_overlay_raster(&(fps_overlay_info){.fps = values[i], .screen_w = sizes[z][0], .screen_h = sizes[z][1], .mfx = mfx}, s, px, &w, &h);
            CHECK(w > 0 && h > 0 && w <= FPS_OVERLAY_TEX_W && h <= FPS_OVERLAY_TEX_H, "value %d scale %d box %dx%d outside texture", values[i], s, w, h);
            colours(w, h, &green, &yellow);
            int dirty = outside_dirty(w, h);
            CHECK((s < 2 || green > 0) && yellow == 0 && dirty == 0, "value %d scale %d: %d text pixels, %d yellow, %d pixels outside the box", values[i], s, green, yellow, dirty);
        }

    CHECK(fps_overlay_scale(1440) == 4 && fps_overlay_scale(900) == 2 && fps_overlay_scale(4000) == FPS_OVERLAY_MAX_SCALE && fps_overlay_scale(10) == 1,
          "scale for 1440/900/4000/10 = %d/%d/%d/%d", fps_overlay_scale(1440), fps_overlay_scale(900), fps_overlay_scale(4000), fps_overlay_scale(10));

    /* The automatic shader collection box (bottom-left, no pipeline count): its text, bounds of the longest line at every
       scale, and the yellow highlight. */
    char line[96];
    shader_overlay_text(line, sizeof line, 2, 3, 7428, 12);
    CHECK(!strcmp(line, "Shaders 2 compiling / 3 new / 7428 total"), "shader box text \"%s\"", line);
    shader_overlay_text(line, sizeof line, 0, 0, -1, 0);
    CHECK(!strcmp(line, "Shaders 0 compiling / 0 new / -- total"), "shader box text before the cache is counted \"%s\"", line);
    shader_overlay_text(line, sizeof line, 123456, 123456, 9999999, 123456);
    CHECK(!strcmp(line, "Shaders 99999 compiling / 99999 new / 999999 total"), "shader box text clamped \"%s\"", line);
    CHECK(!fps_overlay_line_pixel_font(line), "shader line \"%s\" drawn with the pixel font, which lacks its lower-case letters", line);
    CHECK(fps_overlay_line_pixel_font("FPS 60  2560x1440"), "the FPS line left the pixel font");
    { char fl[96];   /* output resolution, then the internal (scene) one in brackets (2026-10-23) */
      fps_overlay_text(&(fps_overlay_info){.fps = 60, .screen_w = 1920, .screen_h = 1080, .scene_w = 1600, .scene_h = 900}, fl, sizeof fl);
      CHECK(!strcmp(fl, "FPS 60  1920x1080 (1600x900)"), "fps line with the internal resolution \"%s\"", fl);
      fps_overlay_text(&(fps_overlay_info){.fps = 30, .screen_w = 1280, .screen_h = 720}, fl, sizeof fl);
      CHECK(!strcmp(fl, "FPS 30  1280x720"), "fps line before the scene is known \"%s\"", fl); }
    shader_overlay_text(line, sizeof line, 123456, 123456, 9999999, 123456);
    CHECK(!fps_overlay_has_glyph('Q'), "negative control: 'Q' has no glyph in the pixel font but was reported");
    for (int s = 1; s <= FPS_OVERLAY_MAX_SCALE; s++) {
        fps_overlay_raster_line(line, s & 1, s, px, &w, &h);
        CHECK(w > 0 && h > 0 && w <= FPS_OVERLAY_TEX_W && h <= FPS_OVERLAY_TEX_H, "longest shader line at scale %d: box %dx%d outside the texture", s, w, h);
    }
    shader_overlay_text(line, sizeof line, 2, 3, 7428, 12);
    fps_overlay_raster_line(line, 1, 3, px, &w, &h);
    colours(w, h, &green, &yellow);
    CHECK(h == 11 * 3 && yellow > 0 && green == 0 && outside_dirty(w, h) == 0, "highlighted line: box %dx%d, %d yellow and %d green pixels", w, h, yellow, green);
    fps_overlay_raster_line(line, 0, 3, px, &w, &h);
    colours(w, h, &green, &yellow);
    CHECK(yellow == 0 && green > 0, "plain line: %d yellow and %d green pixels", yellow, green);

    CHECK(fps_overlay_value() == -1, "value before any present");
    CHECK(fps_overlay_avg_value() == -1 && fps_overlay_low_value() == -1, "average / 1%% low before any present");
    uint64_t k = 0; double t = 100.0;
    for (int i = 0; i < 120; i++) { fps_overlay_note(++k, t); t += 1.0 / 60; }
    CHECK(fps_overlay_value() == 60, "60 presents per second measured as %d", fps_overlay_value());
    for (int i = 0; i < 60; i++) { fps_overlay_note(++k, t); t += 1.0 / 30; }
    CHECK(fps_overlay_value() == 30, "30 presents per second measured as %d", fps_overlay_value());
    for (int i = 0; i < 60; i++) { k += 2; fps_overlay_note(k, t); t += 1.0 / 30; }   /* display link merged every other game frame */
    CHECK(fps_overlay_value() == 60, "merged frames: 60 game presents per second measured as %d", fps_overlay_value());
    /* Readout detail: the average and the 1% low over the last FPS_STATS_SECONDS. Steady 60 first (the earlier presents
       fall out of the window after a pause), then one 100 ms hitch a second: five slow frames among ~270, so the slowest
       1% (two frames) are hitches -> 10 fps, while the average barely moves. */
    t += 20.0;
    for (int i = 0; i < 6 * 60; i++) { fps_overlay_note(++k, t); t += 1.0 / 60; }
    CHECK(fps_overlay_avg_value() == 60 && fps_overlay_low_value() == 60, "steady 60: average %d, 1%% low %d", fps_overlay_avg_value(), fps_overlay_low_value());
    for (int sec = 0; sec < 6; sec++) {
        for (int i = 0; i < 54; i++) { fps_overlay_note(++k, t); t += 1.0 / 60; }
        fps_overlay_note(++k, t); t += 0.1;
    }
    CHECK(fps_overlay_avg_value() >= 52 && fps_overlay_avg_value() <= 57, "one hitch a second: average %d (about 55)", fps_overlay_avg_value());
    CHECK(fps_overlay_low_value() == 10, "one 100 ms hitch a second: 1%% low %d, expected 10", fps_overlay_low_value());
    for (int i = 0; i < 6 * 30; i++) { k += 2; fps_overlay_note(k, t); t += 1.0 / 30; }   /* merged: two game frames a present */
    CHECK(fps_overlay_avg_value() == 60 && fps_overlay_low_value() == 60, "merged presents: average %d, 1%% low %d (each frame its half)", fps_overlay_avg_value(), fps_overlay_low_value());
    {
        char ft[128];
        fps_overlay_text(&(fps_overlay_info){60, 2560, 1440, 0, 0, 0, 1, 59, 48, 0, 0}, ft, sizeof ft);
        CHECK(!strcmp(ft, "FPS 60  avg 59  1% low 48  2560x1440"), "detailed line \"%s\"", ft);
        fps_overlay_text(&(fps_overlay_info){60, 2560, 1440, 0, 0, 0, 1, -1, -1, 0, 0}, ft, sizeof ft);
        CHECK(!strcmp(ft, "FPS 60  avg --  1% low --  2560x1440"), "detailed line before a measurement \"%s\"", ft);
        fps_overlay_text(&(fps_overlay_info){60, 2560, 1440, 0, 0, 0, 0, 59, 48, 0, 0}, ft, sizeof ft);
        CHECK(!strcmp(ft, "FPS 60  2560x1440"), "simple line \"%s\" (detail off hides the numbers)", ft);
        int simple, detailed;
        fps_overlay_raster(&(fps_overlay_info){60, 2560, 1440, 0, 0, 0, 0, 0, 0, 0, 0}, 2, px, &simple, &h);
        fps_overlay_raster(&(fps_overlay_info){60, 2560, 1440, 0, 0, 0, 1, 59, 48, 0, 0}, 2, px, &detailed, &h);
        CHECK(detailed > simple + 60, "detailed box %d wider than simple %d", detailed, simple);
        for (int s = 1; s <= FPS_OVERLAY_MAX_SCALE; s++) {
            fps_overlay_raster(&(fps_overlay_info){999, 99999, 99999, 1, 5000, 1, 1, 999, 999, 0, 0}, s, px, &w, &h);
            CHECK(w > 0 && w <= FPS_OVERLAY_TEX_W && h <= FPS_OVERLAY_TEX_H && outside_dirty(w, h) == 0, "longest detailed line at scale %d: box %dx%d", s, w, h);
        }
    }
    /* Interpolated frames shown: their own rate, independent of the game rate; a gap of more than a second (interpolation
       switched off and on) starts a new window instead of averaging the off time in. */
    CHECK(fps_overlay_gen_value() == -1, "generated rate before any interpolated frame");
    uint64_t gk = 0; double gt = 200.0;
    for (int i = 0; i < 100; i++) { fps_overlay_note_gen(++gk, gt); gt += 1.0 / 50; }
    CHECK(fps_overlay_gen_value() == 50, "50 interpolated frames per second measured as %d", fps_overlay_gen_value());
    CHECK(fps_overlay_value() == 60, "the game rate is unaffected by the generated one (%d)", fps_overlay_value());
    gt += 30.0;   /* off for 30 s, then on again */
    for (int i = 0; i < 60; i++) { fps_overlay_note_gen(++gk, gt); gt += 1.0 / 50; }
    CHECK(fps_overlay_gen_value() == 50, "after a gap: 50 per second, not an average over the gap (%d)", fps_overlay_gen_value());

    /* The bottom-right load line: text, its glyphs, the widest line within the wider texture at every scale, the busy-time
       union of in-order command buffers, and one real sample of this machine (cores split into P and E, GPU known). */
    sysload_info li = {4, 6, {55, 56, 55, 53}, {43, 29, 20, 12, 9, 7}, 99, 70, 47, "x", 0, -1, -1, -1, -1, -1, -1, 0};
    sysload_text(line, sizeof line, &li);
    CHECK(!strcmp(line, "GPU 99% (70%)  P 55% 56% 55% 53%  E 43% 29% 20% 12%  9%  7%  T 47%"), "load line \"%s\"", line);
    li.gpu_sys = -1; li.gpu_game = -1; li.thread_max = -1; sysload_text(line, sizeof line, &li);
    CHECK(!strcmp(line, "GPU --  P 55% 56% 55% 53%  E 43% 29% 20% 12%  9%  7%"), "load line without GPU numbers \"%s\"", line);
    for (const char *c = "PEGUT%()"; *c; c++) CHECK(fps_overlay_has_glyph(*c), "no glyph for '%c' (load line)", *c);
    static uint8_t lpx[LOAD_OVERLAY_TEX_W * FPS_OVERLAY_TEX_H * 4];
    sysload_info wide = {4, 6, {100, 100, 100, 100}, {100, 100, 100, 100, 100, 100}, 100, 100, 100, "x", 0, -1, -1, -1, -1, -1, -1, 0};
    char wl[128]; sysload_text(wl, sizeof wl, &wide);
    int unit = (int)strlen(wl) * 6 - 1 + 4;
    for (int s = 1; s <= FPS_OVERLAY_MAX_SCALE; s++) {
        fps_overlay_raster_line_tex(wl, 0, s, lpx, LOAD_OVERLAY_TEX_W, FPS_OVERLAY_TEX_H, &w, &h);
        CHECK(w > 0 && h > 0 && w <= LOAD_OVERLAY_TEX_W && h <= FPS_OVERLAY_TEX_H && w % unit == 0,
              "widest load line at scale %d: box %dx%d (the whole line, within the texture)", s, w, h);
    }
    fps_overlay_raster_line_tex(wl, 0, 4, lpx, LOAD_OVERLAY_TEX_W, FPS_OVERLAY_TEX_H, &w, &h);
    CHECK(w == unit * 4, "the widest load line keeps scale 4 (1440p) in its texture: width %d", w);
    /* The power line: every value, a draining battery (highlighted), charging, no charger, nothing known. */
    char pline[128];   // the panel keeps 128 bytes per line (fps_overlay.c g_hud_text)
    sysload_info pw = {0}; pw.have_power = 1; pw.cpu_w = 9.04; pw.gpu_w = 21.26; pw.sys_w = 42.4; pw.in_w = 36.2; pw.bat_w = -6.3; pw.charger_w = 100; pw.left_min = -1;
    CHECK(sysload_power_text(pline, sizeof pline, &pw) == 1 && !strcmp(pline, "CPU 9.0 W  GPU 21.3 W  Total 42 W  Charger 100 W (36 W in)  Battery -6 W draining"), "power line, draining \"%s\"", pline);
    pw.left_min = 125;
    CHECK(sysload_power_text(pline, sizeof pline, &pw) == 1 && strstr(pline, "Battery -6 W draining, 2 h 05 min left"), "power line, time left \"%s\"", pline);
    pw.left_min = 42;
    CHECK(strstr((sysload_power_text(pline, sizeof pline, &pw), pline), "draining, 42 min left"), "power line, minutes left \"%s\"", pline);
    pw.left_min = 900;
    CHECK(strstr((sysload_power_text(pline, sizeof pline, &pw), pline), "draining, over 10 h left"), "power line, long time left \"%s\"", pline);
    pw.left_min = -1;
    pw.bat_w = 3.2; pw.in_w = 46;
    CHECK(sysload_power_text(pline, sizeof pline, &pw) == 0 && !strcmp(pline, "CPU 9.0 W  GPU 21.3 W  Total 42 W  Charger 100 W (46 W in)  Battery +3 W charging"), "power line, charging \"%s\"", pline);
    pw.bat_w = -0.2;
    CHECK(sysload_power_text(pline, sizeof pline, &pw) == 0 && strstr(pline, "Battery 0 W"), "power line, battery held \"%s\"", pline);
    pw.charger_w = 0; pw.in_w = 0; pw.bat_w = -42.4; pw.left_min = 104;
    CHECK(sysload_power_text(pline, sizeof pline, &pw) == 1 && strstr(pline, "No charger  Battery -42 W draining, 1 h 44 min left"), "power line, on battery \"%s\"", pline);
    pw = (sysload_info){0}; pw.cpu_w = pw.gpu_w = pw.sys_w = pw.in_w = -1; pw.bat_w = NAN; pw.charger_w = -1; pw.left_min = -1; pw.have_power = 1;
    CHECK(sysload_power_text(pline, sizeof pline, &pw) == -1 && !pline[0], "power line with nothing known is empty \"%s\"", pline);
    sysload_busy b = {0, 0};
    sysload_busy_add(&b, 1.0, 1.005); sysload_busy_add(&b, 1.005, 1.010); sysload_busy_add(&b, 1.008, 1.012); sysload_busy_add(&b, 1.020, 1.020);
    CHECK(b.busy > 0.01199 && b.busy < 0.01201, "busy union of 0-5, 5-10, 8-12 ms (and an empty one) is 12 ms, got %.6f s", b.busy);
    pthread_t spin; pthread_create(&spin, NULL, spin_main, NULL);   // a thread that keeps one core busy: the busiest thread
    sysload_start(); usleep(1200000);
    sysload_info real; int got = sysload_get(&real);
    CHECK(got && real.np >= 1 && real.np + real.ne >= 2 && real.gpu_sys >= 0 && real.gpu_sys <= 100 && real.gpu_game == 0 && real.thread_max >= 0 && real.thread_max <= 100 && real.thread_name[0],
          "a real sample: %d P and %d E cores, busiest thread %d%% (%s), GPU %d%%, game GPU %d%% (no command buffers in this probe)", real.np, real.ne, real.thread_max, real.thread_name, real.gpu_sys, real.gpu_game);
    for (int i = 0; got && i < real.np; i++) CHECK(real.p[i] >= 0 && real.p[i] <= 100, "P core %d load %d", i, real.p[i]);
    for (int i = 0; got && i < real.ne; i++) CHECK(real.e[i] >= 0 && real.e[i] <= 100, "E core %d load %d", i, real.e[i]);
    CHECK(got && real.thread_max >= 80 && !strcmp(real.thread_name, "probe-spin"), "the spinning thread is the busiest: %d%% (%s)", real.thread_max, real.thread_name);
    atomic_store(&g_spin_stop, 1); pthread_join(spin, NULL);
    if (got) { sysload_text(line, sizeof line, &real); fprintf(stderr, "this machine now: %s (busiest thread %s)\n", line, real.thread_name); }
    CHECK(got && real.have_power && real.cpu_w > 0 && real.cpu_w < 200 && real.gpu_w >= 0 && real.gpu_w < 200, "a real power sample: CPU %.2f W, GPU %.2f W (IOReport)", real.cpu_w, real.gpu_w);
    if (got && sysload_power_text(line, sizeof line, &real) >= 0) fprintf(stderr, "this machine's power now: %s\n", line);

    printf("{\"status\":\"%s\",\"probe\":\"probe-fps-overlay\",\"checked\":%d,\"failures\":%d,\"scope\":\"5x7 glyph pixels (ASCII lines), the system-font FPS box (MetalFX label, box growth, text colour, nothing outside the box), box bounds for values -1..5000 and screen sizes at scales 1..%d, scale choice, game-present and interpolated-frame rate measurement, FPS 50+50 form, Readout detail (average and 1%% low over the last 5 s: steady, hitches, merged presents; the detailed line and its box), bottom-right load line (text, glyphs, width, GPU busy union, busiest thread found, one real sample), shader collection box text / glyphs / bounds / highlight; negative controls flip one pixel and ask for a missing glyph\"}\n",
           failures ? "fail" : "pass", checks, failures, FPS_OVERLAY_MAX_SCALE);
    return failures ? 1 : 0;
}
