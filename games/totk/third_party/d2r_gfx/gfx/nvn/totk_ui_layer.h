/* Included only by nvn_mtl.m (after totk_post.h). The UI-layer composite (TOTK_UI_LAYER, default on; 2026-10-24).

   Key 4 / Aspect ratio shows the game's 16:9 canvas stretched over a wider or taller window, with the camera seeing that
   wider or taller view (runtime/aspect_patch.h). The game still lays its UI out for 16:9, so drawn into the same canvas
   it comes out stretched. The first answer squeezed each UI draw back by its viewport (uw_ui_squeeze), choosing UI draws
   by a list of vertex shaders: every UI draw the list missed stayed stretched (the boss bar's fill sat 56 canvas pixels
   above its track, a line through the boss name), and every full-screen one it caught was squeezed short of the edges
   (fades to black, backdrops, loading screens). The user called the approach flawed (2026-10-24); it was.

   Now nothing is squeezed per draw. The UI is taken as one picture: TOTK_MFX's UI layer (colour attachment 7 of every
   canvas pass after the scene, premultiplied: the game's colour blend, alpha accumulating the coverage of "over" draws)
   is filled whether or not frame interpolation is on, the scene is copied when the UI begins (mfx_snapshot_scene), and
   just before the game copies the canvas to the presented texture two passes of ours rewrite the canvas:
     1. ui_squeeze: the UI layer as one picture, squeezed about the centre by the same factors (totk_ui_squeeze.h) into a
        second UI texture; outside the squeezed picture its outermost texels carry on to the edges, so a fade to black
        or a full-screen backdrop still covers the whole window and an empty edge stays empty;
     2. ui_over: canvas = that UI over the scene copy, pixel for pixel.
   The present then stretches the canvas as before, so the UI keeps its shape in the middle 16:9 of the window, every
   element in its exact place relative to the others, and the world fills the rest. With interpolation on, the squeezed UI
   becomes the UI the interpolator takes (mfx_ui_swap), so generated frames match the real ones.
   The scene ends at the first draw into the canvas with any UI vertex shader (uw_ui_vs), not only the interpolation
   list: a loading screen draws 2117f58b quads first.
   Limits: a UI draw whose blend is neither "over" nor additive (multiply, min / max) has no premultiplied form, so in the
   layer it does nothing to what lies below it; a draw whose fragment shader cannot copy its colour to the layer is left
   out of it ([red] [mfx] line). TOTK_UI_LAYER=0 goes back to the per-draw squeeze; TOTK_UI_SQUEEZE=0 (layouts made for the
   wide canvas) turns both off. Breath of the Wild keeps the per-draw squeeze (UIL_GAME). */
static int uil_on(void) {   // declared with g_uil in nvn_mtl.m
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_UI_LAYER"), *sq = getenv("TOTK_UI_SQUEEZE");
        v = UIL_GAME && (!e || strcmp(e, "0")) && !(sq && !strcmp(sq, "0"));
        if (UIL_GAME && e && !strcmp(e, "0")) MLOG("TOTK_UI_LAYER=0: key 4 squeezes each UI draw instead of composing the UI as one layer");
    }
    if (!v) return 0;
    double ux, uy; totk_ui_squeeze(uw_ratio(), &ux, &uy);
    return ux != 1 || uy != 1;
}
static id<MTLTexture> uil_scene_target(NSUInteger w, NSUInteger h, MTLPixelFormat pf) {
    return mfx_enabled() ? mfx_scene_target(g_dev, w, h, pf) : pp_tex(&g_uil.scene, w, h, pf, MTLTextureUsageShaderRead);
}
static id<MTLTexture> uil_ui_target(NSUInteger w, NSUInteger h) {
    return mfx_enabled() ? mfx_ui_target(g_dev, w, h) : pp_tex(&g_uil.ui, w, h, MTLPixelFormatRGBA16Float, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
}
static id<MTLTexture> uil_ui_alt(NSUInteger w, NSUInteger h) {
    return mfx_enabled() ? mfx_ui_alt() : pp_tex(&g_uil.ui2, w, h, MTLPixelFormatRGBA16Float, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
}
static void uil_swap(void) {
    if (mfx_enabled()) { mfx_ui_swap(); return; }
    id<MTLTexture> t = g_uil.ui; g_uil.ui = g_uil.ui2; g_uil.ui2 = t;
}
static void uil_compose(void) {
    g_uil.done = 1;
    id<MTLTexture> c = g_uil.canvas, sc = g_uil.scene_cur, u = c ? uil_ui_target(c.width, c.height) : nil;
    id<MTLTexture> alt = c ? uil_ui_alt(c.width, c.height) : nil;
    if (!c || !sc || !u || !alt || sc.width != c.width || sc.height != c.height) {
        static int n; if (n++ < 5) MLOG("[red] [ui-layer] frame %llu: nothing to compose with (canvas %d, scene copy %d, UI layer %d); the UI stays as drawn",
                                         (unsigned long long)atomic_load(&g_cur_frame), c != nil, sc != nil, u != nil && alt != nil);
        g_uil.missed++; return;
    }
    if (g_enc) rp_end();
    if (mfx_enabled()) mfx_queue_residency();
    double ux, uy; totk_ui_squeeze(uw_ratio(), &ux, &uy);
    float k[12] = {1.f / c.width, 1.f / c.height, (float)c.width, (float)c.height, 0, 0, (float)c.width, (float)c.height, (float)ux, (float)uy, 0, 0};
    MTLScissorRect r = {0, 0, c.width, c.height};
    __unsafe_unretained id<MTLTexture> t1[1] = {u}, t2[2] = {alt, sc};
    if (!pp_pass(PP_UI_SQUEEZE, alt, 1, t1, 1, k, r) || !pp_pass(PP_UI_OVER, c, 1, t2, 2, k, r)) { pp_done_encoding(); g_uil.missed++; return; }
    pp_done_encoding();
    uil_swap();
    if (!g_uil.composed++) MLOG("[ui-layer] the UI is composed as one layer: squeezed %.3f across, %.3f down, canvas %lux%lu (TOTK_UI_LAYER=0: per-draw squeeze)",
                                1 / ux, 1 / uy, (unsigned long)c.width, (unsigned long)c.height);
}
/* Before a draw opens its pass: the first draw into a presented texture (the game's copy of the canvas to the window)
   after this frame's UI composes it first. */
static void uil_hook(void) {
    if (g_uil.done || !g_mfx.ui_cleared || !g_mfx.scene_done || g_defer_replaying || !R.rt.n || !R.rt.color[0] || !uil_on()) return;
    uint64_t ser = R.rt.color[0]->serial;
    for (int i = 0; i < g_mfx.nswap; i++) if (g_mfx.swap[i] == ser) { uil_compose(); return; }
}
/* TOTK_UIL_DIAG=1 (diagnostic, 2026-10-26: the inventory and quick select flash at the stretched scale): the draws into
   the canvas before the UI layer began, per frame; a frame that ends without a UI layer while composing is on logs
   them (vertex / fragment shader, index or vertex count), the first 60 distinct sequences. */
static struct { int on, n; char seq[48][24]; uint32_t hash; uint32_t seen[64]; int nseen, logged; } g_uild = {-1};
static void uil_diag_note(__unsafe_unretained NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    if (g_uild.on < 0) { const char *e = getenv("TOTK_UIL_DIAG"); g_uild.on = e && atoi(e) > 0; }
    if (!g_uild.on || g_uild.n >= 48 || !pr) return;
    snprintf(g_uild.seq[g_uild.n++], 24, "%.8s/%.8s/%u", pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[1] ? pr->sh[1]->key : "-", dw ? dw->count : di ? di->count : 0);
}
static void uil_diag_frame_end(int composed) {
    /* Composed frames: only the draws after the scene's last full-screen finish (the upscale ac58fc18 / a3a570eb or the
       tone map 0ac9a549) and before the UI layer began: UI the layer missed (it went out with the scene, stretched). */
    int from = 0;
    if (composed) { from = g_uild.n; for (int i = 0; i < g_uild.n; i++) { const char *f = strchr(g_uild.seq[i], '/') + 1;
                    if (!strncmp(f, "ac58fc18", 8) || !strncmp(f, "a3a570eb", 8) || !strncmp(f, "0ac9a549", 8)) from = i + 1; }
                    from = from < g_uild.n - 1 ? from : g_uild.n - 1; }   // the last entry is the draw that began the layer
    if (g_uild.on > 0 && g_uild.n - from > (composed ? 1 : 0) && uil_on() && g_uild.logged < 60) {
        uint32_t h = composed ? 7u : 2166136261u; for (int i = from; i < g_uild.n; i++) for (const char *c = g_uild.seq[i]; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
        int dup = 0; for (int i = 0; i < g_uild.nseen; i++) dup |= g_uild.seen[i] == h;
        if (!dup) {
            if (g_uild.nseen < 64) g_uild.seen[g_uild.nseen++] = h;
            g_uild.logged++;
            char b[48 * 26 + 1]; size_t o = 0; for (int i = from; i < g_uild.n && o < sizeof b - 26; i++) o += (size_t)snprintf(b + o, sizeof b - o, "%s ", g_uild.seq[i]);
            MLOG("[ui-layer] diag F%llu: %s (vs/fs/count): %s", (unsigned long long)atomic_load(&g_cur_frame),
                 composed ? "draws after the scene's finish, before the UI layer began, the last one began it" : "no UI layer this frame; canvas draws", b);
        }
    }
    g_uild.n = 0;
}
/* At the game's present, before interpolation takes the frame: count a frame whose UI was never composed, and start the
   next frame (interpolation resets the scene / UI state itself while on). */
static void uil_frame_end(void) {
    if (g_mfx.ui_cleared && !g_uil.done && uil_on()) {
        g_uil.missed++;
        static int n; if (n++ < 5) MLOG("[red] [ui-layer] frame %llu: UI drawn but the canvas was not copied to a presented texture after it; not composed", (unsigned long long)atomic_load(&g_cur_frame));
    }
    uil_diag_frame_end(g_uil.done);
    if (!mfx_enabled()) g_mfx.scene_done = g_mfx.ui_cleared = 0;
    g_uil.done = 0; g_uil.canvas = nil; g_uil.scene_cur = nil;
    if (++g_uil.frames % 3600 == 0 && (g_uil.composed || g_uil.missed))
        MLOG("[ui-layer] to frame %llu: composed %llu frames, not composed %llu", (unsigned long long)atomic_load(&g_cur_frame), (unsigned long long)g_uil.composed, (unsigned long long)g_uil.missed);
}
