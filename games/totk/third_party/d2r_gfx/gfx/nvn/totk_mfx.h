// TOTK: MetalFX frame interpolation (not in D2R). See totk_mfx.m.
#pragma once
#import <Metal/Metal.h>

/* Frame interpolation modes (the settings panel; user 2026-10-25: 30+30, 40+40, 50+50, uncapped):
   MFX_30 / 40 / 50 / 60  the game renders 30 / 40 / 50 / 60 frames a second, MetalFX makes the frame between ("30+30" ...),
            each shown half a game period before the game's own (by display ticks, or our own clock where the display's
            rate is not a multiple: 40 and 30 at 100 Hz);
   MFX_MAX  the game is not paced (uncapped) and every frame gets one in between; the screen still shows one picture per
            refresh, so above half the refresh rate some game frames are replaced by interpolated ones.
   TOTK_MFX=interp30 | interp40 | interp50 | interpmax starts in that mode (interp: 50+50, half of a 100 Hz display, the
   old "half the refresh rate"); flag mfx_interp <0..4> sets it, -1 cycles. A change takes effect at the next game frame
   boundary (mfx_latch at the present), so a frame is never half on. */
#include "totk_mfx_mode.h"
const char *mfx_mode_name(int mode);
void mfx_set(int mode);
void mfx_set_mv_dyn(int on);   // flag mfx_mv_dyn (default on): pixels that are not still world get no camera motion  // mode < 0 cycles the requested mode
int mfx_latch(void);     // at the game's present, after this frame's interpolation: take the requested mode; 1 = it changed
// Per game frame, from the replay: the scene copy (the canvas copied before the first UI draw, in the canvas format),
// the UI layer (colour attachment 7 of the later canvas passes), the G-buffer depth and the camera block (84 floats of
// the 2368-byte block at vertex uniform 1).
id<MTLTexture> mfx_scene_target(id<MTLDevice> dev, NSUInteger w, NSUInteger h, MTLPixelFormat format);
id<MTLTexture> mfx_ui_target(id<MTLDevice> dev, NSUInteger w, NSUInteger h);
/* The UI-layer composite (totk_ui_layer.h): a second UI texture of the same size that the composite writes the squeezed UI
   into, and the swap that makes it the UI the interpolator takes this frame (the other one is drawn into next frame). */
id<MTLTexture> mfx_ui_alt(void);
void mfx_ui_swap(void);
void mfx_note_scene(void);
void mfx_note_depth(id<MTLTexture> depth);
void mfx_note_camera(const float *cam, int n);
id<MTLResidencySet> mfx_residency(void);   // our textures; the queue adds it once
// TOTK_OBJ_MOTION=1 (default 0): per-object motion from the opaque G-buffer draws (totk_objmv.h). The target is RGBA16Float
// of the depth size, holding the NDC delta previous - current per pixel (RG; OBJMV_NONE where none is known, camera motion
// is used there) and the view distance of the fragment that wrote it (B); mfx_note_obj: this frame's G-buffer pass wrote it.
#define OBJMV_NONE 65000.0f
int mfx_obj_motion_on(void);
id<MTLTexture> mfx_obj_target(id<MTLDevice> dev, NSUInteger w, NSUInteger h);
void mfx_note_obj(void);
// At the game's present. prepare: checks this frame's inputs and returns the output slot of the interpolated frame
// between the previous and this frame, or -1 (first frame, missing input, scene cut; state already advanced). frame_s:
// the pacing period (two display refreshes; 0 in unpaced measurement runs). encode: motion vectors and the interpolation into cb (Metal 4, own command buffer, GPU order after this
// frame). abort: a prepared frame that cannot be encoded (no free command buffer); drops the history.
int mfx_frame_prepare(double frame_s);
// Keep the returned resources alive until this command buffer's GPU completion.
// The array holds exactly MFX_FRAME_RESOURCES entries: the interpolator, its colour / previous colour, depth, motion,
// UI, output, the motion kernel's parameters, the previous depth (still-world test) and texture 2 of the motion kernel
// (the per-object motion target with TOTK_OBJ_MOTION, else the motion texture again).
#define MFX_FRAME_RESOURCES 10
// Cached interpolators drop their public texture bindings after encoding.
NSArray *mfx_frame_encode(id<MTL4CommandBuffer> cb, int slot, id<MTL4CounterHeap> heap, NSUInteger stamp);   // heap: timestamps at stamp / stamp+1 around MetalFX (may be nil)
void mfx_note_gpu(double ms);   // GPU time of one interpolation, for the statistics line
void mfx_frame_abort(void);
/* TOTK_MFX_DEBUG_VIEW (setting graphics.mfx_debug_view, flag mfx_debug_view): 0 off, 1 motion, 2 depth, 3 split. debug_encode,
   right after mfx_frame_encode into the same command buffer: draws the view over the interpolated output of slot and over
   present (the game's frame; crop: x, y, w, h of it that is shown, or NULL / zero size for all of it); returns the
   resources to keep alive with the frame's, or nil when off. */
int mfx_debug_view(void);
void mfx_debug_view_set(int view);   // < 0 cycles
NSArray *mfx_debug_encode(id<MTL4CommandBuffer> cb, int slot, id<MTLTexture> present, const int crop[4]);
/* The view with frame interpolation off (no interpolator or output textures). debug_only: nonzero (the view) while the
   view is on and interpolation is off: the replay then takes the G-buffer depth, the camera and (TOTK_OBJ_MOTION) the
   per-object motion; 0 also while held off near the GPU memory budget. At the game's present: debug_prepare returns the
   slot or -1 (first frame, cut, missing input); debug_frame_encode (own command buffer, after the game's frame) runs the
   motion kernel and draws the view over present, returning the resources to keep alive until completion (nil: nothing
   encoded); debug_abort: a prepared frame not encoded. debug_release (every present while debug_only is 0 and
   interpolation off): retires its textures; 1 = it held some (the caller then releases the position arenas). */
int mfx_debug_only(void);
int mfx_debug_prepare(void);
NSArray *mfx_debug_frame_encode(id<MTL4CommandBuffer> cb, int slot, id<MTLTexture> present, const int crop[4]);
void mfx_debug_abort(void);
int mfx_debug_release(void);
id<MTLTexture> mfx_output(int slot);   // RGBA8Unorm view of the sRGB output, like the swapchain textures
void mfx_stats(char *out, size_t n);
void mfx_mem_report(char *out, size_t n);   // memreport (diagnostic): textures, cached / retired interpolators
// At every game present (also while off): releases retired textures / interpolators (an old size or configuration) and
// pauses interpolation and per-object motion above 95% of the GPU memory budget (resumed under 85%). used / budget:
// the device's allocated bytes and gpu_budget_bytes (0: no budget).
void mfx_present_tick(uint64_t used, uint64_t budget);
// gpu_budget_check over the budget: 1 while interpolation is being paused / released (the overrun is tolerated briefly).
int mfx_budget_grace(uint64_t used, uint64_t budget);
void mfx_mv_log(void);
int mfx_dump_due(void);
void mfx_dump(long frame, id<MTLTexture> swap_prev, id<MTLTexture> swap_cur);   // TOTK_MFX_DUMP=<dir>: scenes, depth, motion and output of three frames while the camera turns (diagnostic)   // TOTK_MFX_MVLOG=<n>: motion texture median every n interpolated frames (diagnostic)
