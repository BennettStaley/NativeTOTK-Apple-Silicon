/* Included only by nvn_mtl.m (before draw_inner). TOTK picture settings (settings menu), switchable while playing:
   - "aa_mode" (TOTK_AA): 0 the game's own FXAA (default), 1 off, 2 our FXAA, 3 SMAA 1x. The game's FXAA is one full-screen
     draw (1.4.3: fragment shader 22a61cb1, FXAA 3.11 console) from the tone-mapped picture into its own target at the
     render size, before the game's upscale (a3a570eb / ac58fc18, or MetalFX) and the UI. For modes 1-3 that draw is not
     encoded; our passes read the same input and write the same target over the same viewport / scissor (dynamic
     resolution keeps working), so everything after it, the UI included, is unchanged. Handheld (1.4.3, also every
     iPhone / iPad) has no FXAA and no upscale: the tone map 0ac9a549 writes the canvas directly and the UI follows. A
     frame without the game's FXAA draw gets modes 2 / 3 on the canvas instead, before its first UI draw (pp_ui_hook),
     and mode 1 is the game's own picture. Other game versions: the key of their FXAA draw has to be added to
     pp_is_fxaa (docked they would otherwise get our AA on top of theirs).
   - "sharpen" (TOTK_SHARPEN): 0..100, FidelityFX CAS on the canvas after the upscale (and after the canvas AA), at the
     first UI draw into it (the hook TOTK_MFX uses); a frame without UI before the canvas is copied to the window.
   - "aniso" (TOTK_ANISO): 0 the game's, 2/4/8/16 at least that much anisotropy for mipmapped, linear, repeating samplers
     without compare / min-max reduction; "lod_bias" (TOTK_LOD_BIAS): -100..0 hundredths added to every sampler's LOD
     bias. The game's own LOD bias is passed to Metal now as well (it was dropped; TOTK_GAME_LOD_BIAS=0 leaves it out).
     Changing either drops every built sampler object (released after the GPU is done with it) and the next use builds
     it again (samp_by_id).
   - "hide_hud" (Interface: Hide the HUD, F2 on the Mac): every draw into the canvas with one of the UI vertex shaders
     (uw_ui_vs) from the frame's first UI draw on runs with its colour writes masked off: hearts, map, prompts, and the
     menus too (the inventory's 3D scene, drawn with the tone map 0ac9a549, stays).
   - "dof" (Depth of field): 0 leaves out the game's depth-of-field composite 18b730fa: a full-screen draw that reads the
     depth, picks a level of the half-resolution blurred copy of the scene (34070de6) by distance and blends it over the
     scene (source alpha); without it the scene stays as drawn, sharp.
   - "bloom" (Bloom): 0..100, the bloom's share. The tone map 0ac9a549 adds its texture 1 (the bloom chain's result,
     bright pass 79b89ff1, blurred downsamples and the upsamples 327ae8bb) to the scene; below 100 that texture is
     multiplied in place first, every level, by a blend-only pass of ours (pp_zero, zero + target * strength). The chain
     rewrites it every frame.
   - "lens_flare" (Lens flare): 0 leaves out the game's lens flare. With the sun in view (and only then) the game first
     adds 25 points into a 1x1 target (4c207255 / 85305c25: how much of the sun is visible), then draws about 11 sprites
     additively (src alpha, one) into the scene at the render size, after the bloom chain and before the tone map
     0ac9a549: d9793c43 / 675a28d1, 1509a8d8 / 452f4360 and 9e8525f6 / 452f4360 (the rings and ghosts), 1509a8d8 /
     30bf73e3 (the glint). Those draws are not encoded; the visibility count and the exposure dispatches in the same pass
     are kept. Found 2026-10-20 with the draw log: looking at the sun on Great Sky Island against looking away.
   dm_gfx_set_flag only records a request (any thread); pp_latch takes it at the game's present on the replay thread, so
   a frame is drawn with one setting throughout. */
#include "totk_post_msl.h"
#include "smaa_tables.h"

enum { PP_COPY, PP_FXAA, PP_CAS, PP_EDGE, PP_WEIGHT, PP_BLEND, PP_UI_SQUEEZE, PP_UI_OVER, PP_NFN };   // PP_UI_*: totk_ui_layer.h
static const char *const k_pp_fn[PP_NFN] = {"pp_copy", "pp_fxaa", "pp_cas", "smaa_edge", "smaa_weight", "smaa_blend", "ui_squeeze", "ui_over"};
static const char *const k_pp_aa_name[4] = {"game FXAA", "off", "FXAA", "SMAA 1x"};
static struct {
    id<MTLLibrary> lib; int lib_failed;
    id<MTLRenderPipelineState> ps[PP_NFN][4]; MTLPixelFormat pf[PP_NFN][4];
    id<MTL4ArgumentTable> args[PP_NFN];
    id<MTLTexture> area, search, edges, blend, cas_src;
    __weak id<MTLTexture> canvas;   // this frame's canvas, once drawn into (CAS fallback for a frame without UI)
    int ui_done, fxaa_seen; MTLClearColor cc;   // cc: a clear the game had pending for the FXAA target (pp_pass clear 2)
    uint64_t aa_n, aa_kept[6], cas_n, canvas_aa_n, frames, last_aa_n;
    id<MTLRenderPipelineState> scale_ps[4]; MTLPixelFormat scale_pf[4];   // pp_zero with the multiplying blend (bloom strength)
    uint64_t hud_n, dof_n, bloom_n, bloom_kept, flare_n;
    int hud_diag; uint64_t hud_diag_frame;   // Hide the HUD diagnostic: draws left to log after a toggle to hidden
} g_postfx;

static int pp_flag(const char *name, int v) {
    if (!strcmp(name, "aa_mode")) {
        if (v < 0 || v > 3) { MLOG("[flag] aa_mode requires 0..3"); return 1; }
        atomic_store(&g_pp_req_aa, v); MLOG("[flag] aa_mode = %d (%s, from the next frame)", v, k_pp_aa_name[v]); return 1;
    }
    if (!strcmp(name, "sharpen")) {
        if (v < 0 || v > 100) { MLOG("[flag] sharpen requires 0..100"); return 1; }
        atomic_store(&g_pp_req_sharpen, v); MLOG("[flag] sharpen = %d (from the next frame)", v); return 1;
    }
    if (!strcmp(name, "aniso")) {
        if (v == 1) v = 0;
        if (v != 0 && v != 2 && v != 4 && v != 8 && v != 16) { MLOG("[flag] aniso requires 0, 2, 4, 8 or 16"); return 1; }
        atomic_store(&g_pp_req_aniso, v); MLOG("[flag] aniso = %d (samplers rebuilt at the next frame)", v); return 1;
    }
    if (!strcmp(name, "hide_hud") || !strcmp(name, "dof") || !strcmp(name, "lens_flare")) {
        _Atomic int *r = name[0] == 'h' ? &g_fx_req_hud : name[0] == 'd' ? &g_fx_req_dof : &g_fx_req_flare;
        atomic_store(r, v < 0 ? !atomic_load(r) : v != 0); MLOG("[flag] %s = %d (from the next frame)", name, atomic_load(r)); return 1;
    }
    if (!strcmp(name, "bloom")) {
        if (v < 0 || v > 100) { MLOG("[flag] bloom requires 0..100 (percent of the game's)"); return 1; }
        atomic_store(&g_fx_req_bloom, v); MLOG("[flag] bloom = %d%% (from the next frame)", v); return 1;
    }
    if (!strcmp(name, "lod_bias")) {
        if (v < -100 || v > 0) { MLOG("[flag] lod_bias requires -100..0 (hundredths)"); return 1; }
        atomic_store(&g_pp_req_lodb, v); MLOG("[flag] lod_bias = %d/100 (samplers rebuilt at the next frame)", v); return 1;
    }
    return 0;
}
static int pp_env(const char *name, int lo, int hi, int def) {
    const char *e = getenv(name); if (!e || !*e) return def;
    char *end; long v = strtol(e, &end, 10);
    if (*end || v < lo || v > hi) { MLOG("[red] %s=%s ignored (%d..%d)", name, e, lo, hi); return def; }
    return (int)v;
}
/* The game's present (replay thread, between two frames): take the requested settings for the next frame. A changed
   sampler setting drops every built sampler object under the registry lock (samp_by_id builds it again with the new
   values); the argument blocks that hold their IDs are keyed on g_reg_gen, and the GPU work already encoded keeps its
   objects alive until it completes. */
static void pp_latch(void) {
    static int init;
    if (!init) {
        init = 1;
        int aa = pp_env("TOTK_AA", 0, 3, 0), sh = pp_env("TOTK_SHARPEN", 0, 100, 0), an = pp_env("TOTK_ANISO", 0, 16, 0), lb = pp_env("TOTK_LOD_BIAS", -100, 0, 0);
        if (an == 1 || (an & (an - 1))) { if (an > 1) MLOG("[red] TOTK_ANISO=%d ignored (0, 2, 4, 8 or 16)", an); an = 0; }
        int unset = -1; atomic_compare_exchange_strong(&g_pp_req_aa, &unset, aa);
        unset = -1; atomic_compare_exchange_strong(&g_pp_req_sharpen, &unset, sh);
        unset = -1; atomic_compare_exchange_strong(&g_pp_req_aniso, &unset, an);
        unset = -1000; atomic_compare_exchange_strong(&g_pp_req_lodb, &unset, lb);
    }
    int aa = atomic_load(&g_pp_req_aa), sh = atomic_load(&g_pp_req_sharpen), an = atomic_load(&g_pp_req_aniso), lb = atomic_load(&g_pp_req_lodb);
    if (aa != g_pp_aa || sh != g_pp_sharpen) MLOG("[pp] anti-aliasing %s, sharpen %d from frame %llu", k_pp_aa_name[aa], sh, (unsigned long long)atomic_load(&g_cur_frame) + 1);
    if (aa != g_pp_aa) g_postfx.last_aa_n = g_postfx.aa_n, g_postfx.frames = 0;
    g_pp_aa = aa; g_pp_sharpen = sh;
    {
        int hud = atomic_load(&g_fx_req_hud), dof = atomic_load(&g_fx_req_dof), bloom = atomic_load(&g_fx_req_bloom), flare = atomic_load(&g_fx_req_flare);
        if (hud != g_fx_hud || dof != g_fx_dof || bloom != g_fx_bloom || flare != g_fx_flare)
            MLOG("[pp] HUD %s, depth of field %s, bloom %d%%, lens flare %s from frame %llu (so far: UI draws left out %llu, depth of field composites %llu, bloom scaled %llu / kept %llu, lens flare draws %llu)",
                 hud ? "hidden" : "shown", dof ? "on" : "off", bloom, flare ? "on" : "off", (unsigned long long)atomic_load(&g_cur_frame) + 1,
                 (unsigned long long)g_postfx.hud_n, (unsigned long long)g_postfx.dof_n, (unsigned long long)g_postfx.bloom_n,
                 (unsigned long long)g_postfx.bloom_kept, (unsigned long long)g_postfx.flare_n);
        if (hud && !g_fx_hud) { g_postfx.hud_diag = 6; g_postfx.hud_diag_frame = atomic_load(&g_cur_frame) + 1;
            MLOG("[pp] HUD hidden: canvas texture %llu (%ux%u), presented textures %d (%ux%u), UI begun %d", (unsigned long long)g_mfx.canvas,
                 g_canvas_w, g_canvas_h, g_mfx.nswap, g_mfx.swap_w, g_mfx.swap_h, g_uw_ui); }
        g_fx_hud = hud; g_fx_dof = dof; g_fx_bloom = bloom; g_fx_flare = flare;
    }
    if (an != atomic_load(&g_pp_aniso) || lb != atomic_load(&g_pp_lodb)) {
        int n = 0;
        registry_lock();
        atomic_store(&g_pp_aniso, an); atomic_store(&g_pp_lodb, lb);
        for (int i = 0; i < g_nsreg; i++) if (g_sview[i]) { defer_obj_unref((__bridge_transfer id)g_sview[i]); g_sview[i] = NULL; g_srid[i] = 0; n++; }
        g_reg_gen++;
        registry_unlock();
        MLOG("[pp] samplers: anisotropy %d, LOD bias %+d/100; %d sampler objects rebuilt on use", an, lb, n);
    }
    g_postfx.ui_done = g_postfx.fxaa_seen = 0; g_postfx.canvas = nil;
    if (g_pp_aa && ++g_postfx.frames == 600 && g_postfx.aa_n == g_postfx.last_aa_n)
        MLOG("[pp] anti-aliasing %s: no game FXAA draw in the 600 frames since it was chosen (handheld / a loading screen, or a game version pp_is_fxaa does not cover): on the canvas %llu times; kept %llu/%llu/%llu/%llu",
             k_pp_aa_name[g_pp_aa], (unsigned long long)g_postfx.canvas_aa_n, (unsigned long long)g_postfx.aa_kept[1], (unsigned long long)g_postfx.aa_kept[2], (unsigned long long)g_postfx.aa_kept[3], (unsigned long long)g_postfx.aa_kept[4]);
    if (atomic_load(&g_cur_frame) % 3600 == 0 && (g_pp_aa || g_pp_sharpen))
        MLOG("[pp] to frame %llu: anti-aliasing passes %llu, on the canvas %llu (game FXAA kept: replay %llu, targets %llu, state %llu, textures %llu), sharpen passes %llu",
             (unsigned long long)atomic_load(&g_cur_frame), (unsigned long long)g_postfx.aa_n, (unsigned long long)g_postfx.canvas_aa_n, (unsigned long long)g_postfx.aa_kept[1], (unsigned long long)g_postfx.aa_kept[2],
             (unsigned long long)g_postfx.aa_kept[3], (unsigned long long)g_postfx.aa_kept[4], (unsigned long long)g_postfx.cas_n);
}
static int pp_is_fxaa(const char *key) {
    return !strncmp(key, "22a61cb1ec732aef", 16);   // 1.4.3
}
static id<MTLRenderPipelineState> pp_pso(int fn, MTLPixelFormat pf) {
    for (int i = 0; i < 4; i++) if (g_postfx.ps[fn][i] && g_postfx.pf[fn][i] == pf) return g_postfx.ps[fn][i];
    if (!g_postfx.lib && !g_postfx.lib_failed) {
        NSError *e = nil; g_postfx.lib = [g_dev newLibraryWithSource:@(k_post_msl) options:nil error:&e];
        if (!g_postfx.lib) { g_postfx.lib_failed = 1; MLOG("[red] [pp] shaders do not compile: %s", e.localizedDescription.UTF8String); return nil; }
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 3; ad.maxBufferBindCount = 1;
        for (int f = 0; f < PP_NFN; f++) { g_postfx.args[f] = [g_dev newArgumentTableWithDescriptor:ad error:&e]; if (!g_postfx.args[f]) MDIE("[pp] argument table: %s", e.localizedDescription.UTF8String); }
    }
    if (!g_postfx.lib) return nil;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [g_postfx.lib newFunctionWithName:@"pp_vs"]; d.fragmentFunction = [g_postfx.lib newFunctionWithName:@(k_pp_fn[fn])]; d.colorAttachments[0].pixelFormat = pf;
    NSError *e = nil; id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!ps) { MLOG("[red] [pp] pipeline %s (format %lu): %s", k_pp_fn[fn], (unsigned long)pf, e.localizedDescription.UTF8String); return nil; }
    int i = 0; while (i < 3 && g_postfx.ps[fn][i]) i++;
    g_postfx.ps[fn][i] = ps; g_postfx.pf[fn][i] = pf; return ps;
}
/* A private texture of ours for these passes, (re)made when the size or format changes. */
static id<MTLTexture> pp_tex(__strong id<MTLTexture> *t, NSUInteger w, NSUInteger h, MTLPixelFormat pf, MTLTextureUsage u) {
    if (*t && (*t).width == w && (*t).height == h && (*t).pixelFormat == pf) return *t;
    if (*t) defer_obj_release(*t);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    d.storageMode = MTLStorageModePrivate; d.usage = u;
    *t = [g_dev newTextureWithDescriptor:d]; res_add(*t); return *t;
}
static id<MTLTexture> pp_table(const char *hex, NSUInteger w, NSUInteger h, MTLPixelFormat pf, NSUInteger bpp) {
    size_t n = w * h * bpp; uint8_t *b = malloc(n);
    for (size_t i = 0; i < n; i++) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); b[i] = (uint8_t)v; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d];
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:b bytesPerRow:w * bpp];
    free(b); res_add(t); return t;
}
/* One full-screen pass of ours into dst (level 0) within the scissor r: textures t[0..nt), constants k (12 floats);
   clear: 0 load dst, 1 clear it to 0, 2 clear it to g_postfx.cc. */
static int pp_pass(int fn, id<MTLTexture> dst, int clear, __unsafe_unretained id<MTLTexture> const *t, int nt, const float *k, MTLScissorRect r) {
    id<MTLRenderPipelineState> ps = pp_pso(fn, dst.pixelFormat); if (!ps) return 0;
    MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
    rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.colorAttachments[0].clearColor = clear == 2 ? g_postfx.cc : MTLClearColorMake(0, 0, 0, 0); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    blit_close(); rs_sync(); pt_mark(g_cb, 'R', rp); id<MTL4RenderCommandEncoder> en = [g_cb renderCommandEncoderWithDescriptor:rp]; es_reset(); if (!en) return 0;
    [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    id<MTL4ArgumentTable> at = g_postfx.args[fn];
    for (int i = 0; i < nt; i++) [at setTexture:t[i].gpuResourceID atIndex:i];
    id<MTLBuffer> kb; size_t ko = arena_alloc(12 * sizeof(float), &kb); memcpy((uint8_t *)kb.contents + ko, k, 12 * sizeof(float));
    [at setAddress:kb.gpuAddress + ko atIndex:0];
    [en setArgumentTable:at atStages:MTLRenderStageFragment];
    [en setViewport:(MTLViewport){0, 0, dst.width, dst.height, 0, 1}]; [en setScissorRect:r];
    [en setRenderPipelineState:ps]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; m4_end(en);
    return 1;
}
static void pp_done_encoding(void) { args_dirty_all(); (PSO_DIRTY(), g_dss_dirty = 1); }
static int pp_aa_ready(MTLPixelFormat pf) {   // shaders that do not build: the game's picture stays
    return g_pp_aa != 3 ? pp_pso(g_pp_aa == 1 ? PP_COPY : PP_FXAA, pf) != nil
                        : pp_pso(PP_EDGE, MTLPixelFormatRG8Unorm) && pp_pso(PP_WEIGHT, MTLPixelFormatRGBA8Unorm) && pp_pso(PP_BLEND, pf);
}
/* aa_mode 1-3 from src into dst (same size) within sc; dclr as pp_pass's clear for dst. */
static void pp_aa_run(id<MTLTexture> src, id<MTLTexture> dst, MTLScissorRect sc, int dclr) {
    float k[12] = {1.f / src.width, 1.f / src.height, (float)src.width, (float)src.height, (float)sc.x, (float)sc.y, (float)(sc.x + sc.width), (float)(sc.y + sc.height), 0, 0, 0, 0};
    __unsafe_unretained id<MTLTexture> t[3] = {src};
    if (g_pp_aa != 3) pp_pass(g_pp_aa == 1 ? PP_COPY : PP_FXAA, dst, dclr, t, 1, k, sc);
    else {
        if (!g_postfx.area) { g_postfx.area = pp_table(k_smaa_area_hex, SMAA_AREA_W, SMAA_AREA_H, MTLPixelFormatRG8Unorm, 2); g_postfx.search = pp_table(k_smaa_search_hex, SMAA_SEARCH_W, SMAA_SEARCH_H, MTLPixelFormatR8Unorm, 1); }
        MTLTextureUsage u = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> edges = pp_tex(&g_postfx.edges, src.width, src.height, MTLPixelFormatRG8Unorm, u), blend = pp_tex(&g_postfx.blend, src.width, src.height, MTLPixelFormatRGBA8Unorm, u);
        if (pp_pass(PP_EDGE, edges, 1, t, 1, k, sc)) {
            t[0] = edges; t[1] = g_postfx.area; t[2] = g_postfx.search; pp_pass(PP_WEIGHT, blend, 1, t, 3, k, sc);
            t[0] = src; t[1] = blend; pp_pass(PP_BLEND, dst, dclr, t, 2, k, sc);
        }
    }
    pp_done_encoding();
}
/* The game's FXAA draw with aa_mode 1-3: our anti-aliasing instead. Returns 1 when the draw was replaced. */
static int pp_aa_draw(__unsafe_unretained NvProg *pr) {
    if (!pr || !pr->sh[1] || !pp_is_fxaa(pr->sh[1]->key)) return 0;
    g_postfx.fxaa_seen = 1;   // this frame has the game's AA step: no canvas AA (pp_ui_hook)
    if (!g_pp_aa) return 0;
    __unsafe_unretained NvShader *f = pr->sh[1];
    const view_t *cv = &R.rt.cview[0]; int hv = R.rt.has_cview[0], why = 0;
    if (g_defer_replaying || g_frz_state) why = 1;
    else if (R.rt.n < 1 || !R.rt.color[0]) why = 2;
    for (int i = 1; !why && i < R.rt.n; i++) if (R.rt.color[i]) why = 2;
    if (!why && hv && ((cv->set_levels && cv->base_level) || (cv->set_layers && cv->min_layer))) why = 2;
    if (!why && ((S.color.blend_en & 1) || (S.chmask.mask & 0xf) != 0xf)) why = 3;
    uint64_t h = 0; for (int i = 0; i < f->rf.nb && !h; i++) if (f->rf.b[i].kind == NVN_RB_TEX && f->rf.b[i].reg < 128) h = S.tex[1][f->rf.b[i].reg];
    int tid = (int)(h & 0xfffff);
    id<MTLTexture> src = !why && tid > 0 ? (__bridge id<MTLTexture>)tex_fast(tid) : nil, dst = src ? attachment_texture(R.rt.color[0], cv, hv) : nil;
    if (!why && (!src || !dst || src == dst || src.textureType != MTLTextureType2D || dst.textureType != MTLTextureType2D || src.width != dst.width || src.height != dst.height)) why = 4;
    if (why) {
        static int told[6]; if (!told[why]++)
            MLOG("[pp] game FXAA kept (reason %d: 1 replay / 2 targets / 3 blend or mask / 4 textures): source %lux%lu format %lu, target %lux%lu format %lu", why,
                 (unsigned long)src.width, (unsigned long)src.height, (unsigned long)src.pixelFormat, (unsigned long)dst.width, (unsigned long)dst.height, (unsigned long)dst.pixelFormat);
        g_postfx.aa_kept[why]++; return 0;
    }
    long x0 = 0, y0 = 0, x1 = (long)dst.width, y1 = (long)dst.height;   // the draw's viewport and scissor: the picture's region at dynamic resolution
    if (S.has_vp) { x0 = MAX(x0, lroundf(S.vp[0][0])); y0 = MAX(y0, lroundf(S.vp[0][1])); x1 = MIN(x1, lroundf(S.vp[0][0] + S.vp[0][2])); y1 = MIN(y1, lroundf(S.vp[0][1] + S.vp[0][3])); }
    if (R.has_sc) { x0 = MAX(x0, (long)R.sc.x); y0 = MAX(y0, (long)R.sc.y); x1 = MIN(x1, (long)R.sc.x + R.sc.w); y1 = MIN(y1, (long)R.sc.y + R.sc.h); }
    if (x1 <= x0 || y1 <= y0) return 1;
    if (!pp_aa_ready(dst.pixelFormat)) { g_postfx.aa_kept[5]++; return 0; }
    { static int once; if (!once++) MLOG("[pp] anti-aliasing %s replaces the game FXAA %.8s: %lux%lu format %lu -> format %lu, region %ld,%ld-%ld,%ld",
                                         k_pp_aa_name[g_pp_aa], f->key, (unsigned long)src.width, (unsigned long)src.height, (unsigned long)src.pixelFormat, (unsigned long)dst.pixelFormat, x0, y0, x1, y1); }
    rp_end();
    int dclr = 0; if (R.clear_c[0]) { dclr = 2; g_postfx.cc = R.cc[0]; R.clear_c[0] = 0; }   // a clear still pending for the target lands with our pass, as with the game's draw
    gpu_owned_mark(R.rt.color[0]->serial);
    pp_aa_run(src, dst, (MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)}, dclr);
    g_postfx.aa_n++;
    return 1;
}
static id<MTLTexture> pp_canvas_copy(id<MTLTexture> c) {
    id<MTLTexture> s = pp_tex(&g_postfx.cas_src, c.width, c.height, c.pixelFormat, MTLTextureUsageShaderRead);
    id<MTL4ComputeCommandEncoder> b = blit();
    enc_b_copy_texture(b, c, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(c.width, c.height, 1), s, 0, 0, MTLOriginMake(0, 0, 0));
    enc_b_end(g_blit); g_blit = nil;
    return s;
}
/* The finished scene in the canvas, before the UI: anti-aliasing when this frame had no game FXAA draw (handheld), then
   CAS; each from a copy of the canvas back into it. */
static void pp_canvas(id<MTLTexture> c) {
    g_postfx.ui_done = 1;
    if (!c || c.textureType != MTLTextureType2D) return;
    if (g_enc) rp_end();
    if (g_pp_aa >= 2 && !g_postfx.fxaa_seen && pp_aa_ready(c.pixelFormat)) {
        { static int once; if (!once++) MLOG("[pp] anti-aliasing %s on the canvas (no game FXAA draw this frame): %lux%lu format %lu",
                                             k_pp_aa_name[g_pp_aa], (unsigned long)c.width, (unsigned long)c.height, (unsigned long)c.pixelFormat); }
        pp_aa_run(pp_canvas_copy(c), c, (MTLScissorRect){0, 0, c.width, c.height}, 0); g_postfx.canvas_aa_n++;
    }
    if (!g_pp_sharpen) return;
    id<MTLTexture> s = pp_canvas_copy(c);
    float sharp = g_pp_sharpen / 100.f, k[12] = {1.f / c.width, 1.f / c.height, (float)c.width, (float)c.height, 0, 0, (float)c.width, (float)c.height, -1.f / (8.f + (5.f - 8.f) * sharp), 0, 0, 0};
    __unsafe_unretained id<MTLTexture> t[1] = {s};
    pp_pass(PP_CAS, c, 0, t, 1, k, (MTLScissorRect){0, 0, c.width, c.height});
    pp_done_encoding(); g_postfx.cas_n++;
}
/* Before a draw opens its pass: the first UI draw into the canvas runs pp_canvas first; a frame whose canvas got no UI
   runs it before its first draw into a presented texture (the copy of the canvas to the window). */
static void pp_ui_hook(__unsafe_unretained NvProg *pr) {
    if (!(g_pp_sharpen || g_pp_aa >= 2) || g_postfx.ui_done || g_defer_replaying || !pr || !R.rt.n || !R.rt.color[0] || !g_mfx.canvas) return;
    uint64_t ser = R.rt.color[0]->serial;
    if (ser == g_mfx.canvas) {
        if (pr->sh[0] && (mfx_ui_vs(pr->sh[0]->key) || (uil_on() && uw_ui_vs(pr->sh[0]->key)))) pp_canvas(tex_mtl(R.rt.color[0]));   // the UI-layer composite copies the scene at any UI shader
        else if (!g_postfx.canvas) g_postfx.canvas = tex_mtl(R.rt.color[0]);
        return;
    }
    if (g_postfx.canvas) for (int i = 0; i < g_mfx.nswap; i++) if (g_mfx.swap[i] == ser) { pp_canvas(g_postfx.canvas); return; }
}

/* Bloom strength: the tone map's bloom texture (its texture 1), every level, multiplied by g_fx_bloom / 100 in place. */
static id<MTLRenderPipelineState> fx_scale_pso(MTLPixelFormat pf) {
    for (int i = 0; i < 4; i++) if (g_postfx.scale_ps[i] && g_postfx.scale_pf[i] == pf) return g_postfx.scale_ps[i];
    if (!pp_pso(PP_COPY, MTLPixelFormatRGBA16Float)) return nil;   // builds the library
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [g_postfx.lib newFunctionWithName:@"pp_vs"]; d.fragmentFunction = [g_postfx.lib newFunctionWithName:@"pp_zero"];
    MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[0];
    a.pixelFormat = pf; a.blendingEnabled = YES;
    a.sourceRGBBlendFactor = a.sourceAlphaBlendFactor = MTLBlendFactorZero;
    a.destinationRGBBlendFactor = a.destinationAlphaBlendFactor = MTLBlendFactorBlendColor;
    NSError *e = nil; id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!ps) { MLOG("[red] [pp] bloom pipeline (format %lu): %s", (unsigned long)pf, e.localizedDescription.UTF8String); return nil; }
    int i = 0; while (i < 3 && g_postfx.scale_ps[i]) i++;
    g_postfx.scale_ps[i] = ps; g_postfx.scale_pf[i] = pf; return ps;
}
static void fx_bloom_scale(void) {
    int tid = (int)(S.tex[1][1] & 0xfffff);
    id<MTLTexture> b = tid > 0 ? (__bridge id<MTLTexture>)tex_fast(tid) : nil, rt = R.rt.n > 0 && R.rt.color[0] ? tex_mtl(R.rt.color[0]) : nil;
    /* The handle is a sampling view of the texture (shader read only): the passes draw into its parent, the view's levels. */
    NSUInteger lv0 = 0, nlv = b ? b.mipmapLevelCount : 0, slice = 0;
    if (b && !(b.usage & MTLTextureUsageRenderTarget) && b.parentTexture) {
        lv0 = b.parentRelativeLevel; slice = b.parentRelativeSlice; b = b.parentTexture;
    }
    id<MTLRenderPipelineState> ps = b ? fx_scale_pso(b.pixelFormat) : nil;
    if (!b || !ps || b == rt || b.textureType != MTLTextureType2D || slice || !(b.usage & MTLTextureUsageRenderTarget) || b.width > 1024 || lv0 + nlv > b.mipmapLevelCount) {
        static int told; if (!told++) MLOG("[pp] bloom strength: the tone map's texture 1 is not the bloom texture expected (%lux%lu format %lu usage %#lx); the game's bloom stays",
                                           (unsigned long)b.width, (unsigned long)b.height, (unsigned long)b.pixelFormat, (unsigned long)b.usage);
        g_postfx.bloom_kept++; return;
    }
    { static int once; if (!once++) MLOG("[pp] bloom strength %d%%: the tone map's bloom %lux%lu (levels %lu-%lu of %lu, format %lu) scaled in place", g_fx_bloom,
                                         (unsigned long)b.width, (unsigned long)b.height, (unsigned long)lv0, (unsigned long)(lv0 + nlv - 1), (unsigned long)b.mipmapLevelCount, (unsigned long)b.pixelFormat); }
    rp_end(); blit_close(); rs_sync();
    float k = g_fx_bloom / 100.f;
    for (NSUInteger lv = lv0; lv < lv0 + nlv; lv++) {
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
        rp.colorAttachments[0].texture = b; rp.colorAttachments[0].level = lv;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        pt_mark(g_cb, 'R', rp); id<MTL4RenderCommandEncoder> en = [g_cb renderCommandEncoderWithDescriptor:rp]; es_reset(); if (!en) break;
        [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
        NSUInteger w = MAX(b.width >> lv, 1u), h = MAX(b.height >> lv, 1u);
        [en setViewport:(MTLViewport){0, 0, w, h, 0, 1}]; [en setScissorRect:(MTLScissorRect){0, 0, w, h}];
        [en setRenderPipelineState:ps]; [en setBlendColorRed:k green:k blue:k alpha:k];
        [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; m4_end(en);
    }
    pp_done_encoding(); g_postfx.bloom_n++;
}
/* 1.4.3 (other versions: these keys have to be found again). */
static int fx_is_dof(const char *key) { return !strncmp(key, "18b730fad0eec4ba", 16); }
static int fx_is_tonemap(const char *key) { return !strncmp(key, "0ac9a54938974396", 16); }
/* The lens flare's sprites, by vertex and fragment shader together (452f4360 alone might be a shared sprite shader). */
static int fx_is_flare(const char *vs, const char *fs) {
    static const char *const k[][2] = {{"d9793c43f9f3f91b", "675a28d16bbd6513"}, {"1509a8d88b9b1c47", "452f436094c74162"},
                                       {"9e8525f6255f4193", "452f436094c74162"}, {"1509a8d88b9b1c47", "30bf73e31acb5890"}};
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++) if (!strncmp(vs, k[i][0], 16) && !strncmp(fs, k[i][1], 16)) return 1;
    return 0;
}
/* Before the draw opens its pass: 1 = leave this draw out (depth of field or lens flare off); a UI draw with the HUD hidden is masked;
   the tone map with a bloom strength below 100 scales the bloom first. */
static int fx_draw(__unsafe_unretained NvProg *pr) {
    if (!pr || g_defer_replaying) return 0;
    __unsafe_unretained NvShader *vs = pr->sh[0], *fs = pr->sh[1];
    /* Diagnostic, once per toggle to hidden: the first six draws that go to the canvas or use a UI vertex shader, with what
       the match below makes of them; 120 frames with none: said once. */
    if (g_fx_hud && g_postfx.hud_diag > 0 && vs) {
        int on = uw_on_canvas(), ui = uw_ui_vs(vs->key);
        uint64_t tgt = R.rt.n > 0 && R.rt.color[0] ? R.rt.color[0]->serial : 0, f = atomic_load(&g_cur_frame);
        if (on || ui) {
            g_postfx.hud_diag--;
            MLOG("[pp] HUD diag F%llu: vs %.16s target %llu (canvas %llu): on canvas %d, UI shader %d, UI begun %d, TOTK_MFX UI shader %d -> %s",
                 (unsigned long long)f, vs->key, (unsigned long long)tgt, (unsigned long long)g_mfx.canvas, on, ui, g_uw_ui, mfx_ui_vs(vs->key),
                 on && ui && (g_uw_ui || mfx_ui_vs(vs->key)) ? "masked" : "drawn");
        } else if (f > g_postfx.hud_diag_frame + 120) {
            g_postfx.hud_diag = 0;
            MLOG("[pp] HUD diag: 120 frames without a draw into the canvas (texture %llu) or with a UI vertex shader", (unsigned long long)g_mfx.canvas);
        }
    }
    if (g_fx_hud && vs && uw_on_canvas() && uw_ui_vs(vs->key) && (g_uw_ui || mfx_ui_vs(vs->key))) {
        /* Drawn, with every colour write masked off (draw() puts the mask back) rather than left out: a masked UI draw
           does everything but write pixels (occlusion queries counting over it see the same samples), and the first
           one still ends the scene (sharpening, MetalFX). */
        if (!g_fx_masked) { g_fx_saved_mask = S.chmask.mask; g_fx_masked = 1; }
        S.chmask.mask = 0; PSO_DIRTY(); g_postfx.hud_n++;
        return 0;
    }
    if (!fs) return 0;
    if (!g_fx_dof && fx_is_dof(fs->key)) { g_postfx.dof_n++; return 1; }
    if (!g_fx_flare && vs && fx_is_flare(vs->key, fs->key)) { g_postfx.flare_n++; return 1; }
    if (g_fx_bloom < 100 && fx_is_tonemap(fs->key)) fx_bloom_scale();
    return 0;
}
