/* Player mods (runtime/mods.c): the installed mods, their order and what the game loads from them. Shared by the loader
   (runtime/romfs.c serves the files), the Mac and iPhone / iPad menus (the Mods tab) and the mod browser
   (runtime/mod_browser.m installs from GameBanana).

   Layout, under the mods folder (Mac: ~/Library/Application Support/MacNativeGames/TOTK/mods, iPhone / iPad: the app's
   Documents/mods, visible in Files; TOTK_MODS_DIR overrides):
     mods.txt             the load order: one folder name per line, "-" in front = installed but off; first line wins a
                          file two mods both change. Folders not listed are added at the end, on.
     <Mod>/romfs/...      RomFS files at their game paths, as Yuzu / Ryujinx / Atmosphère mods ship them. A folder
                          whose top level already is RomFS paths (Pack/, Model/, UI/, ...) counts as romfs/ itself.
     <Mod>/exefs/...      code patches (pchtxt, ips, subsdk): detected and reported, not applied yet.
     <Mod>/cheats/<BID>.txt   Atmosphère cheat files named by the main module's build ID (the Yuzu / Ryujinx layout; also
                          found deeper, as in atmosphere/contents/<title>/cheats): on the Mac, runtime/cheats.c lists the
                          running build's file from each mod that is on, in load order (Settings › Cheats).
     <Mod>/mod.json       optional: what the browser knows (name, author, version, source ids, game versions it names).
   Every file a mod ships is checked against the game's RomFS at start (replaces a file, or adds one); a mod's own
   ResourceSizeTable is left out and the runtime fixes the 1.4.3 table for the files mods change instead. A mod's file
   wins over the port's own patch of it, except where the port needs its copy (mods_build_collision). Changes to the
   list apply at the next start. The older single folder mods/<v143>/ with manifest.tsv keeps working. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum {   /* ModInfo.flags: what the menu shows next to a mod */
    MOD_HAS_ROMFS = 1,           /* files the game loads */
    MOD_HAS_CODE = 2,            /* exefs/ code patches: not applied (the port's code is translated ahead of time) */
    MOD_OWN_RSTB = 4,            /* shipped its own ResourceSizeTable: left out, the runtime's fixed table is used */
    MOD_OLD_VERSION = 8,         /* mod.json or its name says another game version (e.g. 1.2.1): may not work on 1.4.3 */
    MOD_CONFLICT = 16,           /* changes a file an earlier mod in the list also changes: that mod's copy is used */
    MOD_TKCL = 32,               /* a TKMM .tkcl package: not supported yet */
    MOD_EMPTY = 64,              /* nothing the game can load */
    MOD_PROMPTS = 128,           /* button icons (Xbox / PlayStation): switched by the Controls › Button prompts setting */
    MOD_HAS_CHEATS = 256,        /* a cheats/ folder (Atmosphère cheat files, <build ID>.txt): the Mac's Cheats tab lists them */
};

typedef struct {
    char folder[256];            /* the folder name under mods/, the mod's id */
    char name[256];              /* mod.json name, else the folder name */
    char author[128], version[64];
    char game_versions[64];      /* e.g. "1.2.1, 1.4.x" from mod.json; "" = not stated */
    int enabled;                 /* 1 = loaded at the next start */
    unsigned flags;              /* MOD_* */
    unsigned files, conflicts;   /* RomFS files it ships; files an earlier mod already changes */
    uint64_t bytes;
    int64_t gamebanana_id;       /* mod.json source: the GameBanana mod (0 = none) */
    int prompts;                 /* MOD_PROMPTS: the style its button icons are (MODS_PROMPTS_XBOX / _PLAYSTATION), else 0 */
} ModInfo;

const char *mods_dir(void);                      /* the mods folder (created on first use) */
int mods_scan(void);                             /* reads the folder and mods.txt again; returns the count */
int mods_count(void);
int mods_get(int i, ModInfo *out);               /* in load order; 0 on success */
int mods_set_enabled(const char *folder, int on);
int mods_move(const char *folder, int to_index); /* load order */
int mods_remove(const char *folder);             /* deletes the folder */
int mods_save(void);                             /* writes mods.txt; 0 on success */
int mods_changed_since_start(void);              /* 1: the list differs from what the game loaded (restart to apply) */
const char *mods_notice(unsigned flags);         /* one line for a flag, for the menu */
/* Mod i's cheat file for this build ID (<its cheats folder>/<build_id>.txt, any case) into out; 1 if there is one.
   Whether the mod is on is the caller's question (mods_get). */
int mods_cheat_file(int i, const char *build_id, char *out, size_t cap);
/* Button prompts (settings: controls.prompts): a mod that changes the game's button-icon archive
   (UI/LayoutArchive/Common.Product.*.blarc.zs) and whose mod.json "prompts" or name says Xbox or PlayStation is a
   prompts mod. mods_set_prompts picks the style before mods_scan: the first prompts mod of that style in the list is on,
   every other prompts mod off (MODS_PROMPTS_SWITCH: all off, the game's own icons); 0 leaves mods.txt as it is. */
enum { MODS_PROMPTS_SWITCH = 1, MODS_PROMPTS_XBOX = 2, MODS_PROMPTS_PLAYSTATION = 3 };
void mods_set_prompts(int style);

/* Added for the loader (runtime/romfs.c) and tests/test_mods.c; the menus need none of this.

   mod.json fields read (at any depth, the first one found): "name" / "title", "author" / "submitter", "version",
   "game_versions" / "gameVersions" / "game_version" (a string or a list of strings), "gamebanana_id" / "gamebananaId" /
   "gamebanana" (a number or digits). Other fields are ignored. A folder named like the older single folder (v143 ...)
   that holds a manifest.tsv is that older layout and not listed. */
typedef struct {
    const char *path;            /* the RomFS path, in the RomFS's own case when the mod spelled it otherwise */
    const char *file;            /* the file on disk */
    uint64_t size;
    int mod;                     /* index in load order (mods_get) */
    int replaces;                /* 1: replaces a RomFS file, 0: adds one (only known with a RomFS lookup) */
} ModFile;
/* How mods.c asks the game's RomFS about a path: 0 = no such file, 1 = exact, 2 = only by case (the RomFS's spelling
   written to real). Without one (tests, a menu outside the game) every file counts as a replacement, spelt as shipped. */
typedef int (*ModsRomfsLookup)(const char *path, char *real, size_t cap);
void mods_set_romfs_lookup(ModsRomfsLookup lookup);
int mods_files(const ModFile **out);             /* the enabled mods' files from the last mods_scan: each path once, the
                                                    first mod's copy, in load order; returns the count */
void mods_mark_loaded(void);                     /* the loader: what mods_changed_since_start compares with */

/* The ResourceSizeTable (RESTBL; 1.4.3: MODS_RSTB_PATH): the memory the game sets aside for each resource, keyed by the
   CRC32 of its path without .zs / .mc, with a name table for paths whose CRC32 collides. */
#define MODS_RSTB_PATH "System/Resource/ResourceSizeTable.Product.143.Nin_NX_NVN.rsizetable.zs"
/* The size to reserve for a resource (TkSharp's calculator): size = decompressed bytes; data = the decompressed bytes
   for the types that need them (mods_rstb_needs_data), else NULL. 0 = leave the entry alone (streams, the table). */
uint32_t mods_rstb_estimate(const char *path, uint64_t size, const uint8_t *data, size_t len);
int mods_rstb_needs_data(const char *path);
typedef struct { unsigned raised, added, kept, skipped; } ModsRstbStats;
/* Each path's entry becomes max(the table's, value); missing paths are added. Returns a malloc'd table (*out_len) or
   NULL when table is not a RESTBL. */
uint8_t *mods_rstb_patch(const uint8_t *table, size_t len, const char *const *paths, const uint32_t *values, size_t n,
                         size_t *out_len, ModsRstbStats *stats);
uint32_t mods_crc32(const char *s);
/* A zstd frame of uncompressed ("raw") blocks: valid zstd without an encoder. Returns a malloc'd frame (*out_len). */
uint8_t *mods_zstd_raw_frame(const uint8_t *data, size_t len, size_t *out_len);

/* A mod file at a path the build's own RomFS patches also change (scripts/make_romfs_patches.py, the HeapDef overlay):
   MODS_BUILD_KEEP = the port needs its copy (System/HeapSize/: the heap sizes for bigger outputs), the mod's is left out;
   MODS_BUILD_MOD_WINS = the mod's copy is used (the game over flow fix only matters on 1.0.0);
   MODS_BUILD_REFIX = the mod's copy is used with the build's fix applied to it again (mods_capture_fix): the Common
   layout archive, which UI mods (button glyphs, HUD) replace. */
enum { MODS_BUILD_MOD_WINS = 0, MODS_BUILD_KEEP = 1, MODS_BUILD_REFIX = 2 };
int mods_build_collision(const char *path);
/* The quick-menu background fix on a decompressed Common layout archive (UI/LayoutArchive/Common.Product.<v>.Nin_NX_NVN
   .blarc, a SARC): in anim/ShortCutBG_00_In.bflan (C_CaptureBG_00, C_CaptureClearBG_00, P_CaptureClearBG_00) and
   anim/AutoBuilder_00_In.bflan (C_CaptureBG_00) the visibility keys [(0.0, hidden), (1.0, shown)] become
   [(0.0, hidden), (0.0, shown)], so the screen capture behind the menu is taken while the world still draws (at 60 fps
   frame 1.0 comes after it stopped: a black background). All or nothing: returns the number of keys moved (4 on the
   game's archive and on mods that keep those animations; 0 when all four already are at 0.0), or -1 with why filled
   when the archive's shape differs (a member, pane or key list not as expected; nothing is changed). */
int mods_capture_fix(uint8_t *sarc, size_t len, char *why, size_t cap);
/* The port's own edits of that archive carried over to a mod's copy (all of them: the quick-menu fix and the layouts for the
   output's shape, such as Native 16:10's taller canvas). The edits are values changed in place, so per member the port's copy
   (port) is compared with the game's (orig) word by word; each changed word the mod (sarc, edited in place) still has as the
   game ships it takes the port's value, a word the mod changed as well keeps the mod's. A member the mod resized or lacks is
   left as the mod has it. Returns the words written (st has the counts), or -1 with why filled when an archive is no SARC. */
typedef struct { unsigned members, skipped, words, kept; } ModsMerge;
int mods_port_merge(uint8_t *sarc, size_t len, const uint8_t *orig, size_t orig_len, const uint8_t *port, size_t port_len,
                    ModsMerge *st, char *why, size_t cap);
