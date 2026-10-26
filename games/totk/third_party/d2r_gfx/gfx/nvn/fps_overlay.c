#include "fps_overlay.h"
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { GLYPH_W = 5, GLYPH_H = 7, ADVANCE = 6, PAD = 2, BOX_ROWS = GLYPH_H + 2 * PAD, MAX_CHARS = 80 };
static const uint8_t TEXT_BGRA[4] = {120, 255, 80, 255}, BOX_BGRA[4] = {12, 12, 12, 255}, HIGHLIGHT_BGRA[4] = {40, 230, 255, 255};   // green / dark / yellow

typedef struct { char c; uint8_t rows[GLYPH_H]; } Glyph;   /* each row: 5 bits, bit 4 = leftmost pixel */
static const Glyph FONT[] = {
    {'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}}, {'1', {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}}, {'3', {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e}},
    {'4', {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}}, {'5', {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e}},
    {'6', {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}}, {'7', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}}, {'9', {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c}},
    {'F', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}}, {'P', {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}},
    {'S', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}}, {'-', {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}},
    {'x', {0x00, 0x00, 0x11, 0x0a, 0x04, 0x0a, 0x11}}, {'>', {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}},
    {'M', {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'X', {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}},
    {'H', {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}}, {'A', {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'D', {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}}, {'E', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}},
    {'R', {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}}, {'+', {0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00}},
    {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}}, {'O', {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'G', {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}}, {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}}, {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}}, {'T', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {' ', {0, 0, 0, 0, 0, 0, 0}},
};

/* Key 1 cycles all -> minimal (FPS only) -> off -> all. TOTK_HINTS=0 starts off; the settings menu's "Performance
   readout" switches between all and off (fps_overlay_set_hints); fps_overlay_set_hud_mode sets one mode directly. */
static _Atomic int g_hud_mode = -1;
static int hud_mode_now(void) {
    int v = atomic_load_explicit(&g_hud_mode, memory_order_relaxed);
    if (v < 0) { const char *e = getenv("TOTK_HINTS"); int start = e && !strcmp(e, "0") ? HUD_MODE_OFF : HUD_MODE_ALL;
        atomic_compare_exchange_strong(&g_hud_mode, &v, start); v = atomic_load(&g_hud_mode); }
    return v;
}
int fps_overlay_hud_mode(void) { return hud_mode_now(); }
int fps_overlay_hints_enabled(void) { return fps_overlay_hud_mode() != HUD_MODE_OFF; }
int fps_overlay_full_hud(void) { return fps_overlay_hud_mode() == HUD_MODE_ALL; }
void fps_overlay_set_hints(int visible) { (void)hud_mode_now(); atomic_store(&g_hud_mode, visible ? HUD_MODE_ALL : HUD_MODE_OFF); }
void fps_overlay_set_hud_mode(int mode) {
    if (mode == HUD_MODE_OFF || mode == HUD_MODE_ALL || mode == HUD_MODE_MINIMAL) { (void)hud_mode_now(); atomic_store_explicit(&g_hud_mode, mode, memory_order_relaxed); }
}
int fps_overlay_toggle_hints(void) {
    int mode = hud_mode_now(), next;
    do next = mode == HUD_MODE_ALL ? HUD_MODE_MINIMAL : mode == HUD_MODE_MINIMAL ? HUD_MODE_OFF : HUD_MODE_ALL;
    while (!atomic_compare_exchange_weak_explicit(&g_hud_mode, &mode, next, memory_order_relaxed, memory_order_relaxed));
    return next;
}

/* The settings menu's "Readout detail": 1 adds the average and the 1% low of the last seconds to the FPS line. */
static _Atomic int g_detail;
void fps_overlay_set_detail(int detailed) { atomic_store(&g_detail, detailed ? 1 : 0); }
int fps_overlay_detail(void) { return atomic_load(&g_detail); }

int fps_overlay_enabled(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_FPS_OVERLAY"); on = !(e && !strcmp(e, "0")); }
    return on;
}

int fps_overlay_scale(double view_h) {
    int s = (int)lround(view_h * 0.03 / BOX_ROWS);
    return s < 1 ? 1 : s > FPS_OVERLAY_MAX_SCALE ? FPS_OVERLAY_MAX_SCALE : s;
}

static const Glyph *glyph(char c) {
    for (size_t i = 0; i < sizeof FONT / sizeof FONT[0]; i++) if (FONT[i].c == c) return &FONT[i];
    return &FONT[sizeof FONT / sizeof FONT[0] - 1];
}
int fps_overlay_has_glyph(char c) { return c == ' ' || glyph(c) != &FONT[sizeof FONT / sizeof FONT[0] - 1]; }

static void fill(uint8_t *out, int stride, int x0, int y0, int w, int h, const uint8_t bgra[4]) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) memcpy(out + 4 * ((size_t)y * stride + x), bgra, 4);
}

static void size_text(char *out, size_t n, int w, int h) {
    if (w > 0 && h > 0) snprintf(out, n, "%dx%d", w > 9999 ? 9999 : w, h > 9999 ? 9999 : h);
    else snprintf(out, n, "--");
}

static void draw_text(uint8_t *out, int stride, const char *text, int n, int scale, const uint8_t color[4]) {
    for (int i = 0; i < n; i++) {
        const Glyph *g = glyph(text[i]);
        for (int r = 0; r < GLYPH_H; r++)
            for (int c = 0; c < GLYPH_W; c++)
                if (g->rows[r] & (0x10 >> c))
                    fill(out, stride, (PAD + i * ADVANCE + c) * scale, (PAD + r) * scale, scale, scale, color);
    }
}

/* A line with characters the 5x7 font does not have (lower-case labels such as "MetalFX on" and "Shaders N compiling")
   in the system font (CoreText), with the same box height, padding and colours as the pixel font at this scale; the
   scale drops until the line fits the texture. */
static void raster_line_ct(const char *text, int highlight, int scale, uint8_t *out, int tex_w, int tex_h, int *box_w, int *box_h) {
    memset(out, 0, (size_t)tex_w * tex_h * 4);
    *box_w = *box_h = 0;
    CFStringRef str = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    if (!str) return;
    const uint8_t *c = highlight ? HIGHLIGHT_BGRA : TEXT_BGRA;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    const CGFloat comps[4] = {c[2] / 255.0, c[1] / 255.0, c[0] / 255.0, 1.0};
    CGColorRef color = CGColorCreate(cs, comps);
    for (scale = scale < 1 ? 1 : scale > FPS_OVERLAY_MAX_SCALE ? FPS_OVERLAY_MAX_SCALE : scale; scale >= 1; scale--) {
        CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 8.0 * scale, NULL);
        const void *keys[2] = {kCTFontAttributeName, kCTForegroundColorAttributeName}, *vals[2] = {font, color};
        CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFAttributedStringRef as = CFAttributedStringCreate(NULL, str, attrs);
        CTLineRef line = CTLineCreateWithAttributedString(as);
        CGFloat ascent = 0, descent = 0;
        double tw = CTLineGetTypographicBounds(line, &ascent, &descent, NULL);
        int w = (int)ceil(tw) + 2 * PAD * scale, h = BOX_ROWS * scale;
        int fits = w <= tex_w && h <= tex_h;
        if (fits || scale == 1) {
            if (w > tex_w) w = tex_w;
            if (h > tex_h) h = tex_h;
            fill(out, tex_w, 0, 0, w, h, BOX_BGRA);
            CGContextRef ctx = CGBitmapContextCreate(out, (size_t)tex_w, (size_t)tex_h, 8, (size_t)tex_w * 4, cs,
                                                     kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);   // B, G, R, A in memory
            if (ctx) {   // memory row 0 is the top of the texture; Core Graphics counts y up from the bottom row
                CGContextClipToRect(ctx, CGRectMake(0, tex_h - h, w, h));
                CGContextSetTextPosition(ctx, PAD * scale, tex_h - h / 2.0 - (ascent - descent) / 2.0);
                CTLineDraw(line, ctx);
                CGContextRelease(ctx);
            }
            *box_w = w; *box_h = h;
        }
        CFRelease(line); CFRelease(as); CFRelease(attrs); CFRelease(font);
        if (fits || scale == 1) break;
    }
    CGColorRelease(color); CGColorSpaceRelease(cs); CFRelease(str);
}

/* The 5x7 font has only the letters the original labels use: any other character (Chinese, or English words such as
   "New shaders ... / total ...") sends the whole line to the system font instead of drawing blanks. */
int fps_overlay_line_pixel_font(const char *s) {
    for (; *s; s++) if (!fps_overlay_has_glyph(*s)) return 0;
    return 1;
}

void fps_overlay_raster_line_tex(const char *text, int highlight, int scale, uint8_t *out, int tex_w, int tex_h, int *box_w, int *box_h) {
    if (!fps_overlay_line_pixel_font(text)) { raster_line_ct(text, highlight, scale, out, tex_w, tex_h, box_w, box_h); return; }
    int n = (int)strnlen(text, MAX_CHARS);
    if (scale < 1) scale = 1;
    if (scale > FPS_OVERLAY_MAX_SCALE) scale = FPS_OVERLAY_MAX_SCALE;
    while (scale > 1 && ((n * ADVANCE - 1 + 2 * PAD) * scale > tex_w || BOX_ROWS * scale > tex_h)) scale--;   // never past the texture
    while (n > 1 && (n * ADVANCE - 1 + 2 * PAD) > tex_w) n--;   // scale 1 and still too long: cut the line
    int w = (n * ADVANCE - 1 + 2 * PAD) * scale, h = BOX_ROWS * scale;
    memset(out, 0, (size_t)tex_w * tex_h * 4);
    fill(out, tex_w, 0, 0, w, h, BOX_BGRA);
    draw_text(out, tex_w, text, n, scale, highlight ? HIGHLIGHT_BGRA : TEXT_BGRA);
    *box_w = w; *box_h = h;
}
void fps_overlay_raster_line(const char *text, int highlight, int scale, uint8_t *out, int *box_w, int *box_h) {
    fps_overlay_raster_line_tex(text, highlight, scale, out, FPS_OVERLAY_TEX_W, FPS_OVERLAY_TEX_H, box_w, box_h);
}

void fps_overlay_text(const fps_overlay_info *info, char *text, size_t n) {
    char fps[8], ss[12];   // ss: the output resolution, then the internal one in brackets
    if (info->fps < 0) snprintf(fps, sizeof fps, "--");
    else snprintf(fps, sizeof fps, "%d", info->fps > 999 ? 999 : info->fps);
    size_text(ss, sizeof ss, info->screen_w, info->screen_h);
    char sc[16] = "";
    if (info->scene_w > 0 && info->scene_h > 0) snprintf(sc, sizeof sc, " (%dx%d)", info->scene_w > 9999 ? 9999 : info->scene_w, info->scene_h > 9999 ? 9999 : info->scene_h);
    char gen[8] = "";
    if (info->gen > 0 && info->fps >= 0) snprintf(gen, sizeof gen, "+%d", info->gen > 999 ? 999 : info->gen);   // interpolated frames a second (key 2)
    // MetalFX did the upscale in the last frames (key 3 selects the manual MetalFX mode); 2K: true 2K output.
    // The whole line in the system font, so switching MetalFX does not switch the font.
    char detail[48] = "";
    if (info->detail) {   // "avg 59  1% low 48" over the last FPS_STATS_SECONDS (settings: Readout detail)
        char avg[8], low[8];
        if (info->avg < 0) snprintf(avg, sizeof avg, "--"); else snprintf(avg, sizeof avg, "%d", info->avg > 999 ? 999 : info->avg);
        if (info->low < 0) snprintf(low, sizeof low, "--"); else snprintf(low, sizeof low, "%d", info->low > 999 ? 999 : info->low);
        snprintf(detail, sizeof detail, "  avg %s  1%% low %s", avg, low);
    }
    snprintf(text, n, "FPS %s%s%s  %s%s%s%s", fps, gen, detail, ss, sc, info->mfx ? "  MetalFX on" : "", info->big ? "  2K" : "");
}

void fps_overlay_raster(const fps_overlay_info *info, int scale, uint8_t *out, int *box_w, int *box_h) {
    char text[MAX_CHARS + 1];
    fps_overlay_text(info, text, sizeof text);
    raster_line_ct(text, 0, scale, out, FPS_OVERLAY_TEX_W, FPS_OVERLAY_TEX_H, box_w, box_h);
}

int hud_compact(void) {
    static int on = -1;
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    const int def = 0;   // the iOS frontend has no corner panel
#else
    const int def = 1;
#endif
    if (on < 0) { const char *e = getenv("TOTK_HUD"); on = e ? strcmp(e, "corners") != 0 : def; }
    return on;
}

static pthread_mutex_t g_hud_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_hud_text[HUD_LINES][128];
static int g_hud_hl[HUD_LINES], g_hud_set[HUD_LINES];
void hud_publish(int line, const char *text, int highlight) {
    if (line < 0 || line >= HUD_LINES) return;
    pthread_mutex_lock(&g_hud_mu);
    snprintf(g_hud_text[line], sizeof g_hud_text[line], "%s", text); g_hud_hl[line] = highlight; g_hud_set[line] = 1;
    pthread_mutex_unlock(&g_hud_mu);
}
int hud_snapshot(int line, char *out, size_t n, int *highlight) {
    if (line < 0 || line >= HUD_LINES) return 0;
    pthread_mutex_lock(&g_hud_mu);
    int set = g_hud_set[line];
    if (set) { snprintf(out, n, "%s", g_hud_text[line]); if (highlight) *highlight = g_hud_hl[line]; }
    pthread_mutex_unlock(&g_hud_mu);
    return set;
}

/* The settings menu's Shader counter, live: on / off now; off also takes its line out of the corner panel. */
static _Atomic int g_shader_overlay = -1;
void shader_overlay_set(int on) {
    atomic_store(&g_shader_overlay, on ? 1 : 0);
    if (!on) { pthread_mutex_lock(&g_hud_mu); g_hud_set[HUD_SHADER] = 0; pthread_mutex_unlock(&g_hud_mu); }
}
int shader_overlay_enabled(void) {
    int set = atomic_load(&g_shader_overlay); if (set >= 0) return set;
    static int on = -1;
    /* On by default on the iPhone too since it shows the render pipelines (user 2026-10-22: only the FPS was there when the
       game was opened from the home screen); it was off there because the phone translates no shaders. */
    const int def = 1;
    if (on < 0) { const char *e = getenv("TOTK_SHADER_OVERLAY"); on = e ? strcmp(e, "0") != 0 : def; }
    return on;
}

/* "Shaders 2 compiling / 10 new / 18074 total" (user, 2026-10-20): what Metal is building right now (pipelines, the
   warm-up's remaining items), shaders translated for the first time this session, and every shader in the cache. */
void shader_overlay_text(char *out, size_t n, uint64_t compiling, uint64_t fresh, int64_t total, uint64_t pipelines) {
    char t[16];
    (void)pipelines;   // user 2026-10-17: the pipeline count is not shown
    if (total < 0) snprintf(t, sizeof t, "--"); else snprintf(t, sizeof t, "%lld", (long long)(total > 999999 ? 999999 : total));
    snprintf(out, n, "Shaders %llu compiling / %llu new / %s total",
             (unsigned long long)(compiling > 99999 ? 99999 : compiling), (unsigned long long)(fresh > 99999 ? 99999 : fresh), t);
}
/* The render pipeline line under the shaders (user 2026-10-22: under the FPS and shaders, show the render pipelines, new /
   total): pipelines this run built that this profile had never built (new to its warm list), and every render pipeline
   ready this run (warm-up and play). */
void pso_overlay_text(char *out, size_t n, uint64_t fresh, int64_t total) {
    char t[16];
    if (total < 0) snprintf(t, sizeof t, "--"); else snprintf(t, sizeof t, "%lld", (long long)(total > 999999 ? 999999 : total));
    snprintf(out, n, "New render pipelines %llu / total %s", (unsigned long long)(fresh > 99999 ? 99999 : fresh), t);
}

/* A count's rate, re-measured every half second (game presents; interpolated frames shown). stale_s > 0: a gap longer
   than that starts a new window (interpolation switched off and on again must not average over the time it was off). */
typedef struct { pthread_mutex_t mu; double t0, stale_s; uint64_t k0; _Atomic int value; } rate_meter;
static rate_meter g_game = {PTHREAD_MUTEX_INITIALIZER, -1, 0, 0, -1}, g_gen = {PTHREAD_MUTEX_INITIALIZER, -1, 1.0, 0, -1};
static void rate_note(rate_meter *m, uint64_t k, double now_s) {
    pthread_mutex_lock(&m->mu);
    if (m->t0 < 0 || k < m->k0 || (m->stale_s > 0 && now_s - m->t0 > m->stale_s)) { m->t0 = now_s; m->k0 = k; }
    else if (now_s - m->t0 >= 0.5) {
        atomic_store(&m->value, (int)lround((double)(k - m->k0) / (now_s - m->t0)));
        m->t0 = now_s; m->k0 = k;
    }
    pthread_mutex_unlock(&m->mu);
}

/* Average and 1% low over the last FPS_STATS_SECONDS of game presents: a ring of (present number, time); a present that
   carries several game frames (merged by the display link) counts each of them at its share of the interval. Re-measured
   every half second. 1% low: the frame rate of the slowest 1% of those frames (their mean frame time), at least one. */
enum { STATS_RING = 2048 };
static struct { pthread_mutex_t mu; double t[STATS_RING]; uint64_t k[STATS_RING]; unsigned head, n; double last_calc; _Atomic int avg, low; }
    g_stats = {PTHREAD_MUTEX_INITIALIZER, {0}, {0}, 0, 0, -1, -1, -1};
static int cmp_desc(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? 1 : x > y ? -1 : 0; }
static void stats_note(uint64_t k, double now_s) {
    pthread_mutex_lock(&g_stats.mu);
    if (g_stats.n && k < g_stats.k[(g_stats.head + STATS_RING - 1) % STATS_RING]) g_stats.n = 0;   // the count started again
    g_stats.t[g_stats.head] = now_s; g_stats.k[g_stats.head] = k; g_stats.head = (g_stats.head + 1) % STATS_RING;
    if (g_stats.n < STATS_RING) g_stats.n++;
    while (g_stats.n > 1 && now_s - g_stats.t[(g_stats.head + STATS_RING - g_stats.n) % STATS_RING] > FPS_STATS_SECONDS) g_stats.n--;
    if (g_stats.last_calc < 0 || now_s - g_stats.last_calc >= 0.5 || now_s < g_stats.last_calc) {
        g_stats.last_calc = now_s;
        unsigned first = (g_stats.head + STATS_RING - g_stats.n) % STATS_RING;
        double span = now_s - g_stats.t[first]; uint64_t frames = k - g_stats.k[first];
        if (g_stats.n >= 2 && span >= 1.0 && frames > 0) {
            static double ft[STATS_RING * 4]; size_t m = 0;
            for (unsigned i = 1; i < g_stats.n; i++) {
                unsigned a = (first + i - 1) % STATS_RING, b = (first + i) % STATS_RING;
                uint64_t dk = g_stats.k[b] - g_stats.k[a]; double dt = g_stats.t[b] - g_stats.t[a];
                for (uint64_t j = 0; j < dk && m < sizeof ft / sizeof *ft; j++) ft[m++] = dt / (double)dk;
            }
            atomic_store(&g_stats.avg, (int)lround((double)frames / span));
            if (m) {
                qsort(ft, m, sizeof *ft, cmp_desc);
                size_t worst = m / 100 ? m / 100 : 1; double sum = 0;
                for (size_t i = 0; i < worst; i++) sum += ft[i];
                atomic_store(&g_stats.low, sum > 0 ? (int)lround((double)worst / sum) : -1);
            }
        }
    }
    pthread_mutex_unlock(&g_stats.mu);
}
int fps_overlay_avg_value(void) { return atomic_load(&g_stats.avg); }
int fps_overlay_low_value(void) { return atomic_load(&g_stats.low); }

void fps_overlay_note(uint64_t ticket, double now_s) { rate_note(&g_game, ticket, now_s); stats_note(ticket, now_s); }
int fps_overlay_value(void) { return atomic_load(&g_game.value); }
void fps_overlay_note_gen(uint64_t n, double now_s) { rate_note(&g_gen, n, now_s); }
int fps_overlay_gen_value(void) { return atomic_load(&g_gen.value); }

