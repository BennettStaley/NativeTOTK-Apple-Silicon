#pragma once
#include <stddef.h>
#include <stdint.h>

/* TOTK: frames per second, the game's output resolution and, in brackets, its internal (3D scene) resolution in its
   top-right corner, one line ("FPS 60  1920x1080 (1600x900)"; the top-left holds the hearts). The internal resolution was
   left out at first (user, 2026-10-16) and asked for on 2026-10-23. The game image itself (frame screenshots, texture dumps) never contains it. TOTK_FPS_OVERLAY=0 hides it. */
enum { FPS_OVERLAY_TEX_W = 768, FPS_OVERLAY_TEX_H = 64, FPS_OVERLAY_MAX_SCALE = 5 };

/* fps / screen size < 0 / 0: not known yet ("--"). mfx: MetalFX upscaled the picture in the last frames. gen > 0: MetalFX
   frame interpolation shows that many generated frames a second besides the game's fps frames -> "FPS 50+50" (key 2).
   big: the game itself outputs more than 1920x1080 (true 2K) -> " 2K" at the end.
   detail: "Readout detail" is Detailed -> "FPS 60  avg 59  1% low 48  2560x1440" (avg / low < 0: "--", not measured yet).
   scene_w / scene_h: the internal (scene) resolution, shown as " (WxH)" after the output size when known. */
typedef struct { int fps, screen_w, screen_h, mfx, gen, big, detail, avg, low, scene_w, scene_h; } fps_overlay_info;   // screen: the output; scene: the internal size, 0 = not known
enum { FPS_STATS_SECONDS = 5 };   /* the window the average and the 1% low are measured over */

int fps_overlay_enabled(void);
/* Shared user toggle for every in-picture HUD layer: FPS, shader collection and load telemetry. */
/* The settings panel (key 1): HUD_MODE_ALL (FPS, shader collection, load), HUD_MODE_MINIMAL (FPS only) or HUD_MODE_OFF. */
enum { HUD_MODE_OFF = 0, HUD_MODE_ALL = 1, HUD_MODE_MINIMAL = 2 };
int fps_overlay_hud_mode(void);
int fps_overlay_hints_enabled(void);   /* the FPS readout shows (all or minimal) */
int fps_overlay_full_hud(void);        /* everything shows (all) */
int fps_overlay_toggle_hints(void);   /* atomically steps to the next mode and returns it (HUD_MODE_OFF = nothing shows) */
void fps_overlay_set_hints(int visible);   /* the settings menu: all (1) or off (0); TOTK_HINTS=0 starts off */
void fps_overlay_set_hud_mode(int mode);   /* HUD_MODE_*; other values are ignored */
/* Pixel scale (1..FPS_OVERLAY_MAX_SCALE) for a game picture view_h pixels tall: one text line is about 3% of its height. */
int fps_overlay_scale(double view_h);
/* The line in a 5x7 pixel font on a dark box, as BGRA8 pixels in out (FPS_OVERLAY_TEX_W x FPS_OVERLAY_TEX_H,
   row stride FPS_OVERLAY_TEX_W * 4). Returns the box size in pixels. */
void fps_overlay_raster(const fps_overlay_info *info, int scale, uint8_t *out, int *box_w, int *box_h);
void fps_overlay_text(const fps_overlay_info *info, char *out, size_t n);   /* "FPS 97  2560x1440  MetalFX on" */
/* Compact HUD (Mac default; TOTK_HUD=corners keeps the lines in the picture's corners): the FPS, shader, render pipeline
   (compact only) and load lines are
   not drawn into the picture but published here, and the window's corner panel (runtime/frontend.m) shows them with the
   shortcuts in one small box. */
enum { HUD_FPS, HUD_SHADER, HUD_PSO, HUD_LOAD, HUD_POWER, HUD_LINES };   // HUD_POWER: compact panel only (totk_sysload.h); runtime.h HUD_LINE_COUNT matches HUD_LINES
int hud_compact(void);
void hud_publish(int line, const char *text, int highlight);
int hud_snapshot(int line, char *out, size_t n, int *highlight);   /* 0: the line was never published (hidden or not known yet) */
/* Any one line (at most 80 characters of the 5x7 font) the same way; highlight draws it yellow instead of green. The _tex
   form rasterises into a texture of tex_w x tex_h pixels (row stride tex_w * 4); the scale drops until the line fits. */
void fps_overlay_raster_line(const char *text, int highlight, int scale, uint8_t *out, int *box_w, int *box_h);
void fps_overlay_raster_line_tex(const char *text, int highlight, int scale, uint8_t *out, int tex_w, int tex_h, int *box_w, int *box_h);
/* The bottom-right load line (totk_sysload.c) is wider than the other two: its texture. */
enum { LOAD_OVERLAY_TEX_W = 2048 };   // the widest line (every value 100%) at scale 4 (1440p)
int fps_overlay_has_glyph(char c);
int fps_overlay_line_pixel_font(const char *text);   // 0: the line has a character the pixel font lacks (drawn with CoreText)
/* TOTK automatic shader collection, bottom-left box (2026-10-16 night): "SHADER +<new>/<in the cache> PSO +<new pipelines>",
   yellow for 3 s after something new was collected. TOTK_SHADER_OVERLAY=0 hides it (default on on the Mac, off on iOS). */
int shader_overlay_enabled(void);
void shader_overlay_set(int on);   /* the settings menu's Shader counter, live */
void shader_overlay_text(char *out, size_t n, uint64_t compiling, uint64_t fresh, int64_t total, uint64_t pipelines);   /* total < 0: "--" (not counted yet) */
void pso_overlay_text(char *out, size_t n, uint64_t fresh, int64_t total);   /* "New render pipelines N / total M", the line under the shaders */
/* Record that the game's present number `ticket` reached the screen at now_s (seconds). The rate of game presents is
   re-measured every half second; fps_overlay_value() returns it, or -1 before the first measurement. */
void fps_overlay_note(uint64_t ticket, double now_s);
int fps_overlay_value(void);
/* Over the last FPS_STATS_SECONDS of game presents: the average rate and the rate of the slowest 1% of frames (-1: less
   than a second measured yet). The settings menu's "Readout detail" (fps_overlay_set_detail) shows them. */
int fps_overlay_avg_value(void);
int fps_overlay_low_value(void);
void fps_overlay_set_detail(int detailed);
int fps_overlay_detail(void);
/* The same for interpolated frames shown (n counts them); fps_overlay_gen_value() is that rate, or -1. */
void fps_overlay_note_gen(uint64_t n, double now_s);
int fps_overlay_gen_value(void);

_Static_assert(HUD_LINES == 5, "runtime/runtime.h HUD_LINE_COUNT");
