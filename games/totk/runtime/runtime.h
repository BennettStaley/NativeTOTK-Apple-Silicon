#pragma once
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
typedef struct { uint64_t where; unsigned kind, symbol; int64_t addend; } Reloc;
typedef struct { const char *name; uint64_t value, size; unsigned defined, binding, type; } Symbol;
typedef struct { uint64_t ret, original; unsigned timer, arg; } Gate;
typedef struct {
 const char *name, *digest; size_t file_size, size;
 uint64_t text_end, ro_off, data_off, patch_off, key_off, init, ctor_flag, init_array, init_size;
 const Symbol *symbols; unsigned nsym; const Reloc *relocs; unsigned nreloc;
 const Gate *gates; unsigned ngate; unsigned *hash, hash_mask;
 const uint32_t *changed, *moved; unsigned nchanged, nmoved;   /* main: [start,end) pairs the Apple-silicon rewrite changed / runs from its patch area (cheats) */
 uint8_t *base; unsigned relative, symbolic, cross, weak;
} Module;
extern pthread_key_t runtime_keys[8];
_Noreturn void runtime_fail(const char *message);
void *runtime_symbol(const char *name);
void *runtime_override(const char *name);
void runtime_hle_stack_init(uint64_t tsd_offset);
void runtime_gpu_path_probe(const char *which);
void runtime_audio_probe(void);
void runtime_audio_output_restart(int recreate);   /* audio_mixer.c: start the outputs again after an audio session interruption (iOS) */
void *runtime_hle_wrap(void *fn);
void runtime_hle_stack_set(uintptr_t top);
void *runtime_font_override(const char *name);
void *runtime_codec_override(const char *name);
void *runtime_movie_override(const char *name);   /* runtime/movie_host.c: movie::Decoder for VP9 cutscenes */
void *runtime_time_override(const char *name);
void *runtime_applet_override(const char *name);
void *runtime_swkbd_override(const char *name);   /* runtime/swkbd_host.c: nn::swkbd text entry with keyboard or controller */
void runtime_swkbd_probe(void);
void runtime_swkbd_selftest(void);
void runtime_dump_threads(void);   /* threads.c: every guest thread's wait state and guest return chain (TOTK_INPUT_FILE "threads") */   /* diagnostic "kbtest": show the text entry bar from a thread of its own */
void runtime_frame_step_pin(double ms);                  /* game_patches.c: pin the real-time frame step (diagnostics), 0 = real time */
void runtime_frame_step_update(uint64_t now_ns);
void runtime_thread_note_hle(unsigned index);            /* threads.c: the calling guest thread's last host call (hletrace) */
uint32_t runtime_thread_current_handle(void);            /* threads.c: handle of the calling guest thread, 0 on a host thread */
void *runtime_hle_wrap_named(void *fn, const char *name);/* hle_stack.c: runtime_hle_wrap with a name for diagnostics */
void runtime_hle_trace_set(int mode);                    /* hle_stack.c: 1 count host calls, 2 also log the first call of each, 0 stop and print */
void runtime_hle_describe(unsigned index, char *out, size_t cap);   /* hle_stack.c: readable name of a host-call wrapper */
/* Text entry in the game window (runtime/frontend.m, Mac): controller keyboard and Mac typing, Return confirms (1, the text
   as UTF-8 in out), Escape cancels (0). The caller waits; give the virtual core up around it (runtime_core_away). */
typedef struct { const char *prompt, *initial; int max_units, min_units, secure, digits_only; uint32_t disabled; } RuntimeTextRequest;
int runtime_front_text_input(const RuntimeTextRequest *req, char *out, size_t out_size);
void runtime_front_text_test(const char *op, const char *text);   /* diagnostics: "type" text, "enter", "esc" into the open bar */
int runtime_core_away(void);          /* runtime/threads.c: a guest thread gives its virtual core up around a long host wait */
void runtime_core_back(int *away);
void runtime_spin_wait(unsigned module, uint64_t head);   // threads.c: a pass of a gated spin wait (loader.c runtime_dispatch)
void runtime_save_probe(void);
int runtime_fs_save_busy(void);   /* runtime/fs_host.c: the game is writing save data (window close waits) */
uint8_t *runtime_module_base(const char *name);   /* runtime/loader.c: "main", "subsdk0", "sdk" (probes) */
uint64_t runtime_module_permission(uint64_t address,uint64_t size,unsigned permission);
uint64_t runtime_module_lock_permission(uint64_t address,uint64_t size,unsigned mask,unsigned attribute);
int runtime_module_query(uint64_t address,uint64_t *base,uint64_t *size,unsigned *state,unsigned *permission,unsigned *attribute);
void runtime_oe_push_message(uint32_t message);
void hle_bind_system_event(void *event, int autoclear);
uint64_t runtime_svc(unsigned num, uint64_t *regs, const char *module, uint64_t offset);
void runtime_gate(void);
void runtime_thread_init(void);
void runtime_dispatch(uint64_t ret, uint64_t *regs);

uint64_t runtime_image_bytes(void);

void runtime_fs_init(void);
void *runtime_fs_override(const char *name);
void runtime_fs_probe(void);

void *runtime_observer(const char *name);

void runtime_startup_checkpoint(uintptr_t caller);

uint64_t runtime_now_ns(void);
uint32_t svc_thread_main_init(void);
int svc_thread(int num,uint64_t *regs);

void *runtime_gfx_override(const char *name);
int runtime_gfx_fault(void *addr,int is_write,void *context);
/* A thread that reads guest memory it does not own (uniform snapshot copies) sets this around the read: a SIGBUS / SIGSEGV
   on that thread then returns 1 from sigsetjmp instead of taking the process down (loader.c signal handler). */
#include <setjmp.h>
extern __thread sigjmp_buf *runtime_fault_guard;
void dm_guest_write_prepare(void *addr,size_t n);

void runtime_trace_guest(uint64_t fp,uint64_t lr);

void runtime_front_run(void(*callback)(void));
void runtime_boot_progress(const char *stage,uint64_t done,uint64_t total);
void runtime_boot_status(const char *stage,const char *detail);
void runtime_boot_count(const char *stage,uint64_t done,uint64_t total);   /* a boot step counted in items (shader import) */
/* True 2K (runtime/game_patches.c, Mac): TOTK_OUTPUT_RES=<w>x<h> sets the game's own output size. runtime_output_res gives it
   (0 when unset); runtime_output_res_extra_bytes is what the HeapDef overlay (scripts/make_output_res_overlay.py, same formula)
   adds to the texture heap and to Graphics, and so to the application pool (runtime/services.c). */
int runtime_output_res(unsigned *w,unsigned *h);
uint64_t runtime_output_res_extra_bytes(void);
int runtime_heap_overlay(char *out,size_t n);   /* the HeapDef overlay folder in TOTK_ROMFS_PATCHES_OUT the heaps need ("<w>x<h>[-scene-<sw>x<sh>]"), 0 = none */
/* Live scene (3D) render size (1.4.3, runtime/game_patches_v143.c; Mac and iOS). runtime_render_request asks for w x h,
   latched at the next present: 0, or -1 when it is above the boot reserve (applies after a restart: TOTK_RENDER_RES) or
   the scene size is not patched. runtime_render_reserve gives the biggest size the heaps sized at boot hold (a request
   fits when w*h is at most its pixels); TOTK_RENDER_RESERVE=<w>x<h> sets it. Older versions refuse and report 0x0. */
int runtime_render_request(unsigned w,unsigned h);
void runtime_render_reserve(unsigned *w,unsigned *h);
/* The game's output (its 16:9 canvas, text and icons included), live up to the swapchain sized at start (1.4.3, Mac):
   runtime_output_request asks for w x h (0, or -1 when outside 640x360 .. runtime_output_max). */
int runtime_output_request(unsigned w,unsigned h);
void runtime_output_max(unsigned *w,unsigned *h);
void runtime_aspect_set(float ratio);   /* aspect_patch.h: the picture's shape, 0 = the game's own 16:9 (Mac) */
int runtime_render_res(unsigned *w, unsigned *h);   /* the scene reserve above 1600x900 that the heaps count (1.4.3; 0 elsewhere) */
int runtime_output_res_toggle(void);                    /* legacy two-state output size switch */
int runtime_output_mode_cycle(void);                    /* key 3: MetalFX 2K -> native 2K -> off */
/* Upstream's resolution setters (its key-1 panel; ours is settings_panel_mac.h): 1080p, MetalFX scaling 1080p up to the window, or the game's own
   canvas at its native size (2K; runtime_output_native gives it), the same with MetalFX scaling it up to the window
   (user 2026-10-25: "4K" as 2K + MetalFX, a 4K swapchain's heaps being too big for one GPU buffer); also 720p, a
   1280x720 canvas whose scene follows it (user 2026-10-25, in place of the Switch's own handheld / docked sizes: the game
   reads its docked scene size only at the start, so a switch to it never took effect).
   runtime_output_modes: a bit (1 << mode) for each this game and start offer; 0 when the resolution cannot change. */
enum { OUTPUT_MODE_OFF, OUTPUT_MODE_METALFX, OUTPUT_MODE_NATIVE, OUTPUT_MODE_NATIVE_METALFX, OUTPUT_MODE_720P };
int runtime_output_modes(void);
int runtime_output_mode(void);
int runtime_output_mode_set(int mode);                  /* returns 0 where that mode is not offered */
int runtime_output_native(int mode, unsigned *w, unsigned *h);   /* the canvas of OUTPUT_MODE_NATIVE / _NATIVE_METALFX; 0 when not offered */
uint64_t runtime_output_size_current(void);              /* configured canvas: width in low 32 bits, height in high 32 bits */
void runtime_warm_progress(uint64_t done,uint64_t total,uint64_t shaders,uint64_t shader_total,uint64_t pipelines,uint64_t pipeline_total);
void runtime_warm_done(void);
void runtime_game_slot_acquire(const char *mode);   /* runtime/game_slot.c: one game at a time */
void *runtime_front_override(const char *name);
int runtime_front_focus(void);
void runtime_settings_toggle(void);   /* key 1 (runtime/input.c): the settings panel opens or closes (frontend.m; nothing on the phone) */
int fps_overlay_toggle_hints(void);   /* key 1: all -> minimal (FPS only) -> off; returns the new mode, 0 = off */
void fps_overlay_set_hud_mode(int mode);   /* gfx/nvn/fps_overlay.h HUD_MODE_*: the panel's overlay choice */
int fps_overlay_hud_mode(void);
int fps_overlay_full_hud(void);
int runtime_aspect_set_ratio(float r);   /* the settings panel (aspect_patch.h; game_patches_botw.c): 16:9, 16:10, 21:9, 32:9 ...; 0 where unavailable */
float runtime_aspect_ratio(void);        /* the ratio picked (16:9 = the game's own); 0 where the shape cannot be changed */
float runtime_aspect_start(void);        /* TOTK_ASPECT's ratio, 0 without it (the panel offers it too) */
int runtime_aspect_toggle(void);   /* runtime/aspect_patch.h: key 4, 16:9 -> fit to the window / display -> 21:9 (or TOTK_ASPECT) (Mac, 1.2.1 / 1.4.3); 0 where unavailable */
int runtime_aspect_step(void);     /* key 4's step: 0 16:9, 1 fit to the window, 2 21:9 / TOTK_ASPECT; -1 where unavailable */
void runtime_aspect_view(double w, double h);   /* frontend.m: the window's drawable size, which key 4 follows */
float runtime_aspect_now(void);     /* the ratio in force, 0 = the game's own 16:9 */
float runtime_aspect_choice(void);  /* key 4's step as the settings menu gives it: -1 fit, 0 16:9, else the wide ratio */
void runtime_fov_set(int percent);
void runtime_fog_set(int percent);   /* aspect_patch.h: fog density 0..100 % live (1.4.3, Mac and iOS), 100 = the game's own */
int runtime_shadow_res(void);        /* aspect_patch.h: the shadow map factor in force from the start (TOTK_SHADOW_RES): 0 = the game's own, 2 / 4, -2 / -4 = 0.5x / 0.25x */
void runtime_fov_aspect(float k);   /* aspect_patch.h: the vertical angle's factor for a screen taller than 16:9 (Vert+), with the player's */  /* runtime/aspect_patch.h: field of view 70..140 % live (1.4.3, Mac and iOS), 100 = the game's own */
void runtime_time_speed_set(int percent);       /* runtime/world_patch.h: time of day 0 (paused) .. 800 % live (1.4.3), 100 = the game's own */
void runtime_render_distance_set(int percent);   /* runtime/world_patch.h: the camera's far clip 50..300 % live (1.4.3), 100 = the game's own */
void runtime_front_resize_aspect(double ratio);   /* Mac: resize the window to a selected picture ratio */
int hud_compact(void);   /* gfx/nvn/fps_overlay.c: the overlay lines go to the window's corner panel (TOTK_HUD=corners: in the picture) */
enum { HUD_LINE_COUNT = 5 };   /* fps_overlay.h HUD_LINES */
int hud_snapshot(int line, char *out, size_t n, int *highlight);   /* line 0..4: FPS, shaders, render pipelines, load, power; 0 = not published */

void *runtime_account_override(const char *name);

void runtime_gfx_probe(int negative);
void runtime_vpool_probe(void);

void runtime_memory_probe(void);

void runtime_input_key(unsigned code,int down);
void runtime_input_clear(void);
void runtime_input_hold(int on);
int runtime_setting_test(const char *key, const char *word, int *value);   /* settings.c: TOTK_INPUT_FILE "set <key> <value>", the menu's path */
void runtime_front_settings_key(unsigned code);   /* frontend.m: a key pressed in the open settings menu (TOTK_INPUT_FILE "menukey") */
void runtime_front_restart(void);       /* frontend.m: close the game and open the app again (the menu's Restart now) */
int runtime_front_can_restart(void);   /* 1 when launched from the app (TOTK_APP_BUNDLE) */
void runtime_front_settings_toggle(void);   /* frontend.m: open or close the Mac settings menu (gamepad.m: Home, or Options + Menu) */
void runtime_front_settings_shot(const char *path);   /* frontend.m: the Mac settings panel as a PNG (TOTK_INPUT_FILE "settings-shot") */   /* 1 while the settings menu is open: the game reads only the controller sticks */
uint64_t runtime_input_buttons(void);   /* runtime/input.c: held buttons (keyboard and controller), npad bit order */
void runtime_guest_regions(uint64_t base[3],uint64_t size[3]);   /* runtime/services.c: heap, alias and ASLR regions */
int runtime_guest_access(uint64_t a,uint64_t n,int (*access)(void *ctx),void *ctx);   /* runtime/services.c: access() only on guest-mapped heap / alias pages */
void runtime_cheats_launch(Module *main_module);   /* runtime/cheats.c: before the modules are sealed */
int runtime_module_image_word(const Module *m,uint64_t off,uint32_t *out);   /* runtime/loader.c: a word of the verified rewritten image file (Mac) */
int runtime_cheats_count(void);
const char *runtime_cheats_name(int i);
int runtime_cheats_state(int i);   /* CHEAT_ON | CHEAT_AT_LAUNCH | CHEAT_APPLIED | CHEAT_REFUSED */
const char *runtime_cheats_problem(int i);
int runtime_cheats_set(int i,int on);   /* returns the new state; saved, and the profile backed up before the first cheat */
int runtime_cheats_live_code(void);   /* code patches are written and put back while the game runs (else at the next launch) */
enum { CHEAT_ON = 1, CHEAT_AT_LAUNCH = 2, CHEAT_APPLIED = 4, CHEAT_REFUSED = 8 };
/* The Cheats tab (Mac): every section of every cheat file read, k in file order; role (cheat_files.h): 0 cheat, 1 master
   code (always on), 2 heading (a group's or a file's title), 3 hidden (a group's end); source: runtime_cheats_source_*. */
typedef struct {
    const char *name; int source, role, state; char problem[112];
    /* The community cheat list (cheat_catalog.h); outside it: label = name, about = option = NULL, group = -1, hide = 0, choice = -1. */
    const char *label;    /* the name shown */
    const char *about;    /* a line about it, or NULL */
    const char *option;   /* in a choice: this variant's label */
    int group, hide;      /* CHEAT_GROUP_*; CHEAT_HIDE_* (not listed) or 0 */
    int choice;           /* -1, or the first section of its choice (the row its options belong to) */
} RuntimeCheatInfo;
int runtime_cheats_total(void);
int runtime_cheats_info(int k, RuntimeCheatInfo *out);   /* 0: no such section */
int runtime_cheats_set_entry(int k, int on);   /* a cheat (role 0) on or off, as runtime_cheats_set; returns the new state */
int runtime_cheats_sources(void);
const char *runtime_cheats_source_label(int s);
const char *runtime_cheats_source_path(int s);
const char *runtime_cheats_source_problem(int s);   /* "" or why the file (or part of it) was not read */
const char *runtime_cheats_build_id(void);   /* the running main's build ID, "" when unknown */
const char *runtime_cheats_folder(void);     /* the player's cheats folder, "0" for none */
int runtime_cheats_files_changed(void);      /* 1: the cheat files differ from those read at launch (restart to read them); 2: only the cheat list */
const char *runtime_cheats_db_dir(void);     /* where the community cheat list is kept (Mac: cheat_db_fetch.h downloads it), "" for none */
int runtime_cheats_db_source(void);          /* the cheat list's source index, -1 when none was read */
int runtime_cheats_db_arrived(void);         /* main thread, after a download wrote a new list: 1 when it was read now (none was at launch) */
int runtime_handheld_mode(void);   /* TOTK_OPERATION_MODE=handheld (runtime/services.c) */
void *runtime_input_override(const char *name);
typedef struct { uint64_t buttons; int32_t lx,ly,rx,ry; int connected; } RuntimeGamepadState;
void runtime_gamepad_start(void);
void runtime_gamepad_switch_left(void *controller, int x, int y);
void runtime_gamepad_switch_right(void *controller, int x, int y);
void runtime_gamepad_read(RuntimeGamepadState *out);
void runtime_gamepad_left_stick(int32_t *x,int32_t *y);   /* the stick without taking the press edges */
void runtime_input_left_stick(int32_t *x,int32_t *y);   /* runtime/input.c: keyboard WASD or the controller, the stronger, -32767..32767, y up */
uint64_t runtime_gamepad_held(void);   /* held buttons, leaving the press edges for runtime_gamepad_read */
/* Mac software keyboard: exclusive controller capture; UI reads physical positions (controller_map.h),
   independent of gameplay remaps. Main-thread only. Closing waits for release before gameplay resumes. */
void runtime_gamepad_text_capture(int on);
void runtime_gamepad_text_read(RuntimeGamepadState *out);
/* macOS opts in; the physical L2 + R2 + left-stick click chord, held for a second, is latched independently of guest polling. */
void runtime_gamepad_settings_enable(int enabled);
int runtime_gamepad_settings_requested(void);
/* Forget held buttons and sticks: macOS stops delivering controller events (including releases) while the app is in
   the background, so a button held at focus loss would otherwise stay pressed for the game. */
void runtime_gamepad_clear(void);
int runtime_gamepad_type_set(int type);     /* runtime/gamepad.m: the settings panel's controller type (gamepad_layout.h GAMEPAD_TYPE_*), saved; returns the type in force */
int runtime_gamepad_type(int *detected);    /* runtime/gamepad.m, main thread: the picked type; *detected = auto's guess for the connected controller, -1 none */
int runtime_setting_word(const char *name, char *word, size_t size);   /* runtime/gamepad.m: a one-word setting file next to controls.txt; 1 read, 0 none */
void runtime_setting_save(const char *name, const char *word, const char *what);   /* runtime/gamepad.m: saves one; what names it in a failure line */
int runtime_mouse_set(int on);              /* runtime/gamepad.m: the settings panel's mouse mode, saved; returns it */
int runtime_mouse_enabled(void);
void runtime_mouse_set_hook(void (*hook)(void));   /* the window (frontend.m): mouse mode changed, on the main queue */
void runtime_mouse_move(double dx, double dy);      /* points, AppKit's deltaX / deltaY (down positive) */
void runtime_mouse_button(int button, int down);    /* main thread; 0 left, 1 right, 2 middle, 3 and 4 the side buttons */
void runtime_mouse_release(void);                   /* main thread: buttons up, stick centred */
void runtime_touch_pad_set(const RuntimeGamepadState *state);   /* on-screen touch controls (iOS) */
int runtime_phone_fps(void);                /* d2r_gfx hle_nvn.c, iPhone / iPad: 30 while held at 30 (TOTK_PHONE_FPS), else 60 */
void runtime_phone_fps_set(int fps);        /* 30 holds 30, anything else up to 60: the touch controls' button, at once */
/* Controller rumble and motion (opt-in TOTK_RUMBLE=1 / TOTK_GYRO=1): the game's hid side in runtime/hid_feedback.c, the
   Mac controller's haptics and motion sensors in runtime/gamepad_feedback.m. */
#define RUNTIME_MOTION_MAX 32
typedef struct { uint64_t sampling, delta_ns; float acc[3], gyro[3], angle[3], dir[9]; } RuntimeMotionState;   /* G, rev/s, rev, 3 axes */
int runtime_rumble_enabled(void);
int runtime_gyro_enabled(void);
void *runtime_hid_feedback_override(const char *name);
int runtime_hid_feedback_flag(const char *name, int v);   /* "flag rumble|gyro 0|1"; 0 = not one of these names */
/* The settings panel's rumble (gyro 0) and gyro aiming (gyro 1) switches (runtime/hid_feedback.c): locked when TOTK_RUMBLE /
   TOTK_GYRO or an agent's test session decides; present when the game has the device in this run; the choice in force, and
   choosing saves rumble.txt / gyro.txt and mutes or unmutes a present device at once. Its strength
   (runtime/gamepad_feedback.m): locked when TOTK_RUMBLE_GAIN is set; set saves rumble-gain.txt and applies at once. */
int runtime_feedback_locked(int gyro);
int runtime_feedback_present(int gyro);
int runtime_feedback_choice(int gyro);
void runtime_feedback_choose(int gyro, int on);
int runtime_rumble_gain_locked(void);
float runtime_rumble_gain(void);
void runtime_rumble_gain_set(float gain);
void runtime_hid_feedback_stats(uint64_t *values, uint64_t *motion_reads);
void runtime_rumble_set(unsigned side, float amp_low, float freq_low, float amp_high, float freq_high);   /* side 0 left, 1 right */
/* The settings panel's DualSense bow trigger switch (runtime/gamepad_feedback.m, main queue): available for Breath of the Wild
   1.9.0 and Tears of the Kingdom 1.4.3; locked when TOTK_DUALSENSE_TRIGGERS or an agent's test session decides; the choice
   in force; choosing saves dualsense-triggers.txt and turns the connected DualSense's triggers on or off at once. */
int runtime_triggers_available(void);
int runtime_triggers_locked(void);
int runtime_triggers_choice(void);
void runtime_triggers_choose(int on);
int runtime_motion_read(RuntimeMotionState *out, int max);   /* newest first; 0 = no motion now */
void runtime_motion_start(void);   /* the game (re)started its sensor: angle and direction start over */
void runtime_motion_feed(const double acc_g[3], const double gyro_rad_s[3], uint64_t now_ns);   /* runtime/motion_fusion.c */
int runtime_motion_axes(const char *spec);   /* TOTK_GYRO_AXES ("-x,y,-z"): 1 set, 0 unset, -1 malformed */
void runtime_motion_deadzone_default(double deg_s);   /* when TOTK_GYRO_DEADZONE is unset: 1.2 for HID motion, else 0 */
void runtime_motion_live(int on);   /* a controller with motion attached (1) / detached (0) */
void runtime_motion_phone(int on);  /* the iPhone's own sensor feeds the six-axis device (1) / stops (0) */
void runtime_motion_mouse(int on);  /* mouse mode (key 7) takes the sensor over (1) / gives it back to the controller (0) */
void runtime_motion_feed_mouse(const double acc_g[3], const double gyro_rad_s[3], uint64_t now_ns);   /* already in the reference frame */
void runtime_feedback_quiet(void);   /* focus lost: stop rumbling */
#ifdef __OBJC__
@class GCController;
void runtime_feedback_attach(GCController *controller);
void runtime_feedback_detach(GCController *controller);
#endif
