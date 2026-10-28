/* Player mods (mods.h): the mods folder, mods.txt, what each mod holds, and the ResourceSizeTable the loader serves for
   them. Plain C with no game state: runtime/romfs.c hands in its RomFS lookup (mods_set_romfs_lookup) and loads the
   files mods_files lists; the menus read mods_get; tests/test_mods.c builds this file on its own. */
#include "mods.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#define MAX_MODS 512
#define MAX_DEPTH 3           /* how deep a romfs/ (or bare RomFS folders) is looked for: <Mod>/<a>/<b>/romfs */
#define MAX_FILES 400000

typedef struct { char *path, *file; uint64_t size; int mod, replaces; } Rec;
typedef struct { ModInfo info; char root[4096], cheats[4096]; size_t first, count; int listed; } Mod;   /* root: the RomFS folder, "" = none; cheats: its cheats/ folder, "" = none; listed: named in mods.txt */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char dir_path[4096];
static Mod *mods; static int nmods;
static Rec *recs; static size_t nrecs, rec_cap;      /* every mod's files, by mod */
static ModFile *winners; static size_t nwinners;     /* the enabled mods' files, each path once */
static ModsRomfsLookup romfs_lookup;
static char *loaded;                                 /* the enabled list the game started with (mods_mark_loaded) */

/* The RomFS's top-level folders (1.4.3): a mod folder whose top level holds some of these is a romfs/ itself. */
static const char *const ROOTS[] = {"AI","AISchedule","AS","Actor","ActorSystem","AnimationEvent","BSA","Bake","Banc",
 "CameraAnimation","Cave","CaveBounding","Chemical","Component","Cooking","Cull","ELink2","Ecosystem","Effect","Env",
 "Event","Font","Forest","Game","GameBalance","GameData","GameParameter","GrassStats","Lib","Life","Logic","Mals",
 "Model","Pack","Perception","Phive","Preload","PrequelSaveData","RSDB","Rumble","SLink2","Scene","Sequence","Shader",
 "Sound","System","TerrainArc","TexToGo","UI","UMii","Voice","VolumeStats","WorldMgr"};

static int ends_with_ci(const char *s, const char *tail) {
    size_t n = strlen(s), m = strlen(tail); return n >= m && !strcasecmp(s + n - m, tail);
}
static void copy(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src ? src : ""); }

const char *mods_dir(void) {
    pthread_mutex_lock(&lock);
    if (!*dir_path) {
        const char *env = getenv("TOTK_MODS_DIR"), *home = getenv("HOME");
        if (env && *env) copy(dir_path, sizeof dir_path, env);
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
        else snprintf(dir_path, sizeof dir_path, "%s/Documents/mods", home ? home : ".");
#else
        else snprintf(dir_path, sizeof dir_path, "%s/Library/Application Support/MacNativeGames/TOTK/mods", home ? home : ".");
#endif
        if (strcmp(dir_path, "0")) {   /* mkdir -p */
            char p[4096]; copy(p, sizeof p, dir_path);
            for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
            mkdir(p, 0755);
        }
    }
    pthread_mutex_unlock(&lock);
    return dir_path;
}

/* ---- mod.json: a forgiving reader for the few flat fields mods.h names. Walks every token; a string, number or list of
   strings right after a key it knows fills that field (the first one found wins). ---- */
static const char *json_string(const char *p, char *out, size_t cap) {   /* p at the opening quote */
    size_t n = 0; p++;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            c = *p++;
            if (c == 'n' || c == 't' || c == 'r') c = ' ';
            else if (c == 'u') { unsigned v = 0; for (int i = 0; i < 4 && isxdigit((unsigned char)*p); i++, p++) v = v*16 + (unsigned)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
                if (v < 0x80) c = (char)v;
                else { char u[3]; int k = 0; if (v < 0x800) { u[k++] = (char)(0xC0 | v >> 6); } else { u[k++] = (char)(0xE0 | v >> 12); u[k++] = (char)(0x80 | ((v >> 6) & 63)); }
                    for (int i = 0; i < k; i++) if (n + 1 < cap) out[n++] = u[i]; c = (char)(0x80 | (v & 63)); } }
        }
        if (n + 1 < cap) out[n++] = c;
    }
    if (cap) out[n] = 0;
    return *p ? p + 1 : p;
}
static int json_key(const char *k, const char *const *names) { for (; *names; names++) if (!strcmp(k, *names)) return 1; return 0; }
/* A button-icon style a text names: "xbox" -> Xbox, "playstation" / "ps4" / "ps5" / "dualsense" / "dualshock" ->
   PlayStation; both or neither -> 0 (a "Controller UI (Xbox&PS)" mod is a choice inside, not one style). */
static int prompts_style(const char *text) {
    char low[600]; size_t n = 0; for (; text[n] && n < sizeof low - 1; n++) low[n] = (char)tolower((unsigned char)text[n]); low[n] = 0;
    int xbox = strstr(low, "xbox") != NULL;
    int ps = strstr(low, "playstation") || strstr(low, "ps4") || strstr(low, "ps5") || strstr(low, "dualsense") || strstr(low, "dualshock");
    return xbox && !ps ? MODS_PROMPTS_XBOX : ps && !xbox ? MODS_PROMPTS_PLAYSTATION : 0;
}
static int prompt_choice;   /* mods_set_prompts */
void mods_set_prompts(int style) { pthread_mutex_lock(&lock); prompt_choice = style; pthread_mutex_unlock(&lock); }
static void read_mod_json(const char *path, ModInfo *m) {
    FILE *f = fopen(path, "rb"); if (!f) return;
    char *text = malloc(1 << 20); size_t n = text ? fread(text, 1, (1 << 20) - 1, f) : 0; fclose(f); if (!text) return; text[n] = 0;
    static const char *const NAME[] = {"name","title",NULL}, *const AUTHOR[] = {"author","submitter",NULL}, *const VERSION[] = {"version",NULL},
        *const GAMES[] = {"game_versions","gameVersions","game_version",NULL}, *const GB[] = {"gamebanana_id","gamebananaId","gamebanana",NULL},
        *const PROMPTS[] = {"prompts",NULL};
    int have_name = 0, have_author = 0, have_version = 0, have_games = 0, have_gb = 0;
    char key[64] = "", s[512];
    for (const char *p = text; *p;) {
        while (*p && strchr(" \t\r\n,:{}", *p)) p++;
        if (!*p) break;
        if (*p == '"') {
            p = json_string(p, s, sizeof s);
            const char *q = p; while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
            if (*q == ':') { copy(key, sizeof key, s); p = q + 1; continue; }   /* a key */
            if (json_key(key, NAME) && !have_name) { copy(m->name, sizeof m->name, s); have_name = 1; }
            else if (json_key(key, AUTHOR) && !have_author) { copy(m->author, sizeof m->author, s); have_author = 1; }
            else if (json_key(key, VERSION) && !have_version) { copy(m->version, sizeof m->version, s); have_version = 1; }
            else if (json_key(key, GAMES) && !have_games) { copy(m->game_versions, sizeof m->game_versions, s); have_games = 1; }
            else if (json_key(key, GB) && !have_gb && isdigit((unsigned char)*s)) { m->gamebanana_id = strtoll(s, NULL, 10); have_gb = 1; }
            else if (json_key(key, PROMPTS)) m->prompts = prompts_style(s);
            *key = 0;
        } else if (*p == '[') {   /* a list: game versions are joined, anything else skipped */
            int games = json_key(key, GAMES) && !have_games; *key = 0; p++;
            int depth = 1;
            while (*p && depth) {
                if (*p == '"') { p = json_string(p, s, sizeof s);
                    if (games && depth == 1 && strlen(m->game_versions) + strlen(s) + 3 < sizeof m->game_versions) {
                        if (*m->game_versions) strcat(m->game_versions, ", ");
                        strcat(m->game_versions, s); } continue; }
                if (*p == '[' || *p == '{') depth++; else if (*p == ']' || *p == '}') depth--;
                p++;
            }
            if (games && *m->game_versions) have_games = 1;
        } else {   /* a number, true / false / null */
            const char *start = p; while (*p && !strchr(" \t\r\n,:{}[]\"", *p)) p++;
            if (p == start) p++;   /* a stray ] */
            if (json_key(key, GB) && !have_gb && isdigit((unsigned char)*start)) { m->gamebanana_id = strtoll(start, NULL, 10); have_gb = 1; }
            *key = 0;
        }
    }
    free(text);
}

/* Game versions a text names, as a bit set of minor versions (1.0 = bit 0 ... 1.4 = bit 4); three_part: only "1.x.y". */
static unsigned versions_named(const char *s, int three_part) {
    unsigned bits = 0;
    for (const char *p = s; *p; p++) {
        if (p[0] != '1' || p[1] != '.' || !isdigit((unsigned char)p[2]) || (p > s && (isdigit((unsigned char)p[-1]) || p[-1] == '.'))) continue;
        if (isdigit((unsigned char)p[3])) continue;   /* 1.10 and up: not a game version */
        int third = p[3] == '.' && (isdigit((unsigned char)p[4]) || tolower((unsigned char)p[4]) == 'x');
        if (three_part && !third) continue;
        int minor = p[2] - '0'; if (minor <= 4) bits |= 1u << minor;
    }
    return bits;
}
static int old_version(const ModInfo *m) {
    unsigned named = versions_named(m->game_versions, 0);
    if (!named) named = versions_named(m->folder, 1) | versions_named(m->name, 1);
    return named && !(named & (3u << 3));   /* names 1.0-1.2 and not 1.3 / 1.4 */
}

/* ---- the folder's layout ---- */
static int is_root_name(const char *name) {
    for (size_t i = 0; i < sizeof ROOTS / sizeof *ROOTS; i++) if (!strcasecmp(name, ROOTS[i])) return 1;
    return 0;
}
static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
typedef struct { char path[4096]; int depth; } Cand;
/* Looks for romfs/ folders (and folders whose entries are RomFS top-level names) under p; also notes exefs/, .tkcl and
   the shallowest cheats/ folder (Yuzu / Ryujinx <Mod>/cheats, or Atmosphère's atmosphere/contents/<title>/cheats). */
static void find_roots(const char *p, int depth, Cand *romfs, int *nromfs, Cand *bare, int *nbare, unsigned *flags, Cand *cheats) {
    DIR *d = opendir(p); if (!d) return;
    struct dirent *e; char sub[4096]; int roots_here = 0;
    char (*dirs)[256] = malloc(64 * 256); int ndirs = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (snprintf(sub, sizeof sub, "%s/%s", p, e->d_name) >= (int)sizeof sub) continue;
        if (!is_dir(sub)) {
            if (ends_with_ci(e->d_name, ".tkcl")) *flags |= MOD_TKCL;
            if (ends_with_ci(e->d_name, ".pchtxt") || ends_with_ci(e->d_name, ".ips")) *flags |= MOD_HAS_CODE;
            continue;
        }
        if (!strcasecmp(e->d_name, "romfs")) { if (*nromfs < 16) { copy(romfs[*nromfs].path, sizeof romfs->path, sub); romfs[(*nromfs)++].depth = depth; } continue; }
        if (!strcasecmp(e->d_name, "exefs")) { *flags |= MOD_HAS_CODE; continue; }
        if (!strcasecmp(e->d_name, "cheats")) { if (!*cheats->path || depth < cheats->depth) { copy(cheats->path, sizeof cheats->path, sub); cheats->depth = depth; } continue; }
        if (is_root_name(e->d_name)) { roots_here = 1; continue; }
        if (dirs && ndirs < 64) copy(dirs[ndirs++], 256, e->d_name);
    }
    closedir(d);
    if (roots_here && *nbare < 16) { copy(bare[*nbare].path, sizeof bare->path, p); bare[(*nbare)++].depth = depth; }
    if (depth < MAX_DEPTH && !roots_here)
        for (int i = 0; i < ndirs; i++) { snprintf(sub, sizeof sub, "%s/%s", p, dirs[i]); find_roots(sub, depth + 1, romfs, nromfs, bare, nbare, flags, cheats); }
    free(dirs);
}
/* The shallowest candidate when it is the only one at its depth; "" otherwise. */
static int pick(const Cand *c, int n, char *out, size_t cap, const char *folder) {
    if (!n) return 0;
    int best = 0, ties = 0;
    for (int i = 1; i < n; i++) if (c[i].depth < c[best].depth) best = i;
    for (int i = 0; i < n; i++) ties += c[i].depth == c[best].depth;
    if (ties > 1) { fprintf(stderr, "[totk] mods: %s holds %d RomFS folders side by side (options?); keep one of them\n", folder, ties); return -1; }
    copy(out, cap, c[best].path); return 1;
}

static void add_rec(const char *path, const char *file, uint64_t size, int mod, int replaces) {
    if (nrecs == MAX_FILES) return;
    if (nrecs == rec_cap) { rec_cap = rec_cap ? rec_cap * 2 : 1024; Rec *r = realloc(recs, rec_cap * sizeof *recs); if (!r) { rec_cap = nrecs; return; } recs = r; }
    char *a = strdup(path), *b = strdup(file); if (!a || !b) { free(a); free(b); return; }
    recs[nrecs++] = (Rec){a, b, size, mod, replaces};
}
static int is_rstb(const char *rel) { return !strncasecmp(rel, "System/Resource/", 16) && strcasestr(rel, ".rsizetable"); }
static int junk(const char *name) { return name[0] == '.' || !strcasecmp(name, "desktop.ini") || !strcasecmp(name, "Thumbs.db"); }
/* Every file under the RomFS folder, as RomFS paths; returns how many. */
static void walk(Mod *m, int index, const char *base, const char *rel, int depth) {
    char p[4096]; if (snprintf(p, sizeof p, "%s%s%s", base, *rel ? "/" : "", rel) >= (int)sizeof p) return;
    DIR *d = opendir(p); if (!d || depth > 32) { if (d) closedir(d); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (junk(e->d_name)) continue;
        char r[4096], f[4096]; struct stat st;
        if (snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", e->d_name) >= (int)sizeof r) continue;
        if (snprintf(f, sizeof f, "%s/%s", base, r) >= (int)sizeof f || stat(f, &st)) continue;
        if (S_ISDIR(st.st_mode)) { walk(m, index, base, r, depth + 1); continue; }
        if (!S_ISREG(st.st_mode)) continue;
        if (is_rstb(r)) { m->info.flags |= MOD_OWN_RSTB; continue; }
        char real[4096]; int found = 1;
        if (romfs_lookup) {
            found = romfs_lookup(r, real, sizeof real);
            if (found == 2) { fprintf(stderr, "[totk] mods: %s: %s is spelt %s in the game (paths are case-sensitive); using the game's spelling\n", m->info.folder, r, real); copy(r, sizeof r, real); }
        }
        add_rec(r, f, (uint64_t)st.st_size, index, found != 0);
        m->info.files++; m->info.bytes += (uint64_t)st.st_size;
    }
    closedir(d);
}

/* A file the mod adds whose name carries another game version's number where the game has the same file under its own
   (UI/LayoutArchive/Common.Product.110... beside the game's .140): the mod was made for that other version, whatever
   its name says (balinwon's "Xbox UI v2" for 1.2.x added the 1.1/1.2 icon archive and replaced the fonts with 1.2's). */
static int other_version_files(const Mod *m) {
    static const char *const nums[] = {"100", "110", "112", "120", "121", "130", "140", "141", "142", "143"};
    if (!romfs_lookup) return 0;
    for (size_t r = m->first; r < m->first + m->count; r++) {
        if (recs[r].replaces) continue;
        const char *at = strstr(recs[r].path, ".Product."); if (!at) continue;
        const char *d = at + 9;
        if (!isdigit((unsigned char)d[0]) || !isdigit((unsigned char)d[1]) || !isdigit((unsigned char)d[2]) || d[3] != '.') continue;
        for (size_t k = 0; k < sizeof nums / sizeof *nums; k++) {
            if (!strncmp(d, nums[k], 3)) continue;
            char alt[1024], real[1024]; if (snprintf(alt, sizeof alt, "%s", recs[r].path) >= (int)sizeof alt) break;
            memcpy(alt + (d - recs[r].path), nums[k], 3);
            if (romfs_lookup(alt, real, sizeof real)) {
                fprintf(stderr, "[totk] mods: %s: %s is for another game version (this game has %s)\n", m->info.name, recs[r].path, alt);
                return 1;
            }
        }
    }
    return 0;
}
static void analyse(Mod *m, int index) {
    char p[4096]; ModInfo *i = &m->info;
    if (snprintf(p, sizeof p, "%s/%s", dir_path, i->folder) >= (int)sizeof p) return;
    copy(i->name, sizeof i->name, i->folder);
    if (ends_with_ci(i->folder, ".tkcl")) { i->flags |= MOD_TKCL | MOD_EMPTY; return; }
    char json[4200]; snprintf(json, sizeof json, "%s/mod.json", p); read_mod_json(json, i);
    if (!*i->name) copy(i->name, sizeof i->name, i->folder);
    Cand *romfs = malloc(16 * sizeof *romfs), *bare = malloc(16 * sizeof *bare), *cheats = calloc(1, sizeof *cheats); int nr = 0, nb = 0;
    if (romfs && bare && cheats) {
        find_roots(p, 0, romfs, &nr, bare, &nb, &i->flags, cheats);
        copy(m->cheats, sizeof m->cheats, cheats->path);
        int got = pick(romfs, nr, m->root, sizeof m->root, i->folder);
        if (!got) got = pick(bare, nb, m->root, sizeof m->root, i->folder);
        if (got < 0) *m->root = 0;
    }
    free(romfs); free(bare); free(cheats);
    if (*m->cheats) i->flags |= MOD_HAS_CHEATS;
    m->first = nrecs;
    if (*m->root) walk(m, index, m->root, "", 0);
    m->count = nrecs - m->first;
    if (i->files) i->flags |= MOD_HAS_ROMFS; else if (!(i->flags & MOD_HAS_CHEATS)) i->flags |= MOD_EMPTY;   /* a cheats-only mod is not empty */
    if (old_version(i) || other_version_files(m)) i->flags |= MOD_OLD_VERSION;
    /* Button icons: only a mod that changes the icon archive counts, named by mod.json or by its name. */
    int icons = 0;
    for (size_t r = m->first; r < m->first + m->count && !icons; r++) icons = recs[r].replaces && !strncmp(recs[r].path, "UI/LayoutArchive/Common.Product.", 32);   /* this game's archive, not another version's */
    if (!icons) i->prompts = 0;
    else if (!i->prompts) { char both[600]; snprintf(both, sizeof both, "%s %s", i->name, i->folder); i->prompts = prompts_style(both); }
    if (i->prompts) i->flags |= MOD_PROMPTS;
}

/* ---- the list ---- */
static int legacy_folder(const char *name) {   /* mods/v143/manifest.tsv: the older single folder, loaded by TOTK_ROMFS_MODS */
    char p[4096]; struct stat st;
    return snprintf(p, sizeof p, "%s/%s/manifest.tsv", dir_path, name) < (int)sizeof p && !stat(p, &st) && S_ISREG(st.st_mode);
}
static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void clear(void) {
    for (size_t k = 0; k < nrecs; k++) { free(recs[k].path); free(recs[k].file); }
    nrecs = 0; nwinners = 0; nmods = 0;
}
static int find(const char *folder) { for (int k = 0; k < nmods; k++) if (!strcmp(mods[k].info.folder, folder)) return k; return -1; }
static int add_mod(const char *folder, int enabled) {
    if (nmods == MAX_MODS || find(folder) >= 0) return -1;
    Mod *m = &mods[nmods]; memset(m, 0, sizeof *m);
    copy(m->info.folder, sizeof m->info.folder, folder); m->info.enabled = enabled;
    return nmods++;
}

/* Paths claimed by enabled mods, earlier first: an open-addressed set over recs indices. */
static uint64_t hash_str(const char *s) { uint64_t h = 1469598103934665603ull; while (*s) h = (h ^ (unsigned char)*s++) * 1099511628211ull; return h; }
static void resolve(void) {
    free(winners); winners = NULL; nwinners = 0;
    size_t cap = 16; while (cap < nrecs * 2 + 16) cap *= 2;
    size_t *set = calloc(cap, sizeof *set); winners = malloc((nrecs + 1) * sizeof *winners); if (!set || !winners) { free(set); return; }
    for (int k = 0; k < nmods; k++) { mods[k].info.conflicts = 0; mods[k].info.flags &= ~(unsigned)MOD_CONFLICT; }
    for (int k = 0; k < nmods; k++) {
        Mod *m = &mods[k];
        if (!m->info.enabled) continue;   /* a mod that is off conflicts with nothing (an archive's other options) */
        for (size_t r = m->first; r < m->first + m->count; r++) {
            size_t h = (size_t)hash_str(recs[r].path) & (cap - 1); int seen = 0;
            for (; set[h]; h = (h + 1) & (cap - 1)) if (!strcmp(recs[set[h] - 1].path, recs[r].path)) { seen = 1; break; }
            if (seen) { if (recs[set[h] - 1].mod != k) { m->info.conflicts++; m->info.flags |= MOD_CONFLICT; } continue; }
            set[h] = r + 1;
            winners[nwinners++] = (ModFile){recs[r].path, recs[r].file, recs[r].size, k, recs[r].replaces};
        }
    }
    free(set);
}

int mods_scan(void) {
    mods_dir();
    pthread_mutex_lock(&lock);
    clear();
    if (!mods) mods = calloc(MAX_MODS, sizeof *mods);
    if (!mods || !strcmp(dir_path, "0")) { pthread_mutex_unlock(&lock); return 0; }
    /* What is in the folder. */
    char **names = NULL; size_t nnames = 0; DIR *d = opendir(dir_path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' || !strcmp(e->d_name, "mods.txt")) continue;
            char p[4096]; if (snprintf(p, sizeof p, "%s/%s", dir_path, e->d_name) >= (int)sizeof p) continue;
            if (!(is_dir(p) || ends_with_ci(e->d_name, ".tkcl")) || legacy_folder(e->d_name)) continue;
            char **n = realloc(names, (nnames + 1) * sizeof *names); if (!n) break; names = n;
            if ((names[nnames] = strdup(e->d_name))) nnames++;
        }
        closedir(d);
    }
    qsort(names, nnames, sizeof *names, by_name);
    /* mods.txt: the order; a folder it names that is gone is dropped. */
    char p[4200]; snprintf(p, sizeof p, "%s/mods.txt", dir_path); FILE *f = fopen(p, "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            char *s = line, *end; while (*s == ' ' || *s == '\t') s++;
            end = s + strlen(s); while (end > s && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
            if (!*s || *s == '#') continue;
            int on = 1; if (*s == '-') { on = 0; s++; } else if (*s == '+') s++;
            for (size_t k = 0; k < nnames; k++) if (!strcmp(names[k], s)) { int at = add_mod(s, on); if (at >= 0) mods[at].listed = 1; break; }
        }
        fclose(f);
    }
    for (size_t k = 0; k < nnames; k++) { add_mod(names[k], 1); free(names[k]); }
    free(names);
    for (int k = 0; k < nmods; k++) analyse(&mods[k], k);
    /* A new mod made for another game version starts off: turning it on is the player's choice. */
    for (int k = 0; k < nmods; k++) if (!mods[k].listed && (mods[k].info.flags & MOD_OLD_VERSION)) mods[k].info.enabled = 0;
    if (prompt_choice) {   /* the Button prompts setting: one prompts mod of that style on, the others off */
        int chosen = -1;
        for (int k = 0; k < nmods; k++) if (mods[k].info.prompts && mods[k].info.prompts == prompt_choice && chosen < 0) chosen = k;
        for (int k = 0; k < nmods; k++) if (mods[k].info.prompts) mods[k].info.enabled = k == chosen;
    }
    resolve();
    int n = nmods; pthread_mutex_unlock(&lock);
    return n;
}

int mods_count(void) { pthread_mutex_lock(&lock); int n = nmods; pthread_mutex_unlock(&lock); return n; }
int mods_get(int i, ModInfo *out) {
    pthread_mutex_lock(&lock); int ok = i >= 0 && i < nmods && out; if (ok) *out = mods[i].info; pthread_mutex_unlock(&lock);
    return ok ? 0 : -1;
}
int mods_set_enabled(const char *folder, int on) {
    pthread_mutex_lock(&lock); int k = folder ? find(folder) : -1; if (k >= 0) { mods[k].info.enabled = !!on; resolve(); }
    pthread_mutex_unlock(&lock); return k >= 0 ? 0 : -1;
}
int mods_move(const char *folder, int to) {
    pthread_mutex_lock(&lock); int k = folder ? find(folder) : -1;
    if (k >= 0) {
        if (to < 0) to = 0; if (to >= nmods) to = nmods - 1;
        Mod m = mods[k];
        if (to < k) memmove(&mods[to + 1], &mods[to], (size_t)(k - to) * sizeof *mods);
        else memmove(&mods[k], &mods[k + 1], (size_t)(to - k) * sizeof *mods);
        mods[to] = m;
        /* recs keep their mod index for the conflict check: renumber and re-sort them by mod. */
        Rec *sorted = malloc((nrecs + 1) * sizeof *sorted); size_t n = 0;
        if (sorted) {
            for (int j = 0; j < nmods; j++) {
                size_t first = n;
                for (size_t r = mods[j].first; r < mods[j].first + mods[j].count; r++) { sorted[n] = recs[r]; sorted[n++].mod = j; }
                mods[j].first = first;
            }
            memcpy(recs, sorted, n * sizeof *recs); free(sorted);
        }
        resolve();
    }
    pthread_mutex_unlock(&lock); return k >= 0 ? 0 : -1;
}
static int remove_entry(const char *p, const struct stat *st, int type, struct FTW *ftw) { (void)st; (void)type; (void)ftw; return remove(p); }
int mods_remove(const char *folder) {
    if (!folder || !*folder || strchr(folder, '/') || !strcmp(folder, ".") || !strcmp(folder, "..")) return -1;
    mods_dir();
    pthread_mutex_lock(&lock);
    char p[4096]; int ok = snprintf(p, sizeof p, "%s/%s", dir_path, folder) < (int)sizeof p && strcmp(dir_path, "0");
    if (ok) {
        struct stat st; ok = !lstat(p, &st);
        if (ok) ok = S_ISDIR(st.st_mode) ? !nftw(p, remove_entry, 16, FTW_DEPTH | FTW_PHYS) : !remove(p);
    }
    int k = find(folder);
    if (ok && k >= 0) {   /* drop it from the list; its files go with it */
        size_t first = mods[k].first, count = mods[k].count;
        for (size_t r = first; r < first + count; r++) { free(recs[r].path); free(recs[r].file); }
        memmove(&recs[first], &recs[first + count], (nrecs - first - count) * sizeof *recs); nrecs -= count;
        memmove(&mods[k], &mods[k + 1], (size_t)(nmods - k - 1) * sizeof *mods); nmods--;
        for (int j = 0; j < nmods; j++) { if (mods[j].first > first) mods[j].first -= count;
            for (size_t r = mods[j].first; r < mods[j].first + mods[j].count; r++) recs[r].mod = j; }
        resolve();
    }
    pthread_mutex_unlock(&lock);
    return ok ? 0 : -1;
}
int mods_save(void) {
    mods_dir();
    pthread_mutex_lock(&lock);
    char p[4200], tmp[4300]; int ok = strcmp(dir_path, "0") != 0;
    snprintf(p, sizeof p, "%s/mods.txt", dir_path); snprintf(tmp, sizeof tmp, "%s.%ld.tmp", p, (long)getpid());
    FILE *f = ok ? fopen(tmp, "w") : NULL; ok = f != NULL;
    if (f) {
        fputs("# Mods load order: the first line wins a file two mods both change. \"-\" in front = installed but off.\n", f);
        for (int k = 0; k < nmods; k++) {
            const char *n = mods[k].info.folder;
            fprintf(f, "%s%s\n", !mods[k].info.enabled ? "-" : (*n == '-' || *n == '+' || *n == '#') ? "+" : "", n);
        }
        ok = !ferror(f); ok &= !fclose(f);
        if (ok) ok = !rename(tmp, p); else unlink(tmp);
    }
    pthread_mutex_unlock(&lock);
    return ok ? 0 : -1;
}
static char *enabled_list(void) {
    size_t cap = 1; for (int k = 0; k < nmods; k++) cap += strlen(mods[k].info.folder) + 1;
    char *s = malloc(cap); if (!s) return NULL; *s = 0;
    for (int k = 0; k < nmods; k++) if (mods[k].info.enabled) { strcat(s, mods[k].info.folder); strcat(s, "\n"); }
    return s;
}
void mods_mark_loaded(void) { pthread_mutex_lock(&lock); free(loaded); loaded = enabled_list(); pthread_mutex_unlock(&lock); }
int mods_changed_since_start(void) {
    pthread_mutex_lock(&lock); int changed = 0;
    if (loaded) { char *now = enabled_list(); changed = !now || strcmp(now, loaded); free(now); }
    pthread_mutex_unlock(&lock); return changed;
}
const char *mods_notice(unsigned flags) {
    if (flags & MOD_TKCL) return "A TKMM package (.tkcl): not supported yet. Use the mod's romfs folder instead.";
    if (flags & MOD_EMPTY) return "Nothing the game can load: no romfs folder (or several side by side: keep one).";
    if (flags & MOD_OLD_VERSION) return "Made for an older game version: may not work on 1.4.3.";
    if (flags & MOD_PROMPTS) return "Button icons: switched on or off by Controls › Button prompts (after a restart).";
    if (flags & MOD_CONFLICT) return "Changes files a mod higher in the list also changes: that mod's copies are used.";
    if (flags & MOD_HAS_CODE) return "Has code patches (exefs): they apply only when the app is built with --mod (docs/mods.md); its game files load either way.";
    if (flags & MOD_OWN_RSTB) return "Ships its own ResourceSizeTable: left out, the game's table is adjusted instead.";
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    if (flags & MOD_HAS_CHEATS) return "Has cheats: cheats work on the Mac for now, not on iPhone or iPad.";
#else
    if (flags & MOD_HAS_CHEATS) return "Has cheats: listed in Settings › Cheats while the mod is on.";
#endif
    if (flags & MOD_HAS_ROMFS) return "Game files: loaded at the next start.";
    return "";
}
int mods_cheat_file(int i, const char *build_id, char *out, size_t cap) {
    if (!build_id || !*build_id || !out || !cap) return 0;
    pthread_mutex_lock(&lock);
    char dir[4096] = ""; if (i >= 0 && i < nmods) copy(dir, sizeof dir, mods[i].cheats);
    pthread_mutex_unlock(&lock);
    *out = 0;
    if (!*dir) return 0;
    char want[64]; if (snprintf(want, sizeof want, "%s.txt", build_id) >= (int)sizeof want) return 0;
    DIR *d = opendir(dir); if (!d) return 0;
    struct dirent *e; int found = 0;
    while (!found && (e = readdir(d))) {   /* the build ID's case varies between cheat databases */
        char p[4400]; struct stat st;
        if (strcasecmp(e->d_name, want) || snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p || stat(p, &st) || !S_ISREG(st.st_mode)) continue;
        found = snprintf(out, cap, "%s", p) < (int)cap;
    }
    closedir(d);
    if (!found) *out = 0;
    return found;
}
void mods_set_romfs_lookup(ModsRomfsLookup lookup) { pthread_mutex_lock(&lock); romfs_lookup = lookup; pthread_mutex_unlock(&lock); }
int mods_files(const ModFile **out) { pthread_mutex_lock(&lock); if (out) *out = winners; int n = (int)nwinners; pthread_mutex_unlock(&lock); return n; }

/* ---- ResourceSizeTable ---- */
uint32_t mods_crc32(const char *s) {
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++) { c ^= (unsigned char)*s; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}
static void canonical(const char *path, char *out, size_t cap) {   /* the table's name: without .zs / .mc */
    size_t n = strlen(path); if (n > 3 && (!strcmp(path + n - 3, ".zs") || !strcmp(path + n - 3, ".mc"))) n -= 3;
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n); out[n] = 0;
}
static const char *ext_of(const char *path, size_t n) {   /* Path.GetExtension over the first n characters */
    for (size_t k = n; k-- > 0;) { if (path[k] == '/') break; if (path[k] == '.') return path + k; }
    return path + n;
}
static void extension(const char *path, char *out, size_t cap) {   /* TkSharp's GetResourceExtension */
    size_t n = strlen(path), m = n;
    if (n > 15 && !strcmp(path + n - 15, ".casset.byml.zs")) { copy(out, cap, ".casset.byml"); return; }
    if (n > 6 && !strcmp(path + n - 6, ".ta.zs")) m = n;
    else if (n > 3 && !strcmp(path + n - 3, ".zs")) m = n - 3;
    const char *e = ext_of(path, m); size_t len = (size_t)(path + m - e); if (len >= cap) len = cap - 1;
    memcpy(out, e, len); out[len] = 0;
}
static int rd32(const uint8_t *d, size_t len, uint64_t off, uint32_t *v) {
    if (off > len || len - off < 4) return 0;
    *v = (uint32_t)d[off] | (uint32_t)d[off+1] << 8 | (uint32_t)d[off+2] << 16 | (uint32_t)d[off+3] << 24; return 1;
}
/* The EXB section's signature count of an .ainb / .asb (0 without one); -1 = out of bounds. */
static int64_t exb_signatures(const uint8_t *d, size_t len, uint32_t exb) {
    uint32_t at, count; if (!exb) return 0;
    if (!rd32(d, len, (uint64_t)exb + 0x20, &at) || !rd32(d, len, (uint64_t)exb + at, &count)) return -1;
    return count;
}
int mods_rstb_needs_data(const char *path) {
    char e[64]; extension(path, e, sizeof e);
    return !strcmp(e, ".ainb") || !strcmp(e, ".asb") || !strcmp(e, ".bstar") || !strcmp(e, ".mc");
}
/* Ported from TkSharp (TkSharp.Merging/ResourceSizeTable/TkResourceSizeCollector.cs and the Calculators folder,
   github.com/TKMM-Team/TkSharp, MIT, Copyright (c) 2024 TKMM-Team and Contributors; THIRD_PARTY_NOTICES.md). One
   difference: TkSharp adds fileSize.AlignUp(0x20) to the size; the 1.4.3 table itself holds align32(size) + the type's
   constant (0x100 for most), so the size is rounded up to 32 here, which is what that line means to do. */
uint32_t mods_rstb_estimate(const char *path, uint64_t size, const uint8_t *data, size_t len) {
    char canon[1024], e[64]; canonical(path, canon, sizeof canon); extension(path, e, sizeof e);
    if (!strcmp(canon, "Pack/ZsDic.pack") || !strcmp(e, ".rsizetable") || !strcmp(e, ".bwav") || !strcmp(e, ".webm")) return 0;
    uint64_t s = (size + 31) & ~(uint64_t)31, r;
    static const struct { const char *name; unsigned add; } FIXED[] = {
        {"Event/EventFlow/Dm_ED_0004.bfevfl",0x1E0},{"Effect/static.Nin_NX_NVN.esetb.byml",0x1000},
        {"Effect/static.Product.110.Nin_NX_NVN.esetb.byml",0x1000},{"Lib/agl/agl_resource.Nin_NX_NVN.release.sarc",0x1000},
        {"Lib/gsys/gsys_resource.Nin_NX_NVN.release.sarc",0x1000},{"Lib/Terrain/tera_resource.Nin_NX_NVN.release.sarc",0x1000},
        {"Shader/ApplicationPackage.Nin_NX_NVN.release.sarc",0x1000}};
    static const struct { const char *ext; unsigned add; } ADD[] = {
        {".baatarc",0x100},{".baev",0x120},{".bagst",0x100},{".bars",0x240},{".bcul",0x100},{".beco",0x100},{".belnk",0x100},
        {".bfarc",0x100},{".bfevfl",0x120},{".bfsha",0x100},{".bhtmp",0x100},{".blal",0x100},{".blarc",0x100},{".blwp",0x100},
        {".bnsh",0x100},{".bntx",0x100},{".bphcl",0x100},{".bphhb",0x100},{".bphnm",0x120},{".bphsh",0x170},{".bslnk",0x100},
        {".byml",0x100},{".cai",0x100},{".casset.byml",0x1C0},{".chunk",0x100},{".crbin",0x100},{".cutinfo",0x100},{".dpi",0x100},
        {".genvb",0x180},{".jpg",0x100},{".pack",0x180},{".png",0x100},{".quad",0x100},{".sarc",0x180},{".tscb",0x100},
        {".txtg",0x100},{".txt",0x100},{".vsts",0x100},{".wbr",0x100},{".zs",0x100}};
    for (size_t k = 0; k < sizeof FIXED / sizeof *FIXED; k++) if (!strcmp(canon, FIXED[k].name)) { r = s + FIXED[k].add; goto done; }
    r = (s + 1500) * 4;   /* any other type */
    if (!strcmp(e, ".bgyml")) r = (s + 2000) * 8;
    else if (!strcmp(e, ".ainb") && data) {
        uint32_t exb; int64_t sig = rd32(data, len, 0x44, &exb) ? exb_signatures(data, len, exb) : -1;
        if (sig >= 0) r = s + 392 + 16 + (uint64_t)(sig + 1) / 2 * 8;
    } else if (!strcmp(e, ".asb") && data) {
        uint32_t nodes, exb; int64_t sig = rd32(data, len, 0x10, &nodes) && rd32(data, len, 0x60, &exb) ? exb_signatures(data, len, exb) : -1;
        if (sig >= 0) r = s + 552 + 40 * (uint64_t)nodes + (exb ? 16 + (uint64_t)(sig + 1) / 2 * 8 : 0);
    } else if (!strcmp(e, ".bstar") && data) {
        uint32_t count; if (rd32(data, len, 8, &count)) r = s + 0x120 + (uint64_t)count * 8;
    } else if (!strcmp(e, ".mc") && data) {
        uint32_t flags; if (rd32(data, len, 8, &flags)) { int32_t fl = (int32_t)flags; int64_t m = (int64_t)(fl >> 5) * ((int64_t)1 << (fl & 0xF)); r = m > 0 ? (uint64_t)((double)m * 2.55) : r; }
    } else for (size_t k = 0; k < sizeof ADD / sizeof *ADD; k++) if (!strcmp(e, ADD[k].ext)) { r = s + ADD[k].add; break; }
done:
    return r > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)r;
}

typedef struct { uint32_t crc, size; } Crc;
static int crc_order(const void *a, const void *b) { uint32_t x = ((const Crc *)a)->crc, y = ((const Crc *)b)->crc; return (x > y) - (x < y); }
static size_t name_size;
static int name_order(const void *a, const void *b) { return strncmp(a, b, name_size); }
static Crc *crc_find(Crc *t, size_t n, uint32_t crc) {
    size_t lo = 0, hi = n; while (lo < hi) { size_t m = lo + (hi - lo) / 2; if (t[m].crc < crc) lo = m + 1; else hi = m; }
    return lo < n && t[lo].crc == crc ? &t[lo] : NULL;
}
uint8_t *mods_rstb_patch(const uint8_t *table, size_t len, const char *const *paths, const uint32_t *values, size_t n,
                         size_t *out_len, ModsRstbStats *stats) {
    ModsRstbStats st = {0};
    uint32_t version, strsz, ncrc, nname;
    if (len < 22 || memcmp(table, "RESTBL", 6) || !rd32(table, len, 6, &version) || !rd32(table, len, 10, &strsz) ||
        !rd32(table, len, 14, &ncrc) || !rd32(table, len, 18, &nname) || strsz < 8 || strsz > 4096 ||
        (uint64_t)22 + (uint64_t)ncrc * 8 + (uint64_t)nname * (strsz + 4) != len) return NULL;
    size_t rec = strsz + 4;
    Crc *crcs = malloc(((size_t)ncrc + n + 1) * sizeof *crcs); uint8_t *names = malloc(((size_t)nname + n + 1) * rec);
    char **extra_name = calloc(n + 1, sizeof *extra_name); Crc *extra = crcs ? crcs + ncrc : NULL; size_t nextra = 0, nn = nname;
    if (!crcs || !names || !extra_name) { free(crcs); free(names); free(extra_name); return NULL; }
    for (uint32_t k = 0; k < ncrc; k++) { rd32(table, len, 22 + (uint64_t)k * 8, &crcs[k].crc); rd32(table, len, 26 + (uint64_t)k * 8, &crcs[k].size); }
    memcpy(names, table + 22 + (size_t)ncrc * 8, (size_t)nname * rec);
    char canon[1024];
    for (size_t i = 0; i < n; i++) {
        uint32_t v = values[i]; if (!v) { st.skipped++; continue; }
        canonical(paths[i], canon, sizeof canon);
        uint8_t *named = NULL;   /* the name table: paths whose CRC32 collides with another's */
        for (size_t k = 0; k < nn && !named; k++) if (!strncmp((char *)names + k * rec, canon, strsz)) named = names + k * rec;
        if (named) { uint32_t old; memcpy(&old, named + strsz, 4); if (v > old) { memcpy(named + strsz, &v, 4); st.raised++; } else st.kept++; continue; }
        uint32_t crc = mods_crc32(canon); Crc *c = crc_find(crcs, ncrc, crc);
        if (c) { if (v > c->size) { c->size = v; st.raised++; } else st.kept++; continue; }
        size_t k = 0; while (k < nextra && extra[k].crc != crc) k++;
        if (k < nextra && !strcmp(extra_name[k], canon)) { if (v > extra[k].size) { extra[k].size = v; st.raised++; } else st.kept++; continue; }
        if (k < nextra) {   /* two new paths, one CRC32: the second one goes to the name table */
            if (strlen(canon) >= strsz) { st.skipped++; continue; }
            named = names + nn++ * rec; memset(named, 0, rec); memcpy(named, canon, strlen(canon)); memcpy(named + strsz, &v, 4); st.added++; continue;
        }
        char *dup = strdup(canon); if (!dup) { st.skipped++; continue; }
        extra[nextra] = (Crc){crc, v}; extra_name[nextra++] = dup; st.added++;
    }
    for (size_t k = 0; k < nextra; k++) free(extra_name[k]);
    free(extra_name);
    size_t total = (size_t)ncrc + nextra; qsort(crcs, total, sizeof *crcs, crc_order);
    name_size = rec; qsort(names, nn, rec, name_order);
    size_t out_size = 22 + total * 8 + nn * rec; uint8_t *out = malloc(out_size);
    if (out) {
        memcpy(out, "RESTBL", 6); uint32_t h[4] = {version, strsz, (uint32_t)total, (uint32_t)nn}; memcpy(out + 6, h, 16);
        for (size_t k = 0; k < total; k++) { memcpy(out + 22 + k * 8, &crcs[k].crc, 4); memcpy(out + 26 + k * 8, &crcs[k].size, 4); }
        memcpy(out + 22 + total * 8, names, nn * rec);
        *out_len = out_size;
    }
    free(crcs); free(names);
    if (stats) *stats = st;
    return out;
}

uint8_t *mods_zstd_raw_frame(const uint8_t *data, size_t len, size_t *out_len) {
    /* Magic, a frame header with Single_Segment set and a 4-byte content size (FHD 0xA0; 8 bytes past 4 GB, FHD 0xE0),
       then raw blocks of at most 128 KB (the format's block limit), the last one flagged. */
    const size_t block = 128 << 10; size_t nblocks = len ? (len + block - 1) / block : 1;
    uint8_t *out = malloc(4 + 1 + 8 + nblocks * 3 + len); if (!out) return NULL;
    size_t o = 0; const uint32_t magic = 0xFD2FB528u;
    for (int k = 0; k < 4; k++) out[o++] = (uint8_t)(magic >> (8 * k));
    int wide = (uint64_t)len > 0xFFFFFFFFu; out[o++] = wide ? 0xE0 : 0xA0;
    for (int k = 0; k < (wide ? 8 : 4); k++) out[o++] = (uint8_t)((uint64_t)len >> (8 * k));
    size_t done = 0;
    do {
        size_t n = len - done < block ? len - done : block; uint32_t h = (uint32_t)n << 3 | (done + n == len ? 1u : 0u);   /* type 0 = raw */
        out[o++] = (uint8_t)h; out[o++] = (uint8_t)(h >> 8); out[o++] = (uint8_t)(h >> 16);
        memcpy(out + o, data + done, n); o += n; done += n;
    } while (done < len);
    *out_len = o; return out;
}

/* ---- the port's own patches ---- */
int mods_build_collision(const char *path) {
    if (!path) return MODS_BUILD_MOD_WINS;
    if (!strncmp(path, "System/HeapSize/", 16)) return MODS_BUILD_KEEP;
    static const char head[] = "UI/LayoutArchive/Common.Product.", tail[] = ".Nin_NX_NVN.blarc.zs";
    size_t n = strlen(path), h = sizeof head - 1, t = sizeof tail - 1;
    if (n > h + t && !strncmp(path, head, h) && !strcmp(path + n - t, tail)) {
        for (size_t k = h; k < n - t; k++) if (!isdigit((unsigned char)path[k])) return MODS_BUILD_MOD_WINS;
        return MODS_BUILD_REFIX;
    }
    return MODS_BUILD_MOD_WINS;
}
static unsigned le16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
/* A SARC member by name (*len its size), or NULL (absent, or not a SARC). */
static uint8_t *sarc_member(uint8_t *d, size_t n, const char *want, size_t *len) {
    if (n < 0x20 || memcmp(d, "SARC", 4) || d[6] != 0xFF || d[7] != 0xFE) return NULL;
    size_t hdr = le16(d + 4), data = le32(d + 0x0C);
    if (hdr + 12 > n || memcmp(d + hdr, "SFAT", 4)) return NULL;
    size_t count = le16(d + hdr + 6), sfnt = hdr + 12 + count * 16;
    if (sfnt + 8 > n || memcmp(d + sfnt, "SFNT", 4)) return NULL;
    for (size_t k = 0; k < count; k++) {
        const uint8_t *node = d + hdr + 12 + k * 16; uint32_t attr = le32(node + 4), start = le32(node + 8), end = le32(node + 12);
        size_t at = sfnt + 8 + (size_t)(attr & 0xFFFFFF) * 4;
        if (!(attr >> 24) || at >= n || !memchr(d + at, 0, n - at) || strcmp((const char *)d + at, want)) continue;
        if (start > end || data + end > n) return NULL;
        *len = end - start; return d + data + start;
    }
    return NULL;
}
/* A BFLAN's visibility (FLVI) keys for the panes asked for (the layout scripts/make_romfs_patches.py's flvi_keys reads);
   -1 when the animation does not read. */
typedef struct { const char *pane; uint8_t *keys; unsigned nkeys; int tracks, step; } FlviPane;
static int flvi_panes(uint8_t *b, size_t n, FlviPane *panes, int np) {
    if (n < 0x14 || memcmp(b, "FLAN", 4) || b[4] != 0xFF || b[5] != 0xFE) return -1;
    size_t pos = 0x14; unsigned sections = le16(b + 0x10);
    for (unsigned s = 0; s < sections; s++) {
        if (pos + 8 > n) return -1;
        size_t size = le32(b + pos + 4); if (size < 8 || size > n - pos) return -1;
        if (!memcmp(b + pos, "pai1", 4)) {
            if (size < 20) return -1;
            size_t count = le16(b + pos + 14), table = pos + le32(b + pos + 16);
            for (size_t e = 0; e < count; e++) {
                if (table + 4 * e + 4 > n) return -1;
                size_t eo = pos + le32(b + table + 4 * e); if (eo + 32 > n) return -1;
                char name[29]; memcpy(name, b + eo, 28); name[28] = 0;
                int p = 0; while (p < np && strcmp(name, panes[p].pane)) p++;
                if (p == np) continue;
                for (unsigned t = 0; t < b[eo + 28]; t++) {
                    if (eo + 32 + 4 * t + 4 > n) return -1;
                    size_t to = eo + le32(b + eo + 32 + 4 * t); if (to + 8 > n) return -1;
                    if (memcmp(b + to, "FLVI", 4)) continue;
                    for (unsigned g = 0; g < b[to + 4]; g++) {
                        if (to + 8 + 4 * g + 4 > n) return -1;
                        size_t go = to + le32(b + to + 8 + 4 * g); if (go + 12 > n) return -1;
                        size_t nk = le16(b + go + 4), ko = go + le32(b + go + 8); if (ko > n || nk * 8 > n - ko) return -1;
                        panes[p].tracks++; panes[p].step = b[go + 2] == 1; panes[p].keys = b + ko; panes[p].nkeys = (unsigned)nk;
                    }
                }
            }
        }
        pos += size;
    }
    return 0;
}
int mods_capture_fix(uint8_t *sarc, size_t len, char *why, size_t cap) {
    static const struct { const char *member, *panes[3]; int np; } FIX[] = {
        {"anim/ShortCutBG_00_In.bflan", {"C_CaptureBG_00", "C_CaptureClearBG_00", "P_CaptureClearBG_00"}, 3},
        {"anim/AutoBuilder_00_In.bflan", {"C_CaptureBG_00", NULL, NULL}, 1}};
    char scratch[8]; if (!why || !cap) { why = scratch; cap = sizeof scratch; }
    *why = 0;
    uint8_t *move[4]; int nmove = 0;
    for (size_t f = 0; f < sizeof FIX / sizeof *FIX; f++) {
        size_t n = 0; uint8_t *m = sarc ? sarc_member(sarc, len, FIX[f].member, &n) : NULL;
        if (!m) { snprintf(why, cap, "no %s in the archive", FIX[f].member); return -1; }
        FlviPane p[3]; memset(p, 0, sizeof p); for (int k = 0; k < FIX[f].np; k++) p[k].pane = FIX[f].panes[k];
        if (flvi_panes(m, n, p, FIX[f].np)) { snprintf(why, cap, "%s does not read as a layout animation", FIX[f].member); return -1; }
        for (int k = 0; k < FIX[f].np; k++) {
            if (p[k].tracks != 1 || !p[k].step || p[k].nkeys != 2) {
                snprintf(why, cap, "%s: %s has %d visibility tracks, %u keys (one step track of 2 expected)", FIX[f].member, p[k].pane, p[k].tracks, p[k].nkeys); return -1; }
            float f0, f1; memcpy(&f0, p[k].keys, 4); memcpy(&f1, p[k].keys + 8, 4);
            unsigned v0 = le16(p[k].keys + 4), v1 = le16(p[k].keys + 12);
            if (f0 != 0.0f || v0 != 0 || v1 != 1 || (f1 != 1.0f && f1 != 0.0f)) {
                snprintf(why, cap, "%s: %s keys are (%g, %u), (%g, %u), not (0, 0), (1, 1)", FIX[f].member, p[k].pane, (double)f0, v0, (double)f1, v1); return -1; }
            if (f1 == 1.0f) move[nmove++] = p[k].keys + 8;
        }
    }
    const float zero = 0.0f; for (int k = 0; k < nmove; k++) memcpy(move[k], &zero, 4);
    return nmove;
}
/* ---- the port's edits on a member the mod rebuilt: matched by name instead of by offset ---- */
/* A named piece of a layout (a pane section) or of an animation (one target's key list): key = name (len bytes, NUL padded),
   tag (the section's or the animation tag's magic), id (an animation target: its index, target, curve and the entry's
   type); fixed = how many bytes from at still line up when the mod's copy of it has another size (a pane's base fields,
   an animation's keys by index). */
typedef struct { const uint8_t *name; unsigned len; uint32_t tag, id; uint8_t *at; size_t size, fixed; } Part;
enum { PANE_BASE = 84 };   /* a pane section's head: magic, size, flags, name, user data, translate, rotate, scale, size */
static size_t flyt_parts(uint8_t *b, size_t n, Part *out, size_t cap) {   /* the panes of a BFLYT, in order; (size_t)-1 = not one */
    static const char *const PANES[] = {"pan1", "pic1", "txt1", "wnd1", "bnd1", "prt1", "ali1", "scr1"};
    if (n < 0x14 || memcmp(b, "FLYT", 4) || b[4] != 0xFF || b[5] != 0xFE) return (size_t)-1;
    size_t pos = le16(b + 6), np = 0; unsigned sections = le16(b + 0x10);
    for (unsigned s = 0; s < sections; s++) {
        if (pos + 8 > n) return (size_t)-1;
        size_t size = le32(b + pos + 4); if (size < 8 || size > n - pos) return (size_t)-1;
        for (size_t k = 0; k < sizeof PANES / sizeof *PANES; k++)
            if (!memcmp(b + pos, PANES[k], 4) && size >= PANE_BASE) { if (np == cap) return (size_t)-1; out[np++] = (Part){b + pos + 12, 24, le32(b + pos), 0, b + pos, size, PANE_BASE}; break; }
        pos += size;
    }
    return np;
}
static size_t flan_parts(uint8_t *b, size_t n, Part *out, size_t cap) {   /* every key list of a BFLAN (step and hermite curves); (size_t)-1 = not one */
    if (n < 0x14 || memcmp(b, "FLAN", 4) || b[4] != 0xFF || b[5] != 0xFE) return (size_t)-1;
    size_t pos = 0x14, np = 0; unsigned sections = le16(b + 0x10);
    for (unsigned s = 0; s < sections; s++) {
        if (pos + 8 > n) return (size_t)-1;
        size_t size = le32(b + pos + 4); if (size < 8 || size > n - pos) return (size_t)-1;
        if (!memcmp(b + pos, "pai1", 4)) {
            if (size < 20) return (size_t)-1;
            size_t count = le16(b + pos + 14), table = pos + le32(b + pos + 16);
            for (size_t e = 0; e < count; e++) {
                if (table + 4 * e + 4 > n) return (size_t)-1;
                size_t eo = pos + le32(b + table + 4 * e); if (eo + 32 > n) return (size_t)-1;
                for (unsigned t = 0; t < b[eo + 28]; t++) {
                    if (eo + 32 + 4 * t + 4 > n) return (size_t)-1;
                    size_t to = eo + le32(b + eo + 32 + 4 * t); if (to + 8 > n) return (size_t)-1;
                    for (unsigned g = 0; g < b[to + 4]; g++) {
                        if (to + 8 + 4 * g + 4 > n) return (size_t)-1;
                        size_t go = to + le32(b + to + 8 + 4 * g); if (go + 12 > n) return (size_t)-1;
                        unsigned curve = b[go + 2]; size_t key = curve == 1 ? 8 : curve == 2 ? 12 : 0;
                        size_t nk = le16(b + go + 4), ko = go + le32(b + go + 8); if (!key) continue;
                        if (ko > n || nk * key > n - ko || np == cap) return (size_t)-1;
                        uint32_t id = (uint32_t)b[go] | (uint32_t)b[go + 1] << 8 | curve << 16 | (uint32_t)b[eo + 29] << 24;
                        out[np++] = (Part){b + eo, 28, le32(b + to), id, b + ko, nk * key, nk * key};
                    }
                }
            }
        }
        pos += size;
    }
    return np;
}
static int part_same(const Part *a, const Part *b) {
    return a->tag == b->tag && a->id == b->id && !strncmp((const char *)a->name, (const char *)b->name, a->len < b->len ? a->len : b->len);
}
static size_t part_nth(const Part *ps, size_t i) { size_t k = 0; for (size_t j = 0; j < i; j++) k += part_same(&ps[j], &ps[i]); return k; }
static unsigned words_changed(const uint8_t *o, const uint8_t *p, size_t n) {
    unsigned c = 0; for (size_t w = 0; w < n; w += 4) c += memcmp(o + w, p + w, n - w < 4 ? n - w : 4) != 0; return c;
}
/* Over n bytes: a word the port changed that the mod (m) still has as the game ships it (o) takes the port's (p) value; one
   the mod changed as well keeps the mod's. */
static void merge_words(uint8_t *m, const uint8_t *o, const uint8_t *p, size_t n, ModsMerge *st) {
    for (size_t w = 0; w < n; w += 4) {
        size_t wn = n - w < 4 ? n - w : 4;
        if (!memcmp(o + w, p + w, wn) || !memcmp(m + w, p + w, wn)) continue;
        if (memcmp(m + w, o + w, wn)) { st->kept++; continue; }
        memcpy(m + w, p + w, wn); st->words++;
    }
}
/* A member the mod resized (ml != ol): each pane (BFLYT) or animation key list (BFLAN) the port changed is found by name in the
   mod's copy and merged as merge_words does. 0 = done, -1 = a member that is not a layout or does not read. */
static int merge_by_name(uint8_t *m, size_t ml, const uint8_t *o, const uint8_t *p, size_t ol, ModsMerge *st) {
    int flyt = ol >= 4 && !memcmp(o, "FLYT", 4);
    if (!flyt && (ol < 4 || memcmp(o, "FLAN", 4))) return -1;
    size_t (*parts)(uint8_t *, size_t, Part *, size_t) = flyt ? flyt_parts : flan_parts, oc = ol / 8 + 1, mc = ml / 8 + 1;
    Part *op = malloc(oc * sizeof *op), *mp = malloc(mc * sizeof *mp);
    size_t on = op ? parts((uint8_t *)o, ol, op, oc) : (size_t)-1, mn = mp ? parts(m, ml, mp, mc) : (size_t)-1;
    if (on == (size_t)-1 || mn == (size_t)-1) { free(op); free(mp); return -1; }
    unsigned before = st->words, inside = 0;
    for (size_t i = 0; i < on; i++) {
        const uint8_t *pa = p + (op[i].at - o);   /* the port edits in place: at the game's offsets */
        unsigned changed = words_changed(op[i].at, pa, op[i].size); if (!changed) continue;
        inside += changed;
        size_t nth = part_nth(op, i), j = 0, seen = 0;
        for (; j < mn; j++) if (part_same(&op[i], &mp[j]) && seen++ == nth) break;
        if (j == mn) { st->lost += changed; continue; }   /* the mod's copy has no such pane or key list */
        size_t span = op[i].size;
        if (mp[j].size != op[i].size) {   /* resized too: the part of it that still lines up */
            span = op[i].fixed < mp[j].fixed ? op[i].fixed : mp[j].fixed; if (span > op[i].size) span = op[i].size;
            st->lost += words_changed(op[i].at + span, pa + span, op[i].size - span);
        }
        merge_words(mp[j].at, op[i].at, pa, span, st);
    }
    unsigned all = words_changed(o, p, ol); if (all > inside) st->lost += all - inside;   /* changes outside every pane / key list */
    st->by_name += st->words - before;
    free(op); free(mp); return 0;
}
int mods_port_merge(uint8_t *sarc, size_t len, const uint8_t *orig, size_t orig_len, const uint8_t *port, size_t port_len,
                    ModsMerge *st, char *why, size_t cap) {
    char scratch[8]; if (!why || !cap) { why = scratch; cap = sizeof scratch; }
    *why = 0; memset(st, 0, sizeof *st);
    if (!sarc || len < 0x20 || memcmp(sarc, "SARC", 4)) { snprintf(why, cap, "the mod's archive is not a SARC"); return -1; }
    const uint8_t *d = port; size_t n = port_len;
    if (n < 0x20 || memcmp(d, "SARC", 4) || d[6] != 0xFF || d[7] != 0xFE) { snprintf(why, cap, "the port's archive is not a SARC"); return -1; }
    if (orig_len < 0x20 || memcmp(orig, "SARC", 4)) { snprintf(why, cap, "the game's archive is not a SARC"); return -1; }
    size_t hdr = le16(d + 4), data = le32(d + 0x0C);
    if (hdr + 12 > n || memcmp(d + hdr, "SFAT", 4)) { snprintf(why, cap, "the port's archive has no SFAT"); return -1; }
    size_t count = le16(d + hdr + 6), sfnt = hdr + 12 + count * 16;
    if (sfnt + 8 > n || memcmp(d + sfnt, "SFNT", 4)) { snprintf(why, cap, "the port's archive has no SFNT"); return -1; }
    for (size_t k = 0; k < count; k++) {
        const uint8_t *node = d + hdr + 12 + k * 16; uint32_t attr = le32(node + 4), start = le32(node + 8), end = le32(node + 12);
        size_t at = sfnt + 8 + (size_t)(attr & 0xFFFFFF) * 4;
        if (!(attr >> 24) || at >= n || !memchr(d + at, 0, n - at) || start > end || data + end > n) continue;
        const char *name = (const char *)d + at; const uint8_t *p = d + data + start; size_t pl = end - start, ol = 0, ml = 0;
        const uint8_t *o = sarc_member((uint8_t *)orig, orig_len, name, &ol);
        if (!o || ol != pl || !memcmp(o, p, pl)) continue;   /* a member the port adds, resizes or leaves as the game ships it */
        st->members++;
        uint8_t *m = sarc_member(sarc, len, name, &ml);
        if (!m) { st->skipped++; continue; }   /* the mod dropped that member */
        if (ml != ol) {   /* the mod rebuilt it: by pane / animated pane name (a layout or an animation), else its own copy stays */
            if (merge_by_name(m, ml, o, p, ol, st)) st->skipped++; else st->rebuilt++;
            continue;
        }
        merge_words(m, o, p, pl, st);   /* 4-byte words from the member's start: the port's edits are floats in place */
    }
    return (int)st->words;
}
