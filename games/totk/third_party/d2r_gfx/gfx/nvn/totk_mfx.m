// TOTK: MetalFX frame interpolation (TOTK_MFX=interp30 / interp / interpmax, or key 2 / flag mfx_interp at run time; not in
// D2R). MetalFX makes one more frame between two game frames. Modes (totk_mfx.h): the game at 30 fps ("30+30"), at every
// second display refresh ("50+50" on the user's 100 Hz display: every refresh a new picture, steadier than the uncapped
// 65-97 fps, about one refresh more latency), or uncapped with every frame interpolated. First planned for the phone
// (60 fps on a GPU about a third of the M4's).
// The replay (nvn_mtl.m) hands over per game frame:
//  - the scene before the UI: the canvas copied before the first UI draw (docked the upscaled 1600x900 scene, handheld the
//    1280x720 scene drawn in the canvas itself);
//  - the UI drawn over it: attachment 7 of the later canvas passes, premultiplied (the game's colour blend, alpha
//    accumulating coverage);
//  - the G-buffer depth (D32F, not reversed, infinite far plane) and the camera block.
// Motion vectors come from the camera: depth plus this and the previous frame's view-projection. A pixel whose depth does
// not match the previous frame's there (the previous depth around the reprojected position) is not still world: it gets
// no motion on screen (mfx_mv_dyn, 2026-10-23). Before, what moves with the camera (Link) got the background's camera
// motion and was put off his place in every interpolated frame, by the camera's step between two game frames: at 25+25
// or 30+30 twice the step of 50+50 and 25-30 times a second, so he shook while the world stayed right (Breath of the Wild
// 1.6.0). Zero motion everywhere shook the whole picture: MetalFX relies on the motion it is given.
// TOTK_OBJ_MOTION=1 (default 0) adds per-object motion (totk_objmv.h, like RTX Remix's draw call tracker): the opaque
// G-buffer draws also write their clip-space motion into an RGBA16Float attachment (NDC delta previous - current;
// OBJMV_NONE where no draw of the previous frame matched; B the fragment's view distance), and the kernel takes it where
// present and drawn by the surface the final depth shows (before the depth test above), the camera motion elsewhere. Conventions measured by the probe: motion in pixels of the motion texture, pointing to the
// previous position; the UI given apart and premultiplied (the "composited" mode returns a black frame).
#import <MetalFX/MetalFX.h>
#import <QuartzCore/QuartzCore.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "totk_mfx.h"
#include <TargetConditionals.h>

#define MLOG(...) do { fprintf(stderr, "[dm] [nvn-mtl] [mfx] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define MDIE(...) do { MLOG("FATAL " __VA_ARGS__); abort(); } while (0)

#if TARGET_OS_VISION
/* visionOS has no MetalFX frame interpolator. Keep every entry point off, including environment initialization. */
int mfx_enabled(void) { return 0; }
const char *mfx_mode_name(int mode) { return mode == MFX_OFF ? "off" : "not available on visionOS"; }
void mfx_set(int mode) { if (mode) MLOG("frame interpolation is not available on visionOS"); }
void mfx_set_mv_dyn(int on) { (void)on; }
int mfx_latch(void) { return 0; }
id<MTLTexture> mfx_scene_target(id<MTLDevice> dev, NSUInteger w, NSUInteger h, MTLPixelFormat format) { (void)dev; (void)w; (void)h; (void)format; return nil; }
id<MTLTexture> mfx_ui_target(id<MTLDevice> dev, NSUInteger w, NSUInteger h) { (void)dev; (void)w; (void)h; return nil; }
id<MTLTexture> mfx_ui_alt(void) { return nil; }
void mfx_ui_swap(void) {}
void mfx_note_scene(void) {}
void mfx_note_depth(id<MTLTexture> depth) { (void)depth; }
void mfx_note_camera(const float *cam, int n) { (void)cam; (void)n; }
id<MTLResidencySet> mfx_residency(void) { return nil; }
int mfx_frame_prepare(double frame_s) { (void)frame_s; return -1; }
NSArray *mfx_frame_encode(id<MTL4CommandBuffer> cb, int slot, id<MTL4CounterHeap> heap, NSUInteger stamp) { (void)cb; (void)slot; (void)heap; (void)stamp; return nil; }
void mfx_note_gpu(double ms) { (void)ms; }
void mfx_frame_abort(void) {}
id<MTLTexture> mfx_output(int slot) { (void)slot; return nil; }
void mfx_stats(char *out, size_t n) { snprintf(out, n, "not available on visionOS"); }
void mfx_mv_log(void) {}
int mfx_dump_due(void) { return 0; }
void mfx_dump(long frame, id<MTLTexture> prev, id<MTLTexture> cur) { (void)frame; (void)prev; (void)cur; }
#else

#define MFX_OUT 3
static struct {
    id<MTLDevice> dev;
    id<MTL4FXFrameInterpolator> fi;
    NSUInteger in_w, in_h, out_w, out_h; MTLPixelFormat scene_fmt;
    id<MTLTexture> scene[2], ui, ui2, mv, out[MFX_OUT], outv[MFX_OUT];   // ui2: the UI-layer composite's squeezed UI (mfx_ui_swap)
    id<MTLTexture> pdepth; uint64_t pdepth_frame;   // the previous interpolated frame's depth (copied after its motion), and its g.frames
    id<MTLResidencySet> rs;
    id<MTLComputePipelineState> mv_ps;
    id<MTL4ArgumentTable> mv_args[MFX_OUT];
    id<MTLBuffer> mv_par[MFX_OUT];
    id<MTLFence> fence;
    int cur;                       // scene[cur] is written by this frame
    int scene_ok, cam_ok;          // this frame
    __weak id<MTLTexture> depth;   // this frame's G-buffer depth (owned by the game's texture table)
    double cam[84], prev_cam[84];
    int prev_ok, hist_ok, slot;    // prev_ok: scene[cur ^ 1] / prev_cam hold the previous frame; hist_ok: MetalFX saw it
    double yflip;                  // clip-space y of the game: +1 = top row (TOTK_MFX_YFLIP=-1 tries the other way)
    id<MTLTexture> enc_depth;      // depth of a prepared frame, until it is encoded
    int scene_ok_was, cam_ok_was;
    double mv_prev_yaw;
    id<MTLTexture> last_depth;     // for mfx_dump
    int last_slot;
    uint64_t frames, made, skip_input, skip_first, skip_cut, skip_busy, miss[4];   // miss: scene, camera, depth, UI
    double gpu_ms[300]; int ngpu;   // GPU time of the last interpolations (MetalFX part)
    double last_prepare_s, dt;      // time of the previous game frame's prepare; this pair's interval (MetalFX deltaTime)
    id<MTLTexture> obj;             // TOTK_OBJ_MOTION: per-object motion of the G-buffer draws (see mfx_obj_target)
    int obj_ok, enc_obj;            // obj written this frame; the prepared frame uses it
    double near_p, far_p, fov_deg, aspect; int proj_src;   // this frame's projection for MetalFX (cam_projection)
    uint64_t obj_frames;
    id<MTLTexture> last_om;         // the per-object motion the last encoded frame used (nil: none), for mfx_debug_encode
    id<MTLTexture> dz;              // TOTK_MFX_FIXED_INPUT: the G-buffer depth resampled to the output size (dz_resample)
    id<MTLRenderPipelineState> dz_ps; id<MTLDepthStencilState> dz_ds; id<MTL4ArgumentTable> dz_args[MFX_OUT];
    id<MTLRenderPipelineState> dbg_ps[3]; MTLPixelFormat dbg_fmt[3];   // TOTK_MFX_DEBUG_VIEW, by target format
    id<MTL4ArgumentTable> dbg_args[MFX_OUT];
    id<MTLBuffer> dbg_par[MFX_OUT];
} g;
/* Flag mfx_mv_dyn (default on): pixels that are not still world get no screen motion instead of the camera's. */
static _Atomic int g_mv_dyn = 1;
void mfx_set_mv_dyn(int on) { atomic_store(&g_mv_dyn, on != 0); MLOG("[flag] mfx_mv_dyn = %d: %s", on != 0, on ? "moving pixels get no camera motion" : "camera motion everywhere"); }

/* Reconstructing identical MetalFX interpolators repeatedly grows device
 * allocations on the tested Mac, even after the old Objective-C object dies.
 * Keep a bounded set of exact configurations without eviction/rebuild churn.
 * In-flight frames retain their own resource payload independently of the
 * cache. Once full, new configurations use the previous uncached path. This does not establish or
 * fix every source of long-session memory growth. Diagnostic opt-out below. */
#define MFX_FI_CACHE 4
static id<MTL4FXFrameInterpolator> g_fi_cache[MFX_FI_CACHE];   // least recently used first
static uint64_t g_fi_bytes[MFX_FI_CACHE];                        // GPU memory each one added when it was made (fi_cache_add)
static unsigned g_fi_cache_count;
static int fi_cache_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_MFX_FI_CACHE"); on = !e || strcmp(e, "0"); }
    return on;
}
static void fi_cache_touch(id<MTL4FXFrameInterpolator> fi);
static id<MTL4FXFrameInterpolator> fi_cached(NSUInteger iw, NSUInteger ih) {
    if (!fi_cache_on()) return nil;
    for (unsigned i = 0; i < g_fi_cache_count; i++) {
        id<MTL4FXFrameInterpolator> fi = g_fi_cache[i];
        if (fi.inputWidth == iw && fi.inputHeight == ih && fi.outputWidth == g.out_w &&
            fi.outputHeight == g.out_h && fi.colorTextureFormat == g.scene_fmt) {
            MLOG("reuse cached interpolator %u: %lux%lu -> %lux%lu; reset history", i,
                 (unsigned long)iw, (unsigned long)ih, (unsigned long)g.out_w, (unsigned long)g.out_h);
            fi_cache_touch(fi);
            return fi;
        }
    }
    return nil;
}

/* TOTK_MFX_FIXED_INPUT (default 1): the interpolator's input size is the output size, whatever the render size: the
   G-buffer depth is resampled to it (nearest, one full-screen pass, dz_resample) and the motion kernel runs at it.
   MetalFX gives no memory back when an interpolator is released (measured 2026-10-23: 19 interpolators made while
   switching between six render resolutions added 1.66 GB that never came back, about what each added when made, 44-157
   MB at 2560x1440 output), so one interpolator per render size leaked with every switch the cache could not serve. Now
   a render resolution change keeps the same interpolator; only an output size change makes a new one.
   0: input = render size, one interpolator per render size (old). */
static int mfx_fixed_input(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_MFX_FIXED_INPUT"); on = !e || strcmp(e, "0");
        MLOG("TOTK_MFX_FIXED_INPUT=%d: %s", on, on ? "depth and motion at the output size, one interpolator per output size" : "depth and motion at the render size, one interpolator per render size (old)"); }
    return on;
}
/* g_on: the mode of this game frame (read by the replay, the present path and the pacing in hle_nvn.c); g_req: what the
   user asked for (key 2 / flag mfx_interp), taken over at the next present. */
static _Atomic int g_on, g_req;
static void mfx_release(int all);
static int g_bp_paused;   // mfx_present_tick: paused near the GPU memory budget
static int g_dbg_live;    // the debug view without interpolation holds textures (mfx_debug_only)
static int g_dbg_mem;     // the debug view without interpolation is held off near the GPU memory budget
static int g_applied = 1; // 0: the mode last chosen (mfx_set) has not been turned on yet (mfx_interp_state: refused)
static uint64_t g_mem_used, g_mem_budget;   // at the last present (mfx_present_tick)
static unsigned g_last_w, g_last_h;         // the last output size interpolation ran at (mfx_need_bytes)
static int mfx_refuse(int mode);
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
const char *mfx_mode_name(int m) {
    static const char *const names[MFX_NMODES] = {"off", "30+30 (the game at 30 fps)", "half the refresh rate (50+50 on a 100 Hz display)", "uncapped (every game frame interpolated)",
                                                  "40+40 (the game at 40 fps)", "50+50 (the game at 50 fps)"};   // totk_mfx_mode.h order
    return m >= 0 && m < MFX_NMODES ? names[m] : "?";
}
static void mfx_init(void) {
    const char *e = getenv("TOTK_MFX"); int v;
    if (!e || !*e || !strcmp(e, "0")) v = MFX_OFF;
    else if (!strcmp(e, "interp30")) v = MFX_30;
    else if (!strcmp(e, "interp")) v = MFX_HALF;
    else if (!strcmp(e, "interp40")) v = MFX_40;
    else if (!strcmp(e, "interp50")) v = MFX_50;
    else if (!strcmp(e, "interpmax")) v = MFX_MAX;
    else MDIE("TOTK_MFX must be interp30, interp (half the refresh rate), interp40, interp50, interpmax or 0, got %s", e);
    atomic_store(&g_req, v); atomic_store(&g_on, v);
    if (v) MLOG("TOTK_MFX=%s: MetalFX frame interpolation, %s", e, mfx_mode_name(v));
}
int mfx_enabled(void) { pthread_once(&g_once, mfx_init); return atomic_load_explicit(&g_on, memory_order_relaxed); }
int mfx_requested(void) { pthread_once(&g_once, mfx_init); return atomic_load_explicit(&g_req, memory_order_relaxed); }
void mfx_set(int m) {
    pthread_once(&g_once, mfx_init);
    if (m >= MFX_NMODES) { MLOG("[flag] mfx_interp takes 0..%d (-1 cycles), got %d", MFX_NMODES - 1, m); return; }
    int v = m < 0 ? (atomic_load(&g_req) + 1) % MFX_NMODES : m;
    if (g_bp_paused) { g_bp_paused = 0; MLOG("the user's choice ends the pause near the GPU memory budget"); }
    g_applied = v == MFX_OFF;
    atomic_store(&g_req, v);
    MLOG("[flag] mfx_interp = %d: %s (from the next game frame)", v, mfx_mode_name(v));
}
int mfx_latch(void) {
    pthread_once(&g_once, mfx_init);
    int r = atomic_load(&g_req), was = atomic_load(&g_on);
    if (r == was) return 0;
    if (was == MFX_OFF && mfx_refuse(r)) return 0;   // it would not fit: never turned on (mfx_interp_state says so)
    if (r != MFX_OFF) { g_applied = 1; g_dbg_live = 0; }   // the debug view's motion / depth textures are remade at interpolation's sizes (ensure_out, ensure_fi)
    g.prev_ok = g.hist_ok = 0; g.scene_ok = g.cam_ok = 0; g.depth = nil; g.last_prepare_s = 0;   // the next interpolated pair starts fresh
    atomic_store(&g_on, r);
    MLOG("frame interpolation %s -> %s", mfx_mode_name(was), mfx_mode_name(r));
    if (r == MFX_OFF) mfx_release(g_bp_paused);   // nothing of ours stays allocated while off (the interpolator cache too when paused)
    return 1;
}

static id<MTLTexture> mk(MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLTextureUsage u) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.usage = u; d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [g.dev newTextureWithDescriptor:d];
    if (!t) MDIE("texture %lux%lu format %lu", (unsigned long)w, (unsigned long)h, (unsigned long)f);
    if (!g.rs) { NSError *e = nil; g.rs = [g.dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&e]; if (!g.rs) MDIE("residency set: %s", e.localizedDescription.UTF8String); }
    [g.rs addAllocation:t]; [g.rs commit];
    return t;
}
/* Retired objects (textures of an old size, interpolators of an old configuration): the game's command buffers of the
   last frames and the interpolation payloads may still use them, and Metal 4 encoders do not retain what they use, so
   they stay referenced and resident for MFX_RETIRE_TICKS presents (mfx_present_tick; frames in flight are at most 3)
   and are then released in one go, with the bytes freed in the log. */
#define MFX_RETIRE_TICKS 8
#define MFX_RETIRE_MAX 64
static struct { __strong id obj; uint64_t tick, bytes; } g_ret[MFX_RETIRE_MAX];
static int g_nret; static uint64_t g_tick;
static NSHashTable *g_fi_gone;   // released interpolators (weak): memreport shows how many are still alive (held elsewhere)
static uint64_t g_fi_made, g_fi_made_bytes;   // interpolators made this session, and the GPU allocation they added
static uint64_t obj_bytes(id o) { return [o conformsToProtocol:@protocol(MTLResource)] ? (uint64_t)((id<MTLResource>)o).allocatedSize : 0; }
static int is_cached(id<MTL4FXFrameInterpolator> fi);
static void retire_drain(int all) {
    uint64_t bytes = 0, n = 0, fis = 0; int k = 0;
    for (int i = 0; i < g_nret; i++) {
        if (!all && g_tick - g_ret[i].tick < MFX_RETIRE_TICKS) { if (k != i) { g_ret[k] = g_ret[i]; g_ret[i].obj = nil; } k++; continue; }
        id o = g_ret[i].obj; g_ret[i].obj = nil;
        if ([o conformsToProtocol:@protocol(MTLAllocation)] && g.rs) [g.rs removeAllocation:(id<MTLAllocation>)o];
        if ([o conformsToProtocol:@protocol(MTL4FXFrameInterpolator)]) { fis++; if (!g_fi_gone) g_fi_gone = [NSHashTable weakObjectsHashTable]; [g_fi_gone addObject:o]; }
        bytes += g_ret[i].bytes; n++;
    }
    g_nret = k;
    if (!n) return;
    if (g.rs) [g.rs commit];
    MLOG("released %llu MB of retired MetalFX resources (%llu objects, %llu of them interpolators, whose own memory is not counted)",
         (unsigned long long)(bytes >> 20), (unsigned long long)n, (unsigned long long)fis);
}
static void retire(id o) {
    if (!o) return;
    for (int i = 0; i < g_nret; i++) if (g_ret[i].obj == o) return;
    if (g_nret == MFX_RETIRE_MAX) retire_drain(1);   // never expected (a few objects per size change); bounded all the same
    g_ret[g_nret].obj = o; g_ret[g_nret].tick = g_tick; g_ret[g_nret].bytes = obj_bytes(o); g_nret++;
}
// Retire a texture of ours (it leaves the residency set when released, see retire_drain).
static void drop(id<MTLTexture> __strong *t) { if (*t) { retire(*t); *t = nil; } }
static int is_cached(id<MTL4FXFrameInterpolator> fi) { for (unsigned i = 0; i < g_fi_cache_count; i++) if (g_fi_cache[i] == fi) return 1; return 0; }
/* Least recently used first. A new interpolator always joins (it is in use); the least recently used one is retired
   when the cache is full (before, a fifth configuration was never cached). It is not bounded by memory: releasing an
   interpolator gives no GPU memory back (TOTK_MFX_FIXED_INPUT), so evicting one only costs a rebuild that allocates
   again. g_fi_bytes: what each added when made (Metal reports no allocatedSize for it), at least fi_estimate. */
static uint64_t fi_estimate(NSUInteger iw, NSUInteger ih) { return (uint64_t)iw * ih * 16 + (uint64_t)g.out_w * g.out_h * 16; }
static void fi_cache_retire_at(unsigned i) {
    retire(g_fi_cache[i]);
    for (unsigned j = i + 1; j < g_fi_cache_count; j++) { g_fi_cache[j - 1] = g_fi_cache[j]; g_fi_bytes[j - 1] = g_fi_bytes[j]; }
    g_fi_cache_count--; g_fi_cache[g_fi_cache_count] = nil; g_fi_bytes[g_fi_cache_count] = 0;
}
static uint64_t fi_cache_bytes(void) { uint64_t b = 0; for (unsigned i = 0; i < g_fi_cache_count; i++) b += g_fi_bytes[i]; return b; }
static void fi_cache_touch(id<MTL4FXFrameInterpolator> fi) {   // most recently used last
    for (unsigned i = 0; i + 1 < g_fi_cache_count; i++) if (g_fi_cache[i] == fi) {
        uint64_t b = g_fi_bytes[i];
        for (unsigned j = i + 1; j < g_fi_cache_count; j++) { g_fi_cache[j - 1] = g_fi_cache[j]; g_fi_bytes[j - 1] = g_fi_bytes[j]; }
        g_fi_cache[g_fi_cache_count - 1] = fi; g_fi_bytes[g_fi_cache_count - 1] = b; return;
    }
}
static void fi_cache_add(id<MTL4FXFrameInterpolator> fi, uint64_t bytes) {
    unsigned n = 0;
    while (g_fi_cache_count >= MFX_FI_CACHE) { fi_cache_retire_at(0); n++; }
    g_fi_cache[g_fi_cache_count] = fi; g_fi_bytes[g_fi_cache_count] = bytes; g_fi_cache_count++;
    if (n) MLOG("interpolator cache full: the least recently used one retired for the new one (%u cached, about %llu MB)", g_fi_cache_count, (unsigned long long)(fi_cache_bytes() >> 20));
}
/* The cached interpolators of another output size or colour format than the current one: an interpolator holds history
   and intermediate textures of its output size (hundreds of MB at 3456x1944), and a resolution change would otherwise
   keep up to MFX_FI_CACHE of them alongside the new one. all: every cached one (memory released for a pause). */
static unsigned fi_cache_evict(int all) {
    unsigned k = 0, n = 0;
    for (unsigned i = 0; i < g_fi_cache_count; i++) {
        id<MTL4FXFrameInterpolator> fi = g_fi_cache[i];
        if (!all && fi.outputWidth == g.out_w && fi.outputHeight == g.out_h && fi.colorTextureFormat == g.scene_fmt) { g_fi_bytes[k] = g_fi_bytes[i]; g_fi_cache[k++] = fi; }
        else { retire(fi); n++; }
    }
    for (unsigned i = k; i < g_fi_cache_count; i++) { g_fi_cache[i] = nil; g_fi_bytes[i] = 0; }
    g_fi_cache_count = k;
    return n;
}

// Scene / UI / output textures follow the canvas size; the interpolator also depends on the depth size.
static void ensure_out(id<MTLDevice> dev, NSUInteger w, NSUInteger h, MTLPixelFormat fmt) {
    if (!g.dev) g.dev = dev;
    if (g.out_w == w && g.out_h == h && g.scene[0] && (!fmt || fmt == g.scene_fmt)) return;
    if (!fmt) fmt = g.scene_fmt ? g.scene_fmt : MTLPixelFormatRG11B10Float;
    uint64_t old = 0; NSUInteger ow = g.out_w, oh = g.out_h; int had = g.scene[0] != nil;
    for (int i = 0; i < 2; i++) { old += obj_bytes(g.scene[i]); drop(&g.scene[i]); }
    old += obj_bytes(g.ui) + obj_bytes(g.ui2); drop(&g.ui); drop(&g.ui2);
    for (int i = 0; i < MFX_OUT; i++) { old += obj_bytes(g.out[i]); drop(&g.out[i]); g.outv[i] = nil; }
    /* The interpolator and motion texture of the old size go too (the new ones are made at the next prepare), and the
       reference mfx_dump keeps to the old depth. The per-object motion target follows the depth size (mfx_obj_target). */
    old += obj_bytes(g.mv); drop(&g.mv); old += obj_bytes(g.pdepth); drop(&g.pdepth); g.pdepth_frame = 0; old += obj_bytes(g.dz); drop(&g.dz);
    if (g.fi && !is_cached(g.fi)) retire(g.fi);
    g.fi = nil; g.last_depth = nil; g.last_slot = -1; g.in_w = g.in_h = 0;
    MTLTextureUsage rt = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    for (int i = 0; i < 2; i++) g.scene[i] = mk(fmt, w, h, MTLTextureUsageShaderRead);   // blit copies of the canvas
    g.scene_fmt = fmt;
    g.ui = mk(MTLPixelFormatRGBA16Float, w, h, rt);
    for (int i = 0; i < MFX_OUT; i++) {
        g.out[i] = mk(MTLPixelFormatRGBA8Unorm_sRGB, w, h, MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView);
        g.outv[i] = [g.out[i] newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
    }
    g.out_w = w; g.out_h = h; g_last_w = (unsigned)w; g_last_h = (unsigned)h; g.fi = nil; g.prev_ok = g.hist_ok = 0;
    unsigned evicted = fi_cache_evict(0);
    MLOG("scene (format %lu) / UI / output textures %lux%lu", (unsigned long)fmt, (unsigned long)w, (unsigned long)h);
    if (had) MLOG("size change %lux%lu -> %lux%lu: %llu MB of textures of the old size and %u cached interpolators of another output size retired (released after %d presents)",
                  (unsigned long)ow, (unsigned long)oh, (unsigned long)w, (unsigned long)h, (unsigned long long)(old >> 20), evicted, MFX_RETIRE_TICKS);
}
id<MTLTexture> mfx_scene_target(id<MTLDevice> dev, NSUInteger w, NSUInteger h, MTLPixelFormat fmt) { ensure_out(dev, w, h, fmt); return g.scene[g.cur]; }
id<MTLTexture> mfx_ui_target(id<MTLDevice> dev, NSUInteger w, NSUInteger h) { ensure_out(dev, w, h, 0); return g.ui; }
id<MTLTexture> mfx_ui_alt(void) {
    if (!g.ui) return nil;
    if (!g.ui2) g.ui2 = mk(MTLPixelFormatRGBA16Float, g.out_w, g.out_h, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    return g.ui2;
}
void mfx_ui_swap(void) { id<MTLTexture> t = g.ui; g.ui = g.ui2; g.ui2 = t; }
id<MTLResidencySet> mfx_residency(void) { return g.rs; }
void mfx_note_scene(void) { g.scene_ok = 1; }
/* TOTK_OBJ_MOTION: the RGBA16Float target the G-buffer draws write their motion to (RG: NDC delta, previous minus
   current; OBJMV_NONE: no motion known, the camera motion is used; B: the view distance (clip w) of the fragment that
   wrote it, checked against the final depth by mfx_mv). Same size as the G-buffer depth. */
int mfx_obj_motion_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_OBJ_MOTION"); on = e && atoi(e) > 0; if (on) MLOG("TOTK_OBJ_MOTION=1: per-object motion vectors from the opaque G-buffer draws"); }
    return on;
}
id<MTLTexture> mfx_obj_target(id<MTLDevice> dev, NSUInteger w, NSUInteger h) {
    if (!g.dev) g.dev = dev;
    if (!g.obj || g.obj.width != w || g.obj.height != h) {
        uint64_t old = obj_bytes(g.obj); NSUInteger ow = g.obj ? g.obj.width : 0, oh = g.obj ? g.obj.height : 0;
        drop(&g.obj); g.obj = mk(MTLPixelFormatRGBA16Float, w, h, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        if (ow) MLOG("per-object motion target %lux%lu (the %lux%lu one, %llu MB, retired)", (unsigned long)w, (unsigned long)h, (unsigned long)ow, (unsigned long)oh, (unsigned long long)(old >> 20));
        else MLOG("per-object motion target %lux%lu", (unsigned long)w, (unsigned long)h);
    }
    return g.obj;
}
void mfx_note_obj(void) { g.obj_ok = 1; }
void mfx_note_depth(id<MTLTexture> d) { if (d && d.pixelFormat == MTLPixelFormatDepth32Float && d.textureType == MTLTextureType2D) g.depth = d; }
void mfx_note_camera(const float *c, int n) {
    if (n < 84) return;
    for (int i = 0; i < 84; i++) g.cam[i] = c[i];
    g.cam_ok = c[56] > 0 && c[57] > c[56] && c[62] > 0 && c[70] > 0;   // near, far, aspect, vertical field of view (radians)
}

/* The motion kernel (mfx_mv), its argument tables and parameter buffers: made once, for interpolation or the debug view
   alone (mfx_debug_only). g.rs must exist (a texture of ours was made first). */
static void mv_pipeline(void) {
    if (g.mv_ps) return;
    NSError *e = nil;
    static const char *src =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct P { float4x4 reproj; float2 size; float2 inv_size; float yflip; float obj; float dyn, maxpx; float2 om_scale; float near, ztol, dilate, nf; };\n"
        // view distance from a depth value (not reversed, infinite far plane: z = near / (1 - d)), in units of near
        "static float vz(float d) { return 1.0 / max(1.0 - d, 1e-7); }\n"
        // camera motion of depth pixel g (depth d0): where it was in the previous frame, in pixels of this texture. With
        // TOTK_OBJ_MOTION the motion the G-buffer draw wrote there wins (an NDC delta in Metal clip space: y up,
        // texture rows down); OBJMV_NONE (65000) or anything that large means none, the camera motion is used.
        // The per-object motion is read at the same render-size texel the depth came from (TOTK_MFX_FIXED_INPUT: the
        // resampled depth takes the texel under the pixel centre) and taken only where the view distance its draw
        // stored (B) is the one of the final depth there within ztol: a draw that is not a motion draw (terrain, large
        // meshes, border-emulated shaders) leaves the attachment alone, so the motion of an object it hides stays under it.
        // With dyn, a pixel whose distance at that place in the previous frame is not what a still point would have
        // had moved itself: no motion on screen. Still: within the 3x3 range of the previous depth there (1 % margin)
        // where that depth is continuous (a slope), and within 1 % of one of the nine where it is not (an edge: the
        // range of Link and the land behind him would take in any distance between them). Only a point in front of what
        // was there moved itself; one behind it (farther than all nine) was hidden then (the land Link uncovers as he or
        // the camera moves) and keeps the camera motion: zero there drew a dark Link-shaped trail beside him.
        // The object's motion is dropped when it differs from the camera motion (before the still-world test) by more
        // than maxpx pixels: such a jump is almost always a draw matched with another object (culling changed while turning).
        "static float2 mv_at(uint2 g, float d0, texture2d<float, access::read> om, depth2d<float, access::read> pd, constant P &p) {\n"
        "  float2 uv = (float2(g) + 0.5) * p.inv_size;\n"
        "  float4 c = p.reproj * float4(uv.x * 2.0 - 1.0, p.yflip * (1.0 - uv.y * 2.0), d0 * 2.0 - 1.0, 1.0);\n"
        "  float2 m = 0.0, cm = 0.0;\n"
        "  if (c.w > 1e-7) {\n"
        "    float2 n = c.xy / c.w, prev = float2(n.x * 0.5 + 0.5, 0.5 - 0.5 * p.yflip * n.y);\n"
        "    m = (prev - uv) * p.size; cm = m;\n"
        "    int2 q = int2(floor(prev * p.size));\n"
        "    if (p.dyn > 0.5 && d0 < 1.0 && all(q >= 1) && all(q < int2(p.size) - 1)) {\n"
        "      float ze = vz(c.z / c.w * 0.5 + 0.5), zmin = 3.4e38, zmax = 0.0; bool near = false;\n"
        "      for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {\n"
        "        float z = vz(pd.read(uint2(q + int2(x, y)))); zmin = min(zmin, z); zmax = max(zmax, z); near = near || fabs(z - ze) <= ze * 0.01; }\n"
        "      bool still = zmax <= zmin * 1.05 ? (ze >= zmin * 0.99 && ze <= zmax * 1.01) : near;\n"
        "      if (!still && !(ze > zmax * 1.01)) m = 0.0;\n"
        "    }\n"
        "  }\n"
        "  if (any(isnan(m)) || any(isinf(m))) m = 0.0;\n"
        "  if (p.obj > 0.5 && d0 < 1.0) {\n"
        "    uint2 oq = min(uint2((float2(g) + 0.5) * p.om_scale), uint2(om.get_width() - 1, om.get_height() - 1));\n"
        "    float4 o = om.read(oq); float zd = p.near / max(1.0 - d0 * p.nf, 1e-7);   // nf = 1 - near / far\n"
        "    if (all(abs(o.xy) < 16.0) && fabs(o.z - zd) <= zd * p.ztol) {\n"
        "      float2 op = o.xy * float2(0.5, -0.5) * p.size; if (length(op - cm) <= p.maxpx) m = op; } }\n"
        "  return m;\n"
        "}\n"
        // dilate (TOTK_MFX_MV_DILATE=1): the motion of the nearest depth of the 3x3 around the pixel (as FSR 2/3 hands
        // its frame generation dilated motion), so a moving object's silhouette edge carries its motion, not the background's.
        "kernel void mfx_mv(depth2d<float, access::read> d [[texture(0)]], texture2d<float, access::write> mv [[texture(1)]],\n"
        "                   texture2d<float, access::read> om [[texture(2)]], depth2d<float, access::read> pd [[texture(3)]],\n"
        "                   constant P &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
        "  if (g.x >= uint(p.size.x) || g.y >= uint(p.size.y)) return;\n"
        "  uint2 at = g; float d0 = d.read(g);\n"
        "  if (p.dilate > 0.5) {\n"
        "    int2 hi = int2(p.size) - 1;\n"
        "    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {\n"
        "      uint2 q = uint2(clamp(int2(g) + int2(x, y), int2(0), hi)); float dq = d.read(q); if (dq < d0) { d0 = dq; at = q; } }\n"
        "  }\n"
        "  mv.write(float4(mv_at(at, d0, om, pd, p), 0.0, 0.0), g);\n"
        "}\n";
    MTLCompileOptions *o = [MTLCompileOptions new]; o.mathMode = MTLMathModeSafe;
    id<MTLLibrary> lib = [g.dev newLibraryWithSource:@(src) options:o error:&e];
    id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"mfx_mv"] : nil;
    g.mv_ps = fn ? [g.dev newComputePipelineStateWithFunction:fn error:&e] : nil;
    if (!g.mv_ps) MDIE("motion vector kernel: %s", e.localizedDescription.UTF8String);
    MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1; ad.maxTextureBindCount = 4;
    for (int i = 0; i < MFX_OUT; i++) {
        g.mv_args[i] = [g.dev newArgumentTableWithDescriptor:ad error:&e]; if (!g.mv_args[i]) MDIE("argument table: %s", e.localizedDescription.UTF8String);
        g.mv_par[i] = [g.dev newBufferWithLength:256 options:MTLResourceStorageModeShared]; [g.rs addAllocation:g.mv_par[i]];
    }
    [g.rs commit];
    g.fence = [g.dev newFence];
    const char *yf = getenv("TOTK_MFX_YFLIP"); g.yflip = yf ? atof(yf) : 1.0;   // diagnostic: -1 tries the other clip-space y convention
}
static void ensure_fi(NSUInteger iw, NSUInteger ih) {
    if (g.fi && g.in_w == iw && g.in_h == ih && g.fi.colorTextureFormat == g.scene_fmt && g.fi.outputWidth == g.out_w && g.fi.outputHeight == g.out_h) return;
    NSError *e = nil;
    if (g.fi && !is_cached(g.fi)) retire(g.fi);   // another input (render) size: the old interpolator unless the cache keeps it
    g.fi = fi_cached(iw, ih);
    if (!g.fi) {
        MTLFXFrameInterpolatorDescriptor *d = [MTLFXFrameInterpolatorDescriptor new];
        d.colorTextureFormat = g.scene_fmt; d.outputTextureFormat = MTLPixelFormatRGBA8Unorm_sRGB;
        d.depthTextureFormat = MTLPixelFormatDepth32Float; d.motionTextureFormat = MTLPixelFormatRG16Float; d.uiTextureFormat = MTLPixelFormatRGBA16Float;
        d.inputWidth = iw; d.inputHeight = ih; d.outputWidth = g.out_w; d.outputHeight = g.out_h;
        id<MTL4Compiler> comp = [g.dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&e];
        if (!comp) MDIE("MTL4 compiler: %s", e.localizedDescription.UTF8String);
        if (![MTLFXFrameInterpolatorDescriptor supportsMetal4FX:g.dev]) MDIE("this GPU has no Metal 4 MetalFX frame interpolation");
        uint64_t before = (uint64_t)g.dev.currentAllocatedSize;
        g.fi = [d newFrameInterpolatorWithDevice:g.dev compiler:comp];
        if (!g.fi) MDIE("frame interpolator %lux%lu -> %lux%lu not created", (unsigned long)iw, (unsigned long)ih, (unsigned long)g.out_w, (unsigned long)g.out_h);
        uint64_t after = (uint64_t)g.dev.currentAllocatedSize, est = fi_estimate(iw, ih), b = after > before + est ? after - before : est;
        MLOG("new interpolator %lux%lu -> %lux%lu: GPU allocated %+lld MB at creation (counted as %llu MB)", (unsigned long)iw, (unsigned long)ih, (unsigned long)g.out_w, (unsigned long)g.out_h,
             (long long)(after - before) / 1048576, (unsigned long long)(b >> 20));
        g_fi_made++; g_fi_made_bytes += after > before ? after - before : 0;
        if (fi_cache_on()) fi_cache_add(g.fi, b);
    }
    drop(&g.mv); g.mv = mk(MTLPixelFormatRG16Float, iw, ih, MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
    drop(&g.pdepth); g.pdepth = mk(MTLPixelFormatDepth32Float, iw, ih, MTLTextureUsageShaderRead); g.pdepth_frame = 0;
    drop(&g.dz); if (mfx_fixed_input()) g.dz = mk(MTLPixelFormatDepth32Float, iw, ih, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    mv_pipeline();
    g.in_w = iw; g.in_h = ih; g.prev_ok = g.hist_ok = 0;
    MLOG("frame interpolator: depth / motion %lux%lu -> %lux%lu (scene format %lu, output RGBA8 sRGB, UI RGBA16F premultiplied)", (unsigned long)iw, (unsigned long)ih, (unsigned long)g.out_w, (unsigned long)g.out_h, (unsigned long)g.scene_fmt);
}

// 4x4 inverse (Gauss-Jordan, partial pivoting) and product, row-major, in double: the view-projection matrices hold
// world coordinates in the thousands, and only the combined reprojection goes to the GPU in float.
static int inv4(const double a_[16], double r[16]) {
    double a[16]; memcpy(a, a_, sizeof a);
    for (int i = 0; i < 16; i++) r[i] = i % 5 == 0;
    for (int c = 0; c < 4; c++) {
        int p = c; for (int k = c + 1; k < 4; k++) if (fabs(a[k * 4 + c]) > fabs(a[p * 4 + c])) p = k;
        if (fabs(a[p * 4 + c]) < 1e-12) return 0;
        if (p != c) for (int k = 0; k < 4; k++) { double t = a[c * 4 + k]; a[c * 4 + k] = a[p * 4 + k]; a[p * 4 + k] = t; t = r[c * 4 + k]; r[c * 4 + k] = r[p * 4 + k]; r[p * 4 + k] = t; }
        double s = 1.0 / a[c * 4 + c];
        for (int k = 0; k < 4; k++) { a[c * 4 + k] *= s; r[c * 4 + k] *= s; }
        for (int q = 0; q < 4; q++) if (q != c) { double f = a[q * 4 + c]; if (f != 0) for (int k = 0; k < 4; k++) { a[q * 4 + k] -= f * a[c * 4 + k]; r[q * 4 + k] -= f * r[c * 4 + k]; } }
    }
    return 1;
}
static void mul4(const double a[16], const double b[16], double r[16]) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { double s = 0; for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j]; r[i * 4 + j] = s; }
}
/* MetalFX's near / far plane, vertical field of view and aspect ratio (WWDC25 session 211: wrong values give occlusion
   artefacts) from the matrices of this frame rather than the block's scalar fields alone: projection = view-projection
   (floats 12..27) times the inverse view (floats 0..11, 3x4 rows). GL-style clip depth mapped to 0..1, as the motion
   kernel takes it, is the same function of view depth as Metal's 0..1 projection with the same near / far, so the
   planes carry over. An infinite far plane (P[2][2] = -1) is given as 1e7 (MetalFX linearises with it; the depth curve
   differs from the infinite one by near / 1e7). Falls back to the block's floats 56 / 57 / 70 / 62 when the product
   is not a perspective projection. */
static void cam_projection(void) {
    double v[16] = {0}, iv[16], vp[16], p[16];
    for (int i = 0; i < 12; i++) v[i] = g.cam[i];
    v[15] = 1;
    for (int i = 0; i < 16; i++) vp[i] = g.cam[12 + i];
    int ok = inv4(v, iv);
    if (ok) mul4(vp, iv, p);
    double n = 0, f = 0, fov = 0, asp = 0;
    if (ok && fabs(p[14] + 1) < 1e-3 && fabs(p[15]) < 1e-3 && p[5] != 0 && p[0] != 0) {
        double a = p[10], b = p[11];
        n = b / (a - 1); f = fabs(a + 1) < 1e-7 ? INFINITY : b / (a + 1);
        fov = 2 * atan(1 / fabs(p[5])); asp = fabs(p[5] / p[0]);
    }
    static int block_only = -1; if (block_only < 0) { const char *e = getenv("TOTK_MFX_PROJ"); block_only = e && !strcmp(e, "block"); }   // A/B: the old block floats
    int src = !block_only && n > 0 && f > n && fov > 0 && fov < M_PI && asp > 0;
    if (src) { g.near_p = n; g.far_p = isinf(f) ? 1e7 : f; g.fov_deg = fov * 180 / M_PI; g.aspect = asp; }
    else { g.near_p = g.cam[56]; g.far_p = g.cam[57]; g.fov_deg = g.cam[70] * 180 / M_PI; g.aspect = g.cam[62]; }
    if (src != g.proj_src || !g.made) {
        static int said; if (said++ < 4)
            MLOG("MetalFX projection from %s: near %g far %g fov %.3f deg aspect %.4f (block floats: near %g far %g fov %.3f deg aspect %.4f)",
                 src ? "the matrices" : "the camera block", g.near_p, g.far_p, g.fov_deg, g.aspect, g.cam[56], g.cam[57], g.cam[70] * 180 / M_PI, g.cam[62]);
    }
    g.proj_src = src;
}
// A camera jump (teleport, cutscene cut) or a turn of more than 25 degrees in one game frame is not interpolated.
static int camera_cut(void) {
    double dx = g.cam[47] - g.prev_cam[47], dy = g.cam[51] - g.prev_cam[51], dz = g.cam[55] - g.prev_cam[55];
    double f = g.cam[8] * g.prev_cam[8] + g.cam[9] * g.prev_cam[9] + g.cam[10] * g.prev_cam[10];   // view z axes
    return sqrt(dx * dx + dy * dy + dz * dz) > 8.0 || f < cos(25.0 * M_PI / 180.0);
}

/* The reprojection this frame -> previous frame (previous view-projection times the inverse of this one); 0: singular. */
static int mv_reproj(double m[16]) {
    double vc[16], vp[16], ic[16];
    for (int i = 0; i < 16; i++) { vc[i] = g.cam[12 + i]; vp[i] = g.prev_cam[12 + i]; }
    if (!inv4(vc, ic)) return 0;
    mul4(vp, ic, m);
    return 1;
}
/* The motion kernel's parameters for slot s (g.mv_par[s]): reprojection m, the grid (g.mv's size), per-object motion
   (obj), the still-world test, this frame's projection (cam_projection). */
static void mv_params(int s, const double m[16], id<MTLTexture> depth, int obj) {
    float *par = g.mv_par[s].contents;
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) par[c * 4 + r] = (float)m[r * 4 + c];   // float4x4 is column-major
    NSUInteger mw = g.mv.width, mh = g.mv.height;   // the motion kernel's grid: the render size, or the output size (TOTK_MFX_FIXED_INPUT)
    par[16] = (float)mw; par[17] = (float)mh; par[18] = 1.0f / mw; par[19] = 1.0f / mh; par[20] = (float)g.yflip;
    par[24] = (float)depth.width / mw; par[25] = (float)depth.height / mh;   // per-object motion is at the depth size
    par[21] = obj ? 1.0f : 0.0f; g.enc_obj = obj; g.obj_frames += obj;
    par[22] = atomic_load(&g_mv_dyn) && g.pdepth_frame && g.pdepth_frame == g.frames - 1 ? 1.0f : 0.0f;   // the previous game frame's depth is there
    static double maxpct = -1;   // TOTK_OBJ_MOTION_MAXPCT: an object's own motion beyond this % of the width is a mismatch (default 3)
    if (maxpct < 0) { const char *e = getenv("TOTK_OBJ_MOTION_MAXPCT"); maxpct = e ? atof(e) : 3.0; if (maxpct <= 0) maxpct = 1000; }
    par[23] = (float)(maxpct * 0.01 * mw);
    cam_projection();
    /* TOTK_OBJ_MOTION_ZTOL (percent, default 2; 0: no check): the per-object motion only where the view distance its draw
       stored is the final depth's, near / (1 - d (1 - near / far)) with this frame's planes. */
    static double ztol = -1;
    if (ztol < 0) { const char *e = getenv("TOTK_OBJ_MOTION_ZTOL"); ztol = e ? atof(e) : 2.0; if (ztol <= 0) ztol = 1e9;
                    if (mfx_obj_motion_on()) MLOG("TOTK_OBJ_MOTION_ZTOL=%g%%: per-object motion only where its draw is the surface the depth shows", ztol); }
    static int dilate = -1;
    if (dilate < 0) { const char *e = getenv("TOTK_MFX_MV_DILATE"); dilate = e && atoi(e) > 0; if (dilate) MLOG("TOTK_MFX_MV_DILATE=1: motion of the nearest depth in each 3x3"); }
    par[26] = (float)g.near_p; par[27] = (float)(ztol * 0.01); par[28] = dilate ? 1.0f : 0.0f; par[29] = (float)(g.far_p > g.near_p && g.near_p > 0 ? 1.0 - g.near_p / g.far_p : 1.0);
}
int mfx_frame_prepare(double frame_s) {
    g.frames++;
    id<MTLTexture> depth = g.depth;
    int ok = g.scene_ok && g.cam_ok && depth && g.ui;
    /* MetalFX deltaTime: the real interval between the two game frames (20 ms at 100 Hz when the game keeps up), else the
       pacing period. The pacing itself is held at two display refreshes by present_ticks (hle_nvn.c). */
    double now = CACurrentMediaTime(), dt = g.last_prepare_s > 0 ? now - g.last_prepare_s : frame_s;
    g.last_prepare_s = now; g.dt = dt > 1.0 / 250 && dt < 0.1 ? dt : frame_s > 0 ? frame_s : 1.0 / 30;
    int obj = g.obj_ok && mfx_obj_motion_on() && g.obj && depth && g.obj.width == depth.width && g.obj.height == depth.height; g.obj_ok = 0;
    g.scene_ok_was = g.scene_ok; g.cam_ok_was = g.cam_ok; g.scene_ok = g.cam_ok = 0; g.depth = nil;
    if (!ok) {
        g.skip_input++; g.miss[0] += !g.scene_ok_was; g.miss[1] += !g.cam_ok_was; g.miss[2] += !depth; g.miss[3] += !g.ui;
        g.prev_ok = g.hist_ok = 0; return -1;
    }
    if (mfx_fixed_input()) ensure_fi(g.out_w, g.out_h); else ensure_fi(depth.width, depth.height);
    if (!g.prev_ok || camera_cut()) {
        if (g.prev_ok) g.skip_cut++; else g.skip_first++;
        memcpy(g.prev_cam, g.cam, sizeof g.cam); g.cur ^= 1; g.prev_ok = 1; g.hist_ok = 0;
        return -1;
    }
    double m[16];
    if (!mv_reproj(m)) { g.skip_input++; g.prev_ok = g.hist_ok = 0; return -1; }
    int s = g.slot = (g.slot + 1) % MFX_OUT;
    mv_params(s, m, depth, obj);
    g.enc_depth = depth;
    return s;
}
/* TOTK_MFX_FIXED_INPUT: the game's G-buffer depth (render size) resampled to g.dz (the output size), nearest, by one
   full-screen triangle writing the depth. 0: the pipeline could not be made. */
static int dz_resample(id<MTL4CommandBuffer> cb, int s, id<MTLTexture> src) {
    if (!g.dz_ps) {
        static int failed; if (failed) return 0;
        static const char *msl =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 p [[position]]; };\n"
            "vertex V vs(uint i [[vertex_id]]) { V o; float2 t = float2((i << 1) & 2, i & 2); o.p = float4(t * 2.0 - 1.0, 0.0, 1.0); return o; }\n"
            "struct F { float d [[depth(any)]]; };\n"
            "fragment F fs(V in [[stage_in]], depth2d<float, access::read> d [[texture(0)]], constant float2 &inv [[buffer(0)]]) {\n"
            "  uint2 sz = uint2(d.get_width(), d.get_height());\n"
            "  uint2 q = min(uint2(in.p.xy * inv * float2(sz)), sz - 1);\n"
            "  F f; f.d = d.read(q); return f;\n"
            "}\n";
        NSError *e = nil;
        id<MTLLibrary> lib = [g.dev newLibraryWithSource:@(msl) options:nil error:&e];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        g.dz_ps = lib ? [g.dev newRenderPipelineStateWithDescriptor:pd error:&e] : nil;
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new]; dd.depthCompareFunction = MTLCompareFunctionAlways; dd.depthWriteEnabled = YES;
        g.dz_ds = [g.dev newDepthStencilStateWithDescriptor:dd];
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1; ad.maxTextureBindCount = 1;
        for (int i = 0; i < MFX_OUT && g.dz_ps; i++) if (!(g.dz_args[i] = [g.dev newArgumentTableWithDescriptor:ad error:&e])) g.dz_ps = nil;
        if (!g.dz_ps || !g.dz_ds) { failed = 1; MLOG("[red] depth resample pipeline: %s", e ? e.localizedDescription.UTF8String : "?"); return 0; }
        MLOG("depth resample pipeline ready (TOTK_MFX_FIXED_INPUT)");
    }
    float *inv = (float *)((char *)g.mv_par[s].contents + 128);   // after the motion kernel's parameters (P is 120 bytes)
    inv[0] = 1.0f / g.dz.width; inv[1] = 1.0f / g.dz.height;
    MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
    rp.depthAttachment.texture = g.dz; rp.depthAttachment.loadAction = MTLLoadActionDontCare; rp.depthAttachment.storeAction = MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    if (!e) return 0;
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageFragment visibilityOptions:MTL4VisibilityOptionDevice];   // the game's frame is done
    [g.dz_args[s] setTexture:src.gpuResourceID atIndex:0]; [g.dz_args[s] setAddress:g.mv_par[s].gpuAddress + 128 atIndex:0];
    [e setArgumentTable:g.dz_args[s] atStages:MTLRenderStageFragment];
    [e setRenderPipelineState:g.dz_ps]; [e setDepthStencilState:g.dz_ds];
    [e setViewport:(MTLViewport){0, 0, (double)g.dz.width, (double)g.dz.height, 0, 1}];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    return 1;
}
/* The motion kernel for slot s into cb (after the game's frame), then this frame's depth copied to g.pdepth for the next
   frame's still-world test. Returns texture 2 of the kernel (the per-object motion target, or g.mv when none is used). */
static id<MTLTexture> mv_encode(id<MTL4CommandBuffer> cb, int s, id<MTLTexture> depth) {
    [g.mv_args[s] setTexture:depth.gpuResourceID atIndex:0]; [g.mv_args[s] setTexture:g.mv.gpuResourceID atIndex:1]; [g.mv_args[s] setAddress:g.mv_par[s].gpuAddress atIndex:0];
    id<MTLTexture> om = g.enc_obj && g.obj ? g.obj : g.mv;   // without per-object motion the kernel never reads texture 2; any valid texture
    [g.mv_args[s] setTexture:om.gpuResourceID atIndex:2];
    [g.mv_args[s] setTexture:g.pdepth.gpuResourceID atIndex:3];
    id<MTL4ComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch | MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];   // the game's frame is done
    [e setComputePipelineState:g.mv_ps]; [e setArgumentTable:g.mv_args[s]];
    [e dispatchThreads:MTLSizeMake(g.mv.width, g.mv.height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [e updateFence:g.fence afterEncoderStages:MTLStageDispatch];
    // this frame's depth for the next one's still-world test, once the motion has read the previous one
    [e barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
    [e copyFromTexture:depth toTexture:g.pdepth];
    [e endEncoding];
    g.pdepth_frame = g.frames;
    return om;
}
void mfx_frame_abort(void) { g.enc_depth = nil; g.prev_ok = g.hist_ok = 0; g.skip_busy++; }
NSArray *mfx_frame_encode(id<MTL4CommandBuffer> cb, int s, id<MTL4CounterHeap> heap, NSUInteger stamp) {
    id<MTLTexture> depth = g.enc_depth; g.enc_depth = nil; g.last_depth = depth; g.last_slot = s;
    /* Our scene / UI / motion / output textures also made resident on this command buffer itself (the caller has just
       begun it), not only through the queue (mfx_snapshot_scene adds g.rs to the queue once): an Apple developer forum
       report (thread 840795) has MTL4FXFrameInterpolator writing nothing unless a residency set holding its textures is
       attached to the command buffer after beginCommandBufferWithAllocator:. Harmless where the queue's set already
       works (TOTK); TOTK_MFX_CB_RESIDENCY=0 leaves it to the queue alone, as before. */
    static int cb_rs = -1; if (cb_rs < 0) { const char *e = getenv("TOTK_MFX_CB_RESIDENCY"); cb_rs = !e || strcmp(e, "0"); }
    if (cb_rs && g.rs) [cb useResidencySet:g.rs];
    id<MTLTexture> game_depth = depth;
    if (g.dz && (depth.width != g.dz.width || depth.height != g.dz.height)) {
        if (!dz_resample(cb, s, depth)) { g.prev_ok = g.hist_ok = 0; }   // never expected; the motion kernel then reads past the smaller depth's edge
        else { depth = g.dz; g.last_depth = depth; }
    }
    id<MTLTexture> om = mv_encode(cb, s, depth);
    if (heap) [cb writeTimestampIntoHeap:heap atIndex:stamp];   // after the motion vectors (which waited for the frame)
    id<MTL4FXFrameInterpolator> fi = g.fi;
    fi.colorTexture = g.scene[g.cur]; fi.prevColorTexture = g.scene[g.cur ^ 1]; fi.depthTexture = depth; fi.motionTexture = g.mv;
    fi.uiTexture = g.ui; fi.uiTextureComposited = NO; fi.outputTexture = g.out[s]; fi.fence = g.fence;
    /* Per frame (WWDC25 session 211 lists these as the parameters that cause occlusion artefacts when wrong):
       motion already in pixels of the motion texture (scale 1); no jitter (the game anti-aliases with FXAA, and with
       TOTK_TAA the colour given here is the TAA output, which has none either); deltaTime the measured interval between
       the two game frames; depth not reversed (D32F, 0 near, GL-style clip depth mapped to 0..1); near / far / vertical
       field of view in degrees / aspect ratio of this frame's projection (cam_projection). */
    fi.motionVectorScaleX = 1; fi.motionVectorScaleY = 1; fi.jitterOffsetX = 0; fi.jitterOffsetY = 0;
    fi.deltaTime = (float)g.dt; fi.nearPlane = (float)g.near_p; fi.farPlane = (float)g.far_p;
    fi.fieldOfView = (float)g.fov_deg; fi.aspectRatio = (float)g.aspect; fi.depthReversed = NO;
    fi.shouldResetHistory = !g.hist_ok;
    [fi encodeToCommandBuffer:cb];
    // Metal 4 work needs explicit resource lifetime. Hold this exact frame's
    // bindings through completion, while the configuration cache retains only
    // the interpolator, not old scene/pool textures between configurations.
    NSMutableArray *resources = [@[fi, fi.colorTexture, fi.prevColorTexture, depth,
                                   g.mv, g.ui, g.out[s], g.mv_par[s], g.pdepth] mutableCopy];
    [resources addObject:om];   // texture 2: the per-object motion target (TOTK_OBJ_MOTION) or g.mv; always MFX_FRAME_RESOURCES entries
    if (game_depth != depth) [resources addObject:game_depth];   // read by the resample pass
    g.last_om = g.enc_obj && g.obj ? g.obj : nil;   // mfx_debug_encode reads it at the texel mfx_mv did
    fi.colorTexture = nil; fi.prevColorTexture = nil; fi.depthTexture = nil;
    fi.motionTexture = nil; fi.uiTexture = nil; fi.outputTexture = nil;
    if (heap) [cb writeTimestampIntoHeap:heap atIndex:stamp + 1];
    g.mv_prev_yaw = atan2(g.prev_cam[2], g.prev_cam[0]) * 180 / M_PI;   // yaw of the previous frame, for mfx_mv_log
    memcpy(g.prev_cam, g.cam, sizeof g.cam); g.cur ^= 1; g.hist_ok = 1; g.made++;
    return resources;
}
/* TOTK_MFX_DEBUG_VIEW=motion | depth | split (or the setting graphics.mfx_debug_view, flag mfx_debug_view 0..3): what
   MetalFX was given for the frame just encoded, drawn over both pictures shown for it (the interpolated output and the
   game's own frame), so the screen shows the inputs instead of the result.
   - motion: the motion texture exactly as MetalFX reads it (g.mv): the direction of the vector to the previous position
     as hue (right red, up green-yellow, left cyan, down blue-violet), its length as brightness, full at 32 pixels of the
     motion texture per frame. With TOTK_OBJ_MOTION, pixels where the kernel took the object's own motion (the motion
     equals the per-object value there) are tinted lighter and outlined in white. Derived here from g.mv and the
     per-object target, so mfx_mv itself is unchanged.
   - depth: the G-buffer depth linearised with this frame's near / far (cam_projection), log scale from the near plane
     (white) to min(far, 4000 units) (black).
   - split: left third the picture, middle third motion, right third depth, white lines between.
   One render pass per target (load, one full-screen triangle, store) after MetalFX in the interpolation command buffer,
   or after the motion kernel in a command buffer of its own while interpolation is off (mfx_debug_only: drawn over the
   game's frame only, motion in pixels of the render size). Nothing is captured, encoded or allocated while the view is
   off. Frames without motion (first frame, camera cut, missing input) show the plain picture. */
static _Atomic int g_dbg = -1;
int mfx_debug_view(void) {
    int v = atomic_load_explicit(&g_dbg, memory_order_relaxed);
    if (v >= 0) return v;
    const char *e = getenv("TOTK_MFX_DEBUG_VIEW");
    v = !e || !*e || !strcmp(e, "0") || !strcmp(e, "off") ? 0 : !strcmp(e, "motion") || !strcmp(e, "1") ? 1 :
        !strcmp(e, "depth") || !strcmp(e, "2") ? 2 : !strcmp(e, "split") || !strcmp(e, "3") ? 3 : -1;
    if (v < 0) { MLOG("[red] TOTK_MFX_DEBUG_VIEW must be off, motion, depth or split, got %s: off", e); v = 0; }
    if (v) MLOG("TOTK_MFX_DEBUG_VIEW=%s: motion / depth drawn over the picture (with or without frame interpolation)", e);
    atomic_store(&g_dbg, v);
    return v;
}
void mfx_debug_view_set(int v) {
    static const char *const names[] = {"off", "motion", "depth", "split"};
    if (v < 0) v = (mfx_debug_view() + 1) % 4;
    if (v > 3) { MLOG("[flag] mfx_debug_view takes 0..3 (off, motion, depth, split; -1 cycles), got %d", v); return; }
    atomic_store(&g_dbg, v);
    MLOG("[flag] mfx_debug_view = %d: %s%s", v, names[v], v && !mfx_enabled() ? " (frame interpolation off: depth and camera captured for the view alone)" : "");
}
static id<MTLRenderPipelineState> dbg_pipeline(MTLPixelFormat fmt) {
    for (int i = 0; i < 3; i++) if (g.dbg_ps[i] && g.dbg_fmt[i] == fmt) return g.dbg_ps[i];
    static const char *src =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 p [[position]]; };\n"
        "vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
        // vp: the target rectangle (x, y, w, h); size: the motion / depth texture; da / db: 1 / view depth = db + (1 - d) * da;
        // dn: near plane; dlog: 1 / log2(farthest shown / near)
        "struct P { float4 vp; float2 size; float mode; float obj; float da; float db; float dn; float dlog; float2 oms; };\n"
        "static float3 hsv(float h, float v) { return v * clamp(abs(fract(h + float3(0.0, 2.0, 1.0) / 3.0) * 6.0 - 3.0) - 1.0, 0.0, 1.0); }\n"
        // the kernel took the object's own motion here: the motion equals it (up to half-float rounding). The per-object
        // target is at the render size (oms: its size over the motion size), read at the texel mfx_mv reads.
        "static bool objmv(texture2d<float, access::read> mv, texture2d<float, access::read> om, int2 q, float2 sz, float2 oms) {\n"
        "  uint2 c = uint2(clamp(q, int2(0), int2(sz) - 1));\n"
        "  float2 o = om.read(min(uint2((float2(c) + 0.5) * oms), uint2(om.get_width() - 1, om.get_height() - 1))).xy;\n"
        "  if (!all(abs(o) < 16.0)) return false;\n"
        "  float2 op = o * float2(0.5, -0.5) * sz; return length(mv.read(c).xy - op) <= 0.02 + 0.002 * length(op);\n"
        "}\n"
        "fragment float4 fs(V in [[stage_in]], texture2d<float, access::read> mv [[texture(0)]], depth2d<float, access::read> d [[texture(1)]],\n"
        "                   texture2d<float, access::read> om [[texture(2)]], constant P &p [[buffer(0)]]) {\n"
        "  float2 uv = (in.p.xy - p.vp.xy) / p.vp.zw; int view = int(p.mode);\n"
        "  if (view == 3) {\n"
        "    float px = 1.0 / p.vp.z;\n"
        "    if (abs(uv.x - 1.0 / 3.0) < 1.5 * px || abs(uv.x - 2.0 / 3.0) < 1.5 * px) return float4(1.0);\n"
        "    if (uv.x < 1.0 / 3.0) discard_fragment();\n"
        "    view = uv.x < 2.0 / 3.0 ? 1 : 2;\n"
        "  }\n"
        "  int2 q = int2(clamp(uv, 0.0, 0.99999) * p.size);\n"
        "  if (view == 2) {\n"
        "    float z = d.read(uint2(q)); float inv = p.db + (1.0 - z) * p.da;\n"
        "    float v = inv > 0.0 ? 1.0 - saturate(log2(max(1.0 / (inv * p.dn), 1.0)) * p.dlog) : 0.0;\n"
        "    return float4(v, v, v, 1.0);\n"
        "  }\n"
        "  float2 m = mv.read(uint2(q)).xy; float mag = length(m);\n"
        "  float3 c = mag > 1e-4 ? hsv(fract(atan2(-m.y, m.x) / 6.2831853), saturate(mag / 32.0)) : float3(0.0);\n"
        "  if (p.obj > 0.5) {\n"
        "    bool u = objmv(mv, om, q, p.size, p.oms);\n"
        "    bool edge = u != objmv(mv, om, q + int2(1, 0), p.size, p.oms) || u != objmv(mv, om, q - int2(1, 0), p.size, p.oms) ||\n"
        "                u != objmv(mv, om, q + int2(0, 1), p.size, p.oms) || u != objmv(mv, om, q - int2(0, 1), p.size, p.oms);\n"
        "    if (edge) c = float3(1.0); else if (u) c = mix(c, float3(1.0), 0.2);\n"
        "  }\n"
        "  return float4(c, 1.0);\n"
        "}\n";
    NSError *e = nil;
    id<MTLLibrary> lib = [g.dev newLibraryWithSource:@(src) options:nil error:&e];
    if (!lib) { MLOG("[red] debug view shader: %s", e.localizedDescription.UTF8String); return nil; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
    pd.colorAttachments[0].pixelFormat = fmt;
    id<MTLRenderPipelineState> ps = [g.dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!ps) { MLOG("[red] debug view pipeline (format %lu): %s", (unsigned long)fmt, e.localizedDescription.UTF8String); return nil; }
    int k = 0; while (k < 2 && g.dbg_ps[k]) k++;   // the output view's format and the swapchain's; a third replaces the last
    g.dbg_ps[k] = ps; g.dbg_fmt[k] = fmt;
    if (!g.dbg_args[0]) {
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1; ad.maxTextureBindCount = 3;
        for (int i = 0; i < MFX_OUT; i++) {
            g.dbg_args[i] = [g.dev newArgumentTableWithDescriptor:ad error:&e]; if (!g.dbg_args[i]) MDIE("argument table: %s", e.localizedDescription.UTF8String);
            g.dbg_par[i] = [g.dev newBufferWithLength:256 options:MTLResourceStorageModeShared]; [g.rs addAllocation:g.dbg_par[i]];
        }
        [g.rs commit];
    }
    MLOG("debug view pipeline for format %lu", (unsigned long)fmt);
    return ps;
}
/* The view drawn over each of the n targets (crop applies to the last one, the game's frame) with the motion / depth /
   per-object textures of slot s; what the GPU uses is appended to res. 0: no pipeline or encoder. */
static int dbg_draw(id<MTL4CommandBuffer> cb, int s, int view, id<MTLTexture> depth, id<MTLTexture> om_used, id<MTLTexture> const *targets, int n,
                    const int crop[4], NSMutableArray *res) {
    id<MTLTexture> om = om_used ? om_used : g.mv;
    double nr = g.near_p > 0 ? g.near_p : 0.1, f = g.far_p > nr ? g.far_p : 1e7, shown = fmin(f, 4000.0);
    [res addObjectsFromArray:@[depth, g.mv, om]];
    for (int k = 0; k < n; k++) {
        id<MTLTexture> t = targets[k]; if (!t) continue;
        double x = 0, y = 0, w = t.width, h = t.height;
        if (k == n - 1 && crop && crop[2] > 0 && crop[3] > 0 && crop[0] >= 0 && crop[1] >= 0 &&
            (NSUInteger)(crop[0] + crop[2]) <= t.width && (NSUInteger)(crop[1] + crop[3]) <= t.height) { x = crop[0]; y = crop[1]; w = crop[2]; h = crop[3]; }
        id<MTLRenderPipelineState> ps = dbg_pipeline(t.pixelFormat); if (!ps) return 0;
        float *p = (float *)((char *)g.dbg_par[s].contents + k * 128);
        p[0] = (float)x; p[1] = (float)y; p[2] = (float)w; p[3] = (float)h; p[4] = (float)g.mv.width; p[5] = (float)g.mv.height;
        p[6] = (float)view; p[7] = om_used ? 1.0f : 0.0f;
        p[12] = (float)om.width / g.mv.width; p[13] = (float)om.height / g.mv.height;   // the per-object target's scale
        p[8] = (float)((f - nr) / (nr * f)); p[9] = (float)(1.0 / f); p[10] = (float)nr; p[11] = (float)(1.0 / log2(shown / nr > 2 ? shown / nr : 2));
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
        rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionLoad; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTL4RenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        if (!e) { MLOG("[red] debug view: no render encoder"); return 0; }
        [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];   // after MetalFX / the motion kernel and the game's frame
        id<MTL4ArgumentTable> a = g.dbg_args[s];
        [a setTexture:g.mv.gpuResourceID atIndex:0]; [a setTexture:depth.gpuResourceID atIndex:1]; [a setTexture:om.gpuResourceID atIndex:2];
        [a setAddress:g.dbg_par[s].gpuAddress + (uint64_t)k * 128 atIndex:0];
        [e setArgumentTable:a atStages:MTLRenderStageFragment];
        [e setViewport:(MTLViewport){x, y, w, h, 0, 1}]; [e setRenderPipelineState:ps];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
        [res addObject:t];
    }
    [res addObject:g.dbg_par[s]];
    static uint64_t said; if (!said++) MLOG("debug view %d drawn (motion / depth %lux%lu, near %g, farthest shown %g)", view, (unsigned long)depth.width, (unsigned long)depth.height, nr, shown);
    return 1;
}
NSArray *mfx_debug_encode(id<MTL4CommandBuffer> cb, int s, id<MTLTexture> present, const int crop[4]) {
    int view = atomic_load_explicit(&g_dbg, memory_order_relaxed);
    if (view < 0) view = mfx_debug_view();
    if (!view || s < 0 || s >= MFX_OUT || !g.last_depth || !g.mv || !g.out[s] || !g.outv[s] || g.last_slot != s) return nil;
    id<MTLTexture> targets[2] = {g.outv[s], present};
    NSMutableArray *res = [NSMutableArray arrayWithObjects:g.out[s], nil];
    return dbg_draw(cb, s, view, g.last_depth, g.last_om, targets, present ? 2 : 1, crop, res) ? res : nil;
}
/* The debug view with frame interpolation off (mfx_debug_only): nvn_mtl.m still takes the G-buffer depth and the camera
   (mfx_pass_setup without the UI layer, the camera capture) and the per-object motion with TOTK_OBJ_MOTION; the motion
   kernel runs at the depth's size into g.mv in a command buffer of its own after the game's frame, and the view is drawn
   over the game's frame. No interpolator, scene / UI copies or output textures are made: only the motion texture and the
   previous depth (8 bytes per render pixel, about 12 MB at 1600x900), plus the per-object target and position arenas with
   TOTK_OBJ_MOTION. They are retired when the view is turned off (mfx_debug_release) and remade at their own sizes when
   interpolation turns on (ensure_out / ensure_fi). Above MFX_BP_HIGH percent of the GPU memory budget nothing is made or
   drawn (and what was made is released). Off: nothing is captured, made or encoded. */
int mfx_debug_only(void) {
    if (mfx_enabled()) return 0;
    int view = atomic_load_explicit(&g_dbg, memory_order_relaxed);
    if (view < 0) view = mfx_debug_view();
    if (view && g_dbg_mem) {   // held off near the budget until the use at the last present is under MFX_BP_LOW percent
        if (g_mem_budget && g_mem_used >= g_mem_budget / 100 * MFX_BP_LOW) return 0;
        g_dbg_mem = 0; MLOG("back under %d%% of the GPU memory budget: the debug view is drawn again", MFX_BP_LOW);
    }
    return view;
}
int mfx_debug_release(void) {
    if (!g_dbg_live || mfx_enabled()) return 0;
    g_dbg_live = 0;
    uint64_t b = obj_bytes(g.mv) + obj_bytes(g.pdepth) + obj_bytes(g.obj);
    drop(&g.mv); drop(&g.pdepth); drop(&g.obj); g.pdepth_frame = 0;
    g.depth = nil; g.enc_depth = nil; g.last_depth = nil; g.last_om = nil; g.prev_ok = g.hist_ok = 0; g.obj_ok = g.cam_ok = 0;
    MLOG("debug view without interpolation off: %llu MB of motion / depth textures retired (released after %d presents)", (unsigned long long)(b >> 20), MFX_RETIRE_TICKS);
    return 1;
}
int mfx_debug_prepare(void) {
    if (mfx_enabled()) return -1;
    g.frames++;
    id<MTLTexture> depth = g.depth;
    int obj = g.obj_ok && mfx_obj_motion_on() && g.obj && depth && g.obj.width == depth.width && g.obj.height == depth.height;
    int cam_ok = g.cam_ok;
    g.obj_ok = 0; g.cam_ok = 0; g.depth = nil;
    if (g_mem_budget && g_mem_used > g_mem_budget / 100 * MFX_BP_HIGH) {
        MLOG("near the GPU memory budget (%llu of %llu MB): the debug view is not drawn until the use is under %d%%", (unsigned long long)(g_mem_used >> 20), (unsigned long long)(g_mem_budget >> 20), MFX_BP_LOW);
        g_dbg_mem = 1; g.prev_ok = 0;
        return -1;   // mfx_debug_only is 0 from now on: the caller releases (mfx_debug_release, the position arenas)
    }
    g_dbg_live = 1;   // depth / camera capture and per-object motion run from here on: mfx_debug_release ends them
    if (!cam_ok || !depth) { g.prev_ok = 0; return -1; }
    if (!g.dev) g.dev = depth.device;   // set by ensure_out / mfx_obj_target otherwise, neither of which may have run
    if (!g.mv || g.mv.width != depth.width || g.mv.height != depth.height) {
        drop(&g.mv); g.mv = mk(MTLPixelFormatRG16Float, depth.width, depth.height, MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
        drop(&g.pdepth); g.pdepth = mk(MTLPixelFormatDepth32Float, depth.width, depth.height, MTLTextureUsageShaderRead); g.pdepth_frame = 0;
        mv_pipeline();
        g.prev_ok = 0;
        MLOG("debug view without frame interpolation: motion / depth %lux%lu", (unsigned long)depth.width, (unsigned long)depth.height);
    }
    if (!g.prev_ok || camera_cut()) { memcpy(g.prev_cam, g.cam, sizeof g.cam); g.prev_ok = 1; return -1; }
    double m[16];
    if (!mv_reproj(m)) { g.prev_ok = 0; return -1; }
    int s = g.slot = (g.slot + 1) % MFX_OUT;
    mv_params(s, m, depth, obj);
    g.enc_depth = depth;
    return s;
}
void mfx_debug_abort(void) { g.enc_depth = nil; g.prev_ok = 0; }
NSArray *mfx_debug_frame_encode(id<MTL4CommandBuffer> cb, int s, id<MTLTexture> present, const int crop[4]) {
    id<MTLTexture> depth = g.enc_depth; g.enc_depth = nil;
    int view = mfx_debug_only();
    if (!depth || !present || !view || s < 0 || s >= MFX_OUT || !g.mv) { g.prev_ok = 0; return nil; }
    if (g.rs) [cb useResidencySet:g.rs];
    id<MTLTexture> om = mv_encode(cb, s, depth);
    id<MTLTexture> om_used = g.enc_obj && g.obj ? g.obj : nil;
    NSMutableArray *res = [NSMutableArray arrayWithObjects:depth, g.mv, g.pdepth, om, g.mv_par[s], nil];
    memcpy(g.prev_cam, g.cam, sizeof g.cam);
    dbg_draw(cb, s, view, depth, om_used, &present, 1, crop, res);   // on failure the motion kernel's resources still go with the command buffer
    return res;
}
/* TOTK_MFX_MVLOG=<every n frames>: read the motion texture back after the interpolation and log its median and the
   camera step (diagnostic; waits for the GPU). */
void mfx_mv_log(void) {
    static int every = -1; if (every < 0) { const char *e = getenv("TOTK_MFX_MVLOG"); every = e ? atoi(e) : 0; }
    if (every <= 0 || !g.mv || g.made % (uint64_t)every) return;
    static id<MTLCommandQueue> q; if (!q) q = [g.dev newCommandQueue];
    NSUInteger w = g.mv.width, h = g.mv.height;
    id<MTLBuffer> b = [g.dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:g.mv sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const _Float16 *v = b.contents; enum { N = 4096 }; static float xs[N], ys[N]; int n = 0;
    for (NSUInteger y = h / 8; y < h * 7 / 8 && n < N; y += h / 64) for (NSUInteger x = w / 8; x < w * 7 / 8 && n < N; x += w / 64) { xs[n] = (float)v[(y * w + x) * 2]; ys[n] = (float)v[(y * w + x) * 2 + 1]; n++; }
    for (int i = 1; i < n; i++) { float t = xs[i]; int j = i - 1; while (j >= 0 && xs[j] > t) { xs[j + 1] = xs[j]; j--; } xs[j + 1] = t; }
    for (int i = 1; i < n; i++) { float t = ys[i]; int j = i - 1; while (j >= 0 && ys[j] > t) { ys[j + 1] = ys[j]; j--; } ys[j + 1] = t; }
    double yaw = atan2(g.cam[2], g.cam[0]) * 180 / M_PI, pyaw = atan2(g.prev_cam[2], g.prev_cam[0]) * 180 / M_PI;   // prev_cam is this frame's camera after encode
    MLOG("motion (input px, median of %d samples): x %.2f (p10 %.2f p90 %.2f) y %.2f; camera yaw %.3f deg, step to previous %.3f deg; position %.2f %.2f %.2f",
         n, xs[n / 2], xs[n / 10], xs[n * 9 / 10], ys[n / 2], yaw, yaw - g.mv_prev_yaw, g.cam[47], g.cam[51], g.cam[55]);
    (void)pyaw;
}
/* TOTK_MFX_DUMP=<dir>: while the camera turns (more than 1 degree between the two frames), save the previous and this
   frame's scene, the depth, the motion vectors and the interpolated output of the 30th, 60th and 90th such frame as raw
   files (scene RGBA16F, depth float32, motion 2 x float16, output RGBA8 sRGB), with a line in the log. Diagnostic: it
   waits for the GPU and is meant for unpaced runs. */
static void dump_tex(id<MTLCommandQueue> q, id<MTLTexture> t, NSUInteger bpp, const char *dir, const char *name, uint64_t n) {
    NSUInteger w = t.width, h = t.height;
    id<MTLBuffer> b = [g.dev newBufferWithLength:w * h * bpp options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:w * bpp destinationBytesPerImage:w * h * bpp];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    char path[1024]; snprintf(path, sizeof path, "%s/mfx-%llu-%s-%lux%lu.bin", dir, (unsigned long long)n, name, (unsigned long)w, (unsigned long)h);
    FILE *f = fopen(path, "wb"); if (f) { fwrite(b.contents, 1, w * h * bpp, f); fclose(f); } else MLOG("[red] dump: cannot write %s", path);
}
static const char *g_dump_dir; static uint64_t g_turning; static double g_dump_step;
int mfx_dump_due(void) {   // called after each encode; 1 = dump this frame (the caller first waits for the GPU)
    static int init; if (!init) { init = 1; g_dump_dir = getenv("TOTK_MFX_DUMP"); }
    if (!g_dump_dir || !g.last_depth || !g.mv || g.last_slot < 0) return 0;
    g_dump_step = fabs(remainder(atan2(g.prev_cam[2], g.prev_cam[0]) * 180 / M_PI - g.mv_prev_yaw, 360.0));
    if (g_dump_step < 1.0) return 0;
    g_turning++;
    return 1;
}
/* After the GPU has finished: compare the centre of the previous and this frame's scene; a very different pair (or the
   30th turning frame as a reference) is dumped with the presented images of both frames. Returns 1 when dumped. */
static float uf11(uint32_t v) { uint32_t e = (v >> 6) & 31, m = v & 63; return e == 0 ? ldexpf(m / 64.0f, -14) : e == 31 ? 65504.0f : ldexpf(1 + m / 64.0f, (int)e - 15); }
static float uf10(uint32_t v) { uint32_t e = (v >> 5) & 31, m = v & 31; return e == 0 ? ldexpf(m / 32.0f, -14) : e == 31 ? 65504.0f : ldexpf(1 + m / 32.0f, (int)e - 15); }
static void scene_rgb(const void *base, NSUInteger i, float c[3]) {   // one pixel of a scene copy (RG11B10F or RGBA16F)
    if (g.scene_fmt == MTLPixelFormatRGBA16Float) { const _Float16 *p = (const _Float16 *)base + i * 4; for (int k = 0; k < 3; k++) c[k] = (float)p[k]; return; }
    uint32_t v = ((const uint32_t *)base)[i]; c[0] = uf11(v & 0x7ff); c[1] = uf11((v >> 11) & 0x7ff); c[2] = uf10(v >> 22);
}
static double centre_diff(id<MTLCommandQueue> q) {
    NSUInteger w = 320, h = 180, x0 = (g.out_w - w) / 2, y0 = (g.out_h - h) / 2, bpp = g.scene_fmt == MTLPixelFormatRGBA16Float ? 8 : 4;
    id<MTLBuffer> b = [g.dev newBufferWithLength:w * h * bpp * 2 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    for (int k = 0; k < 2; k++) [e copyFromTexture:g.scene[g.cur ^ k] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x0, y0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:b destinationOffset:k * w * h * bpp destinationBytesPerRow:w * bpp destinationBytesPerImage:w * h * bpp];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const uint8_t *p = b.contents; double se = 0;
    for (NSUInteger i = 0; i < w * h; i++) { float a[3], c[3]; scene_rgb(p, i, a); scene_rgb(p + w * h * bpp, i, c);
        for (int k = 0; k < 3; k++) { double d = fmin(fmax(a[k], 0), 4) - fmin(fmax(c[k], 0), 4); se += d * d; } }
    return se / (w * h * 3);
}
void mfx_dump(long frame, id<MTLTexture> swap_prev, id<MTLTexture> swap_cur) {
    const char *dir = g_dump_dir; uint64_t turning = g_turning; double step = g_dump_step;
    static id<MTLCommandQueue> q; if (!q) q = [g.dev newCommandQueue];
    // every 10th turning frame from the 10th to the 160th (sixteen samples); very different pairs are logged and kept too
    double mse = centre_diff(q);
    if (turning % 10 || turning < 10 || turning > 160) return;
    MLOG("frame pair %llu (game frame %ld): centre mean squared difference %.4f%s", (unsigned long long)turning, frame, mse, mse > 0.05 ? " -- very different" : "");
    if (swap_prev) dump_tex(q, swap_prev, 4, dir, "swapprev", turning);
    if (swap_cur) dump_tex(q, swap_cur, 4, dir, "swapcur", turning);
    // after encode, cur was flipped: scene[cur ^ 1] is this frame, scene[cur] the previous one
    NSUInteger sb = g.scene_fmt == MTLPixelFormatRGBA16Float ? 8 : 4;   // RG11B10F canvas copies: 4 bytes
    dump_tex(q, g.scene[g.cur], sb, dir, "prev", turning); dump_tex(q, g.scene[g.cur ^ 1], sb, dir, "cur", turning);
    dump_tex(q, g.last_depth, 4, dir, "depth", turning); dump_tex(q, g.mv, 4, dir, "motion", turning); dump_tex(q, g.out[g.last_slot], 4, dir, "out", turning);
    MLOG("dump %llu (game frame %ld): yaw step %.3f deg, camera %.3f %.3f %.3f -> %s", (unsigned long long)turning, frame, step, g.cam[47], g.cam[51], g.cam[55], dir);
}
id<MTLTexture> mfx_output(int slot) { return slot >= 0 && slot < MFX_OUT ? g.outv[slot] : nil; }
void mfx_note_gpu(double ms) { if (ms > 0 && ms < 100) g.gpu_ms[g.ngpu++ % 300] = ms; }
static double gpu_pct(double q) {
    int n = g.ngpu < 300 ? g.ngpu : 300; if (!n) return -1;
    double a[300]; memcpy(a, g.gpu_ms, sizeof a);
    for (int i = 1; i < n; i++) { double v = a[i]; int j = i - 1; while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; }
    return a[(int)(q * (n - 1))];
}
/* memreport (input file, diagnostic): what interpolation holds now. Interpolators report no allocatedSize, so only their
   number and output sizes are given. */
void mfx_mem_report(char *out, size_t n) {
    uint64_t live = 0; for (int i = 0; i < 2; i++) live += obj_bytes(g.scene[i]);
    live += obj_bytes(g.ui) + obj_bytes(g.ui2) + obj_bytes(g.mv) + obj_bytes(g.pdepth) + obj_bytes(g.obj) + obj_bytes(g.dz);
    for (int i = 0; i < MFX_OUT; i++) live += obj_bytes(g.out[i]);
    uint64_t rb = 0; int rfi = 0; for (int i = 0; i < g_nret; i++) { rb += g_ret[i].bytes; rfi += [g_ret[i].obj conformsToProtocol:@protocol(MTL4FXFrameInterpolator)]; }
    char fis[256] = ""; size_t k = 0;
    for (unsigned i = 0; i < g_fi_cache_count && k < sizeof fis; i++)
        k += (size_t)snprintf(fis + k, sizeof fis - k, "%s%lux%lu->%lux%lu", i ? ", " : "", (unsigned long)g_fi_cache[i].inputWidth, (unsigned long)g_fi_cache[i].inputHeight,
                              (unsigned long)g_fi_cache[i].outputWidth, (unsigned long)g_fi_cache[i].outputHeight);
    snprintf(out, n, "mode %d (paused %d), textures %llu MB at %lux%lu, current interpolator %s, cached interpolators %u (about %llu MB) [%s], retired %d objects %llu MB (%d interpolators); interpolators made %llu (+%llu MB at creation), released but still alive %lu",
             atomic_load(&g_on), g_bp_paused, (unsigned long long)(live >> 20), (unsigned long)g.out_w, (unsigned long)g.out_h, g.fi ? (is_cached(g.fi) ? "cached" : "uncached") : "none",
             g_fi_cache_count, (unsigned long long)(fi_cache_bytes() >> 20), fis, g_nret, (unsigned long long)(rb >> 20), rfi, (unsigned long long)g_fi_made, (unsigned long long)(g_fi_made_bytes >> 20), (unsigned long)g_fi_gone.allObjects.count);
}
void mfx_stats(char *out, size_t n) {
    snprintf(out, n, "interpolated frames %llu (with per-object motion %llu) / game frames %llu (MetalFX GPU p50 %.2f / p95 %.2f ms; frame interval %.1f ms); not interpolated: missing input %llu (colour %llu camera %llu depth %llu UI %llu), first frame / history reset %llu, camera cut %llu, command buffer busy %llu",
             (unsigned long long)g.made, (unsigned long long)g.obj_frames, (unsigned long long)g.frames, gpu_pct(0.5), gpu_pct(0.95), g.dt * 1e3, (unsigned long long)g.skip_input, (unsigned long long)g.miss[0], (unsigned long long)g.miss[1],
             (unsigned long long)g.miss[2], (unsigned long long)g.miss[3],
             (unsigned long long)g.skip_first, (unsigned long long)g.skip_cut, (unsigned long long)g.skip_busy);
}

/* Off: every texture / interpolator of ours is retired (released MFX_RETIRE_TICKS presents later, logged there). The
   interpolator cache is kept for a toggle (key 2) unless all: rebuilding identical interpolators grows device memory
   (see MFX_FI_CACHE), and a pause near the budget is about memory. */
static void mfx_release(int all) {
    uint64_t b = 0;
    for (int i = 0; i < 2; i++) { b += obj_bytes(g.scene[i]); drop(&g.scene[i]); }
    b += obj_bytes(g.ui) + obj_bytes(g.ui2); drop(&g.ui); drop(&g.ui2);
    for (int i = 0; i < MFX_OUT; i++) { b += obj_bytes(g.out[i]); drop(&g.out[i]); g.outv[i] = nil; }
    b += obj_bytes(g.mv); drop(&g.mv); b += obj_bytes(g.obj); drop(&g.obj); b += obj_bytes(g.pdepth); drop(&g.pdepth); g.pdepth_frame = 0; b += obj_bytes(g.dz); drop(&g.dz);
    if (g.fi && !is_cached(g.fi)) retire(g.fi);
    unsigned n = all ? fi_cache_evict(1) : 0;
    g.fi = nil; g.last_depth = nil; g.last_om = nil; g.last_slot = -1; g.in_w = g.in_h = 0; g.out_w = g.out_h = 0; g.depth = nil; g.enc_depth = nil;
    g.prev_ok = g.hist_ok = 0; g.obj_ok = 0;
    MLOG("interpolation off: %llu MB of textures and %u cached interpolators retired (released after %d presents)", (unsigned long long)(b >> 20), n, MFX_RETIRE_TICKS);
}
/* GPU memory budget (nvn_mtl.m gpu_budget_check stops the game past it): frame interpolation and per-object motion are
   optional, so above MFX_BP_HIGH percent of the budget they are paused first (the requested mode becomes off at the next
   present, mfx_latch releases what they hold) and resumed once the use plus what the pause freed is under MFX_BP_LOW
   percent. After MFX_BP_MAX pauses they stay off for the session (each rebuild costs memory; key 2 turns them on again). */
static int g_bp_saved, g_bp_count, g_bp_measured, g_bp_said;
static uint64_t g_bp_tick, g_bp_used_at, g_bp_freed;
static void bp_pause(uint64_t used, uint64_t budget, const char *why) {
    int on = atomic_load(&g_on), req = atomic_load(&g_req);
    if (g_bp_paused || (!on && !req)) return;
    g_bp_paused = 1; g_bp_saved = req ? req : on; g_bp_count++; g_bp_tick = g_tick; g_bp_used_at = used; g_bp_measured = 0; g_bp_freed = 0;
    atomic_store(&g_req, MFX_OFF);
    MLOG("near the GPU memory budget: interpolation paused (%s: %llu of %llu MB, %llu%%; per-object motion too; resumes under %d%%)",
         why, (unsigned long long)(used >> 20), (unsigned long long)(budget >> 20), (unsigned long long)(used * 100 / budget), MFX_BP_LOW);
}
/* Rough GPU memory of interpolation per output pixel, from the textures it makes (ensure_out, ensure_fi): two scene
   copies (up to RGBA16Float, 16 B), the UI layer (RGBA16Float, 8 B), MFX_OUT RGBA8 outputs (12 B), the motion texture
   (RG16Float at the render size, at most 4 B) and the interpolator's own history, which Metal does not report (assumed
   about two RGBA16Float textures, 16 B). About 0.45 GB at 4K, 0.11 GB at 1080p. TOTK_OBJ_MOTION adds its position
   arenas and its RGBA16Float target (render size, at most 8 B per output pixel). */
#define MFX_BYTES_PER_PX 56
unsigned long long mfx_need_bytes(unsigned w, unsigned h) {
    if (!w || !h) { w = g_last_w; h = g_last_h; }
    unsigned long long b = (unsigned long long)w * h * MFX_BYTES_PER_PX;
    if (b && mfx_obj_motion_on()) b += (3ull * 64 << 20) + (unsigned long long)w * h * 8;   // three position arenas of TOTK_OBJ_MOTION_MB (64 MiB to start), the motion target
    return b;
}
/* mfx_latch, off -> mode: the use at the last present plus the estimate above MFX_BP_HIGH percent of the budget: the
   mode is held like a pause (resumed on its own once the use plus the estimate is under MFX_BP_LOW percent) without
   allocating anything. Unknown output size (never ran): not checked here, the pause at the present still applies. */
static int mfx_refuse(int mode) {
    uint64_t need = mfx_need_bytes(0, 0), used = g_mem_used, budget = g_mem_budget;
    if (!budget || !need || used + need <= budget / 100 * MFX_BP_HIGH) return 0;
    g_bp_paused = 1; g_bp_saved = mode; g_bp_count++; g_bp_tick = g_tick; g_bp_used_at = used; g_bp_measured = 1; g_bp_freed = need;
    atomic_store(&g_req, MFX_OFF);
    MLOG("not enough GPU memory for interpolation: %s not turned on (%llu of %llu MB in use, about %llu MB needed; over %d%%)",
         mfx_mode_name(mode), (unsigned long long)(used >> 20), (unsigned long long)(budget >> 20), (unsigned long long)(need >> 20), MFX_BP_HIGH);
    return 1;
}
int mfx_interp_state(void) {
    pthread_once(&g_once, mfx_init);
    if (g_bp_paused) return !g_applied ? MFX_STATE_REFUSED_MEMORY : g_bp_count >= MFX_BP_MAX ? MFX_STATE_OFF_MEMORY : MFX_STATE_PAUSED_MEMORY;
    return atomic_load(&g_on) || atomic_load(&g_req) ? MFX_STATE_ON : MFX_STATE_OFF;
}
void mfx_present_tick(uint64_t used, uint64_t budget) {
    pthread_once(&g_once, mfx_init);
    g_tick++; g_mem_used = used; g_mem_budget = budget;
    if (g_nret) retire_drain(0);
    if (!budget) return;
    if (!g_bp_paused) { if (used > budget / 100 * MFX_BP_HIGH) bp_pause(used, budget, "present"); return; }
    if (!g_bp_measured && !g_nret && g_tick - g_bp_tick > MFX_RETIRE_TICKS + 2) {   // what the pause gave back
        g_bp_measured = 1; g_bp_freed = g_bp_used_at > used ? g_bp_used_at - used : 0;
        MLOG("the pause freed about %llu MB of GPU memory (%llu MB now)", (unsigned long long)(g_bp_freed >> 20), (unsigned long long)(used >> 20));
    }
    if (!g_bp_measured || used + g_bp_freed >= budget / 100 * MFX_BP_LOW) return;
    if (g_bp_count >= MFX_BP_MAX) { if (!g_bp_said++) MLOG("paused %d times near the GPU memory budget: interpolation stays off for this session (key 2 turns it on again)", g_bp_count); return; }
    g_bp_paused = 0; atomic_store(&g_req, g_bp_saved);
    MLOG("back under %d%% of the GPU memory budget (%llu of %llu MB, %llu MB expected back): interpolation resumed, %s",
         MFX_BP_LOW, (unsigned long long)(used >> 20), (unsigned long long)(budget >> 20), (unsigned long long)(g_bp_freed >> 20), mfx_mode_name(g_bp_saved));
}
/* gpu_budget_check over the budget (by at most 5%): 1 while the optional features are being released (the pause starts
   here if it had not), so the overrun they cause does not stop the game; 0: nothing optional left, the check fails. */
int mfx_budget_grace(uint64_t used, uint64_t budget) {
    pthread_once(&g_once, mfx_init);
    if (!g_bp_paused) { if (!atomic_load(&g_on) && !atomic_load(&g_req)) return 0; bp_pause(used, budget, "over the budget"); return 1; }
    return g_tick - g_bp_tick <= MFX_RETIRE_TICKS + 30;
}
#endif  // TARGET_OS_VISION
