/* Test the mods folder (games/totk/runtime/mods.c): layouts, mods.txt, flags, conflicts, and the ResourceSizeTable patch
   with a tiny synthetic table, on a scratch folder. Built with zstd's decoder to check the raw-block frame:
   cc -std=c11 tests/test_mods.c games/totk/third_party/zstd/zstddeclib.c */
#include "../games/totk/runtime/mods.c"
#include "../games/totk/third_party/zstd/zstd.h"
#include <stdio.h>

static int failures;
static void expect(int ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; } }
static char root[1024];
static void put(const char *rel, const char *text) {
    char p[2048]; snprintf(p, sizeof p, "%s/%s", root, rel);
    for (char *s = p + strlen(root) + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    FILE *f = fopen(p, "w"); if (!f) { perror(p); exit(1); } fputs(text, f); fclose(f);
}
static int exists(const char *rel) { char p[2048]; struct stat st; snprintf(p, sizeof p, "%s/%s", root, rel); return !stat(p, &st); }
static int index_of(const char *folder) {
    ModInfo m; for (int i = 0; !mods_get(i, &m); i++) if (!strcmp(m.folder, folder)) return i;
    return -1;
}
static ModInfo get(const char *folder) { ModInfo m; memset(&m, 0, sizeof m); mods_get(index_of(folder), &m); return m; }
/* The game's RomFS as the test sees it: two files, one of them asked for in another case. */
static int lookup(const char *path, char *real, size_t cap) {
    static const char *const known[] = {"UI/Foo.txt", "Pack/Actor/X.pack.zs", "Sound/a.bwav", "UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs"};
    for (size_t k = 0; k < 4; k++) {
        if (!strcmp(path, known[k])) return 1;
        if (!strcasecmp(path, known[k])) { snprintf(real, cap, "%s", known[k]); return 2; }
    }
    return 0;
}
static uint32_t entry_for(const uint8_t *t, size_t len, const char *name) {   /* a table lookup, as the game does it */
    uint32_t strsz, ncrc, nname; memcpy(&strsz, t + 10, 4); memcpy(&ncrc, t + 14, 4); memcpy(&nname, t + 18, 4);
    expect(len == 22 + (size_t)ncrc * 8 + (size_t)nname * (strsz + 4), "table size matches its header");
    for (uint32_t k = 0; k < nname; k++) { const uint8_t *e = t + 22 + (size_t)ncrc * 8 + (size_t)k * (strsz + 4);
        if (!strncmp((const char *)e, name, strsz)) { uint32_t v; memcpy(&v, e + strsz, 4); return v; } }
    uint32_t crc = mods_crc32(name), prev = 0;
    for (uint32_t k = 0; k < ncrc; k++) { uint32_t c, v; memcpy(&c, t + 22 + k * 8, 4); memcpy(&v, t + 26 + k * 8, 4);
        expect(!k || c > prev, "CRC table sorted and unique"); prev = c; if (c == crc) return v; }
    return 0;
}

/* A minimal BFLAN: one pai1 section, one entry per pane with one FLVI step track of two keys [(0, 0), (f1, 1)]. */
static void w16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { for (int k = 0; k < 4; k++) p[k] = (uint8_t)(v >> (8 * k)); }
static size_t make_flan(uint8_t *b, const char *const *panes, const float *f1, int np) {
    memset(b, 0, 0x14 + 20 + 4 * (size_t)np + 64 * (size_t)np);
    memcpy(b, "FLAN", 4); b[4] = 0xFF; b[5] = 0xFE; w16(b + 0x10, 1);
    uint8_t *s = b + 0x14; size_t size = 20 + 4 * (size_t)np;
    memcpy(s, "pai1", 4); w16(s + 14, (unsigned)np); w32(s + 16, 20);
    for (int e = 0; e < np; e++) {
        uint8_t *en = s + size; w32(s + 20 + 4 * e, (uint32_t)size);
        snprintf((char *)en, 28, "%s", panes[e]); en[28] = 1; w32(en + 32, 36);    /* one track, right after */
        uint8_t *t = en + 36; memcpy(t, "FLVI", 4); t[4] = 1; w32(t + 8, 12);      /* one group */
        uint8_t *g = t + 12; g[2] = 1; w16(g + 4, 2); w32(g + 8, 12);             /* step curve, 2 keys */
        uint8_t *k = g + 12; float z = 0, f = f1[e]; memcpy(k, &z, 4); w16(k + 4, 0); memcpy(k + 8, &f, 4); w16(k + 12, 1);
        size += 36 + 12 + 12 + 16;
    }
    w32(s + 4, (uint32_t)size);
    return 0x14 + size;
}
/* A SARC of the given members (names sorted is not needed for the lookup). */
static size_t make_sarc(uint8_t *out, const char *const *names, uint8_t *const *data, const size_t *lens, int n) {
    size_t sfnt = 0x20 + 16 * (size_t)n, at = sfnt + 8, name_off[8];
    for (int k = 0; k < n; k++) { name_off[k] = at - (sfnt + 8); size_t l = strlen(names[k]) + 1; memcpy(out + at, names[k], l); at += (l + 3) & ~(size_t)3; }
    size_t data_start = (at + 255) & ~(size_t)255, pos = 0;
    memset(out, 0, 0x20); memcpy(out, "SARC", 4); w16(out + 4, 0x14); out[6] = 0xFF; out[7] = 0xFE; w32(out + 0x0C, (uint32_t)data_start);
    memcpy(out + 0x14, "SFAT", 4); w16(out + 0x18, 0xC); w16(out + 0x1A, (unsigned)n);
    memcpy(out + sfnt, "SFNT", 4); w16(out + sfnt + 4, 8); w16(out + sfnt + 6, 0);
    for (int k = 0; k < n; k++) {
        uint8_t *node = out + 0x20 + 16 * k; w32(node, 0); w32(node + 4, 0x01000000u | (uint32_t)(name_off[k] / 4));
        w32(node + 8, (uint32_t)pos); w32(node + 12, (uint32_t)(pos + lens[k]));
        memcpy(out + data_start + pos, data[k], lens[k]); pos = (pos + lens[k] + 7) & ~(size_t)7;
    }
    w32(out + 8, (uint32_t)(data_start + pos));
    return data_start + pos;
}
static float key_frame(uint8_t *sarc, size_t len, const char *member, int pane) {   /* the second key of pane #pane */
    size_t n; uint8_t *m = sarc_member(sarc, len, member, &n); if (!m) return -1;
    uint8_t *s = m + 0x14, *en = s + le32(s + 20 + 4 * pane); float f; memcpy(&f, en + 36 + 12 + 12 + 8, 4); return f;
}

int main(void) {
    const char *tmp = getenv("TMPDIR"); snprintf(root, sizeof root, "%s/test-mods-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    setenv("TOTK_MODS_DIR", root, 1);

    put("A Standard/romfs/ui/foo.txt", "a");              /* wrong case: the game says UI/Foo.txt */
    put("A Standard/romfs/Pack/Actor/X.pack.zs", "aa");
    put("A Standard/exefs/main.npdm", "code");
    put("A Standard/mod.json", "{\"name\": \"Alpha \\\"Mod\\\"\", \"author\": \"Someone\", \"version\": \"2.0\", "
        "\"source\": {\"site\": \"gamebanana\", \"gamebanana_id\": 12345}, \"game_versions\": [\"1.2.1\", \"1.4.3\"], \"x\": [1, {\"y\": 2}]}");
    put("B Wrapped/B Inner/romfs/Model/New.bfres.mc", "bbbb");
    put("C Bare/UI/Foo.txt", "c");
    put("C Bare/Pack/Actor/Y.pack.zs", "cc");
    put("D Old v1.2.1/romfs/System/Resource/ResourceSizeTable.Product.121.rsizetable.zs", "rstb");
    put("D Old v1.2.1/romfs/Sound/a.bwav", "d");
    put("E Empty/readme.txt", "nothing");
    put("F Package.tkcl", "tkcl");
    put("G Options/Option 1/romfs/UI/Foo.txt", "g1");
    put("G Options/Option 2/romfs/UI/Foo.txt", "g2");
    put("v143/manifest.tsv", "# the older single folder\n");
    put("v143/UI/Foo.txt", "legacy");

    mods_set_romfs_lookup(lookup);
    expect(!strcmp(mods_dir(), root), "TOTK_MODS_DIR names the folder");
    /* Another version's file (the 1.1/1.2 icon archive beside the game's .140): flagged, starts off, not a prompts mod. */
    put("H Xbox UI v2/romfs/UI/LayoutArchive/Common.Product.110.Nin_NX_NVN.blarc.zs", "old icons");
    mods_scan();
    { ModInfo h = get("H Xbox UI v2");
      expect((h.flags & MOD_OLD_VERSION) && !h.enabled && !(h.flags & MOD_PROMPTS), "another version's file: old, off, no prompts role"); }
    expect(!mods_remove("H Xbox UI v2"), "removed again");
    expect(mods_scan() == 7, "seven mods; the older v143 folder is not one");
    ModInfo a = get("A Standard"), b = get("B Wrapped"), c = get("C Bare"), d = get("D Old v1.2.1"), e = get("E Empty"), f = get("F Package.tkcl"), g = get("G Options");
    expect(index_of("A Standard") == 0 && index_of("G Options") == 6, "without mods.txt: by name");
    expect(a.flags == (MOD_HAS_ROMFS | MOD_HAS_CODE) && a.files == 2 && a.enabled, "romfs/ + exefs/");
    expect(!strcmp(a.name, "Alpha \"Mod\"") && !strcmp(a.author, "Someone") && !strcmp(a.version, "2.0"), "mod.json strings");
    expect(a.gamebanana_id == 12345 && !strcmp(a.game_versions, "1.2.1, 1.4.3"), "mod.json nested id and version list");
    expect(b.flags == MOD_HAS_ROMFS && b.files == 1, "an archive's wrapping folder");
    expect((c.flags & MOD_HAS_ROMFS) && c.files == 2, "RomFS folders at the top level");
    expect((c.flags & MOD_CONFLICT) && c.conflicts == 1, "C changes UI/Foo.txt after A");
    expect(d.flags == (MOD_HAS_ROMFS | MOD_OWN_RSTB | MOD_OLD_VERSION) && d.files == 1, "own table left out; 1.2.1 in the name");
    expect(!d.enabled, "a new mod made for another game version starts off");
    expect(e.flags == MOD_EMPTY && e.files == 0, "nothing to load");
    expect(f.flags & MOD_TKCL, "a .tkcl package");
    expect((g.flags & MOD_EMPTY) && g.files == 0, "two romfs folders side by side: none chosen");
    const ModFile *files; int n = mods_files(&files);
    expect(n == 4, "winners: A 2, B 1, C 1 (UI/Foo.txt stays A); D is off");
    int foo = 0;
    for (int k = 0; k < n; k++) if (!strcmp(files[k].path, "UI/Foo.txt")) { foo++; expect(files[k].mod == 0 && strstr(files[k].file, "A Standard/romfs/ui/foo.txt"), "first mod wins, in the game's spelling"); }
    expect(foo == 1, "a path once");
    for (int k = 0; k < n; k++) if (!strcmp(files[k].path, "Model/New.bfres.mc")) expect(!files[k].replaces, "a file the game lacks is added");
    for (int k = 0; k < n; k++) if (!strcmp(files[k].path, "Sound/a.bwav")) expect(files[k].replaces, "a file the game has is replaced");
    for (unsigned bit = 1; bit <= MOD_PROMPTS; bit <<= 1) expect(*mods_notice(bit) != 0, "every flag has a notice");

    /* mods.txt: order, off, unknown names dropped, the rest appended by name and on. */
    put("mods.txt", "# order\n-C Bare\n  B Wrapped  \nMissing\n\n");
    mods_scan();
    expect(index_of("C Bare") == 0 && index_of("B Wrapped") == 1 && index_of("A Standard") == 2 && index_of("G Options") == 6, "mods.txt order, then the rest");
    c = get("C Bare"); a = get("A Standard");
    expect(!c.enabled && !(c.flags & MOD_CONFLICT) && !(a.flags & MOD_CONFLICT), "an off mod claims nothing");
    mods_mark_loaded();
    expect(!mods_changed_since_start(), "nothing changed yet");
    expect(!mods_set_enabled("C Bare", 1), "turn C on");
    a = get("A Standard");
    expect((a.flags & MOD_CONFLICT) && a.conflicts == 1 && mods_changed_since_start(), "C first now: A loses UI/Foo.txt");
    expect(!mods_move("C Bare", 6) && index_of("C Bare") == 6 && index_of("B Wrapped") == 0, "move to the end");
    a = get("A Standard"); c = get("C Bare");
    expect(!(a.flags & MOD_CONFLICT) && (c.flags & MOD_CONFLICT), "after the move A wins again");
    expect(!mods_set_enabled("E Empty", 0) && mods_set_enabled("nope", 1) < 0, "unknown folder");
    expect(!mods_save(), "save");
    mods_scan();
    expect(index_of("B Wrapped") == 0 && index_of("C Bare") == 6 && get("C Bare").enabled && !get("E Empty").enabled, "mods.txt read back");
    expect(!mods_remove("E Empty") && !exists("E Empty") && index_of("E Empty") < 0 && mods_count() == 6, "remove deletes the folder");
    expect(mods_remove("../x") < 0 && mods_remove("") < 0, "remove stays in the folder");

    /* Estimates (TkSharp's calculator). */
    expect(mods_rstb_estimate("Actor/X.bgyml", 20, NULL, 0) == (32 + 2000) * 8, ".bgyml");
    expect(mods_rstb_estimate("Pack/Actor/X.pack.zs", 100, NULL, 0) == 128 + 0x180, ".pack.zs");
    expect(mods_rstb_estimate("UI/Font.casset.byml.zs", 40, NULL, 0) == 64 + 0x1C0, ".casset.byml");
    expect(mods_rstb_estimate("Sound/Resource/Stream/a.bwav", 100, NULL, 0) == 0, "streams have no entry");
    expect(mods_rstb_estimate("Foo/a.unknown", 1, NULL, 0) == (32 + 1500) * 4, "other types");
    uint8_t bstar[16] = {0}; bstar[8] = 3;
    expect(mods_rstb_estimate("Preload/a.bstar.zs", 16, bstar, 16) == 32 + 0x120 + 24, ".bstar entry count");
    uint8_t mc[16] = {0}; int32_t flags = (100 << 5) | 2; memcpy(mc + 8, &flags, 4);
    expect(mods_rstb_estimate("Model/a.bfres.mc", 16, mc, 16) == (uint32_t)(400 * 2.55), ".mc");
    expect(mods_rstb_needs_data("AI/a.ainb") && mods_rstb_needs_data("AS/a.asb.zs") && !mods_rstb_needs_data("UI/a.blarc.zs"), "needs data");

    /* A tiny RESTBL: three CRC entries and one name entry. */
    const char *names[3] = {"A/a.bgyml", "B/b.byml", "C/c.pack"}; uint32_t sizes[3] = {50000, 1000, 2000};
    uint32_t strsz = 160, ncrc = 3, nname = 1, version = 1; size_t len = 22 + 3 * 8 + 164;
    uint8_t *t = calloc(1, len); memcpy(t, "RESTBL", 6); memcpy(t + 6, &version, 4); memcpy(t + 10, &strsz, 4); memcpy(t + 14, &ncrc, 4); memcpy(t + 18, &nname, 4);
    uint32_t crcs[3]; for (int k = 0; k < 3; k++) crcs[k] = mods_crc32(names[k]);
    for (int k = 0; k < 3; k++) for (int j = k + 1; j < 3; j++) if (crcs[j] < crcs[k]) { uint32_t x = crcs[k]; crcs[k] = crcs[j]; crcs[j] = x; const char *y = names[k]; names[k] = names[j]; names[j] = y; x = sizes[k]; sizes[k] = sizes[j]; sizes[j] = x; }
    for (int k = 0; k < 3; k++) { memcpy(t + 22 + k * 8, &crcs[k], 4); memcpy(t + 26 + k * 8, &sizes[k], 4); }
    memcpy(t + 46, "N/n.txt", 7); uint32_t ten = 10; memcpy(t + 46 + 160, &ten, 4);
    expect(entry_for(t, len, "B/b.byml") == 1000 && entry_for(t, len, "N/n.txt") == 10, "the synthetic table reads");
    expect(mods_crc32("plumless") == mods_crc32("buckeroo"), "a known CRC32 collision");
    const char *paths[] = {"B/b.byml.zs", "A/a.bgyml", "New/x.byml.zs", "N/n.txt", "Skip/x.bwav", "plumless", "buckeroo", "New/x.byml"};
    uint32_t values[] = {4000, 100, 777, 500, 0, 11, 22, 900};
    size_t out_len = 0; ModsRstbStats st;
    uint8_t *p = mods_rstb_patch(t, len, paths, values, 8, &out_len, &st);
    expect(p != NULL, "patched");
    if (p) {
        expect(entry_for(p, out_len, "B/b.byml") == 4000, "raised (path without .zs)");
        expect(entry_for(p, out_len, "A/a.bgyml") == 50000, "kept when already larger");
        expect(entry_for(p, out_len, "C/c.pack") == 2000, "untouched");
        expect(entry_for(p, out_len, "New/x.byml") == 900, "added, then raised by its second mention");
        expect(entry_for(p, out_len, "N/n.txt") == 500, "the name table");
        expect(entry_for(p, out_len, "plumless") == 11 && entry_for(p, out_len, "buckeroo") == 22, "a collision between new paths goes to the name table");
        expect(st.raised == 3 && st.kept == 1 && st.added == 3 && st.skipped == 1, "stats");
        expect(mods_rstb_patch(p, out_len - 1, paths, values, 1, &len, NULL) == NULL, "a damaged table is refused");
        /* Served as a raw-block zstd frame: the real decoder must give it back. */
        size_t fl; uint8_t *frame = mods_zstd_raw_frame(p, out_len, &fl), back[1024];
        expect(frame && ZSTD_getFrameContentSize(frame, fl) == out_len, "frame content size");
        expect(frame && ZSTD_decompress(back, sizeof back, frame, fl) == out_len && !memcmp(back, p, out_len), "zstd reads the frame back");
        free(frame);
    }
    size_t big = 300000, fl; uint8_t *data = malloc(big), *again = malloc(big); for (size_t k = 0; k < big; k++) data[k] = (uint8_t)(k * 7 + k / 1000);
    uint8_t *frame = mods_zstd_raw_frame(data, big, &fl);
    expect(fl == 4 + 1 + 4 + 3 * 3 + big && ZSTD_decompress(again, big, frame, fl) == big && !memcmp(data, again, big), "three blocks");
    free(frame); frame = mods_zstd_raw_frame(data, 0, &fl);
    expect(ZSTD_decompress(again, big, frame, fl) == 0, "an empty frame");
    free(frame); free(data); free(again); free(p); free(t);

    /* A mod file the port's own patches also change. */
    expect(mods_build_collision("System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs") == MODS_BUILD_KEEP, "the HeapDef stays the port's");
    expect(mods_build_collision("UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs") == MODS_BUILD_REFIX, "1.4.3 Common archive: refixed");
    expect(mods_build_collision("UI/LayoutArchive/Common.Product.100.Nin_NX_NVN.blarc.zs") == MODS_BUILD_REFIX, "1.0.0 Common archive: refixed");
    expect(mods_build_collision("UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc") == MODS_BUILD_MOD_WINS &&
           mods_build_collision("UI/LayoutArchive/Common.Product.x.Nin_NX_NVN.blarc.zs") == MODS_BUILD_MOD_WINS, "only that archive");
    expect(mods_build_collision("Event/EventFlow/DmF_SY_GameOver.bfevfl.zs") == MODS_BUILD_MOD_WINS, "the game over flow: the mod wins");

    /* The quick-menu background fix on a synthetic Common archive. */
    const char *const sc_panes[] = {"N_Other_00", "C_CaptureBG_00", "C_CaptureClearBG_00", "P_CaptureClearBG_00"}, *const ab_panes[] = {"C_CaptureBG_00"};
    const float one4[] = {1, 1, 1, 1}, one1[] = {1}, odd1[] = {2};
    uint8_t *sc = calloc(1, 4096), *ab = calloc(1, 4096), *ab_odd = calloc(1, 4096), *other = calloc(1, 64), *arc = calloc(1, 16384);
    size_t sc_n = make_flan(sc, sc_panes, one4, 4), ab_n = make_flan(ab, ab_panes, one1, 1), ab_odd_n = make_flan(ab_odd, ab_panes, odd1, 1);
    memcpy(other, "not a layout", 12);
    const char *const arc_names[] = {"blyt/X.bflyt", "anim/ShortCutBG_00_In.bflan", "anim/AutoBuilder_00_In.bflan"};
    uint8_t *arc_data[] = {other, sc, ab}; size_t arc_lens[] = {12, sc_n, ab_n};
    size_t arc_n = make_sarc(arc, arc_names, arc_data, arc_lens, 3); char why[300];
    expect(key_frame(arc, arc_n, "anim/ShortCutBG_00_In.bflan", 1) == 1.0f, "the synthetic archive reads");
    expect(mods_capture_fix(arc, arc_n, why, sizeof why) == 4, "four keys moved");
    expect(key_frame(arc, arc_n, "anim/ShortCutBG_00_In.bflan", 1) == 0.0f && key_frame(arc, arc_n, "anim/ShortCutBG_00_In.bflan", 3) == 0.0f &&
           key_frame(arc, arc_n, "anim/AutoBuilder_00_In.bflan", 0) == 0.0f, "moved to frame 0");
    expect(key_frame(arc, arc_n, "anim/ShortCutBG_00_In.bflan", 0) == 1.0f, "another pane is left alone");
    expect(mods_capture_fix(arc, arc_n, why, sizeof why) == 0, "already fixed: nothing to move");
    arc_data[2] = ab_odd; arc_lens[2] = ab_odd_n; arc_n = make_sarc(arc, arc_names, arc_data, arc_lens, 3);
    uint8_t *before = malloc(arc_n); memcpy(before, arc, arc_n);
    expect(mods_capture_fix(arc, arc_n, why, sizeof why) == -1 && strstr(why, "(2, 1)") && !memcmp(before, arc, arc_n), "other keys: refused, nothing changed");
    arc_n = make_sarc(arc, arc_names, arc_data, arc_lens, 2);
    expect(mods_capture_fix(arc, arc_n, why, sizeof why) == -1 && strstr(why, "AutoBuilder"), "a missing member: refused");
    expect(mods_capture_fix(other, 12, NULL, 0) == -1, "not a SARC");
    free(sc); free(ab); free(ab_odd); free(other); free(arc); free(before);

    /* The port's own edits merged into a mod's copy (Native 16:10 layouts, mods_port_merge). Members: L the port edits two words
       of, of which the mod changes one too (its icons); A the port edits and the mod resized; B the port leaves alone. */
    {
        float lg[4] = {1, 2, 3, 4}, lp[4] = {1, 0.9f, 3, 5}, lm[4] = {7, 2, 3, 6}, ag[2] = {1, 1}, ap[2] = {1, 0.5f}, am[3] = {1, 1, 9}, bg[2] = {8, 8};
        const char *const nm[] = {"blyt/L.bflyt", "anim/A.bflan", "blyt/B.bflyt"};
        uint8_t *gd[] = {(uint8_t *)lg, (uint8_t *)ag, (uint8_t *)bg}, *pd[] = {(uint8_t *)lp, (uint8_t *)ap, (uint8_t *)bg}, *md[] = {(uint8_t *)lm, (uint8_t *)am, (uint8_t *)bg};
        size_t gl[] = {16, 8, 8}, ml[] = {16, 12, 8};
        uint8_t *g = calloc(1, 4096), *p = calloc(1, 4096), *m = calloc(1, 4096);
        size_t gn = make_sarc(g, nm, gd, gl, 3), pn = make_sarc(p, nm, pd, gl, 3), mn = make_sarc(m, nm, md, ml, 3);
        ModsMerge ms; size_t n;
        expect(mods_port_merge(m, mn, g, gn, p, pn, &ms, why, sizeof why) == 1 && ms.members == 2 && ms.skipped == 1 && ms.kept == 1, "merge: counts");
        float *l = (float *)sarc_member(m, mn, "blyt/L.bflyt", &n);
        expect(l && l[0] == 7 && l[1] == 0.9f && l[2] == 3 && l[3] == 6, "merge: the port's value where the mod kept the game's, the mod's where it changed it");
        float *a = (float *)sarc_member(m, mn, "anim/A.bflan", &n);
        expect(a && n == 12 && a[1] == 1, "merge: a member the mod resized stays the mod's");
        expect(mods_port_merge(m, mn, g, gn, p, pn, &ms, why, sizeof why) == 0, "merge: already merged");
        expect(mods_port_merge(m, mn, g, gn, (const uint8_t *)"not a SARC", 10, &ms, why, sizeof why) == -1 && strstr(why, "port"), "merge: the port's copy is no SARC");
        free(g); free(p); free(m);
    }

    char cmd[1200]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", root); if (system(cmd)) {}
    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    printf("mods: all checks passed\n");
    return 0;
}
