/* The 15 Skyview Towers and 152 shrines (Mac Cheats tab: Teleport to a tower / shrine), read from the game's own RomFS
   and the player's newest save; nothing of the game is kept in the port.
   Places: the surface map's static actors (the Banc/MainField files ending _Static.bcett.byml.zs, BYML v7). A tower is
   FldObj_Cannon_A_01 with Dynamic.TowerNo "Tower01".."Tower15" and its door FldObj_CannonDoor_A_01 a few metres out; a
   shrine is DgnObj_ZonauShrineEntrance_A_01 with Dynamic.DungeonIndexStr "Dungeon000".."Dungeon151" and its travel point
   DgnObj_WarpPoint_Zonau_A_01 within 2 m (1.4.3, 2026-10-27: 15 towers, 152 entrances, each with a door / travel point).
   Names: LocationMsg/Location.msbt of Mals/USen.Product.*.sarc.zs, labelled TowerNN and DungeonNNN, as the map shows them.
   State: progress.sav (map_pins.h): IsVisitLocation.TowerNN, IsActivateCannon.TowerNN (Bool); IsVisitLocation.DungeonNNN
   (Bool), DungeonState.DungeonNNN (Enum: Hidden, Appear, Open, Enter, Clear) (names: marcrobledo/savegame-editors
   zelda-totk, MIT; the game's own events test IsActivateCannon.TowerNN, MrCheeze/totk-tools). Plain C, header only
   (tests/dish/probe.c checks it). */
#pragma once
#include "map_pins.h"   /* map_pins_newest_save, dish_murmur3 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LANDMARK_TOWERS = 15, LANDMARK_SHRINES = 152, LANDMARKS = LANDMARK_TOWERS + LANDMARK_SHRINES };
enum { LANDMARK_UNKNOWN, LANDMARK_NOT_FOUND, LANDMARK_FOUND, LANDMARK_DONE };   /* from the save: done = activated / completed */
/* Landmark k: towers first (k = Tower k+1), then shrines (k - 15 = Dungeon index). at: where a teleport lands, placed: at is known. */
typedef struct { char name[64]; float at[3]; int placed, state; } Landmark;

static inline void landmark_label(int k, char *out, size_t size) {
    if (k < LANDMARK_TOWERS) snprintf(out, size, "Tower%02d", k + 1); else snprintf(out, size, "Dungeon%03d", k - LANDMARK_TOWERS);
}
static inline uint32_t landmark_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* BYML (little-endian "YB", versions 2-7): the nodes this needs, bounds-checked. */
typedef struct { const uint8_t *d; size_t n; uint32_t keys, strs; } LandmarkByml;
static inline const char *landmark_byml_string(const LandmarkByml *b, uint32_t table, uint32_t i) {
    if (!table || (size_t)table + 4 > b->n || b->d[table] != 0xC2 || i >= landmark_le32(b->d + table) >> 8 || (size_t)table + 8 + 4ull * i > b->n) return NULL;
    size_t at = (size_t)table + landmark_le32(b->d + table + 4 + 4 * i);
    return at < b->n && memchr(b->d + at, 0, b->n - at) ? (const char *)b->d + at : NULL;
}
/* A dictionary's value for key: its type and raw word; 0 when node is no dictionary or has no such key. */
static inline int landmark_byml_get(const LandmarkByml *b, uint32_t node, const char *key, uint8_t *type, uint32_t *value) {
    if ((size_t)node + 4 > b->n || b->d[node] != 0xC1) return 0;
    uint32_t count = landmark_le32(b->d + node) >> 8;
    if ((size_t)node + 4 + 8ull * count > b->n) return 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = b->d + node + 4 + 8 * i;
        const char *k = landmark_byml_string(b, b->keys, (uint32_t)e[0] | (uint32_t)e[1] << 8 | (uint32_t)e[2] << 16);
        if (k && !strcmp(k, key)) { *type = e[3]; *value = landmark_le32(e + 4); return 1; }
    }
    return 0;
}
/* An array's length (its types at node + 4, its values after them); -1 when node is no array. */
static inline long landmark_byml_array(const LandmarkByml *b, uint32_t node, uint32_t *values) {
    if ((size_t)node + 4 > b->n || b->d[node] != 0xC0) return -1;
    uint32_t count = landmark_le32(b->d + node) >> 8; *values = node + 4 + ((count + 3) & ~3u);
    return (size_t)*values + 4ull * count <= b->n ? (long)count : -1;
}
static inline const char *landmark_byml_text(const LandmarkByml *b, uint32_t node, const char *key) {
    uint8_t t; uint32_t v; return landmark_byml_get(b, node, key, &t, &v) && t == 0xA0 ? landmark_byml_string(b, b->strs, v) : NULL;
}

/* What one pass over the static actors found: towers' centres and doors, shrine entrances and travel points. */
enum { LANDMARK_DOORS = 64, LANDMARK_WARPS = 512 };
typedef struct {
    float centre[LANDMARK_TOWERS][3], entrance[LANDMARK_SHRINES][3], door[LANDMARK_DOORS][3], warp[LANDMARK_WARPS][3];
    uint8_t has_centre[LANDMARK_TOWERS], has_entrance[LANDMARK_SHRINES]; int ndoors, nwarps;
} LandmarkScan;
/* The actors of one decompressed .bcett.byml; returns how many it added, -1 when it is not a BYML map file. */
static inline int landmarks_scan_byml(const uint8_t *d, size_t n, LandmarkScan *s) {
    if (n < 16 || d[0] != 'Y' || d[1] != 'B') return -1;
    LandmarkByml b = {d, n, landmark_le32(d + 4), landmark_le32(d + 8)};
    uint8_t t; uint32_t actors, values;
    if (!landmark_byml_get(&b, landmark_le32(d + 12), "Actors", &t, &actors) || t != 0xC0) return -1;
    long count = landmark_byml_array(&b, actors, &values); int added = 0;
    for (long i = 0; i < count; i++) {
        if (b.d[actors + 4 + i] != 0xC1) continue;
        uint32_t actor = landmark_le32(b.d + values + 4 * i), tr, tv, dyn = 0;
        const char *gyaml = landmark_byml_text(&b, actor, "Gyaml");
        if (!gyaml || !landmark_byml_get(&b, actor, "Translate", &t, &tr) || t != 0xC0 || landmark_byml_array(&b, tr, &tv) != 3) continue;
        float xyz[3]; int ok = 1;
        for (int c = 0; c < 3; c++) { if (b.d[tr + 4 + c] != 0xD2) ok = 0; uint32_t w = landmark_le32(b.d + tv + 4 * c); memcpy(&xyz[c], &w, 4); }
        if (!ok || xyz[0] != xyz[0] || xyz[1] != xyz[1] || xyz[2] != xyz[2]) continue;
        landmark_byml_get(&b, actor, "Dynamic", &t, &dyn);
        const char *id; int k;
        if (!strcmp(gyaml, "FldObj_Cannon_A_01") && (id = landmark_byml_text(&b, dyn, "TowerNo")) && sscanf(id, "Tower%d", &k) == 1 && k >= 1 && k <= LANDMARK_TOWERS) {
            memcpy(s->centre[k - 1], xyz, 12); s->has_centre[k - 1] = 1; added++;
        } else if (!strcmp(gyaml, "DgnObj_ZonauShrineEntrance_A_01") && (id = landmark_byml_text(&b, dyn, "DungeonIndexStr")) && sscanf(id, "Dungeon%d", &k) == 1 && k >= 0 && k < LANDMARK_SHRINES) {
            memcpy(s->entrance[k], xyz, 12); s->has_entrance[k] = 1; added++;
        } else if (!strcmp(gyaml, "FldObj_CannonDoor_A_01") && s->ndoors < LANDMARK_DOORS) { memcpy(s->door[s->ndoors++], xyz, 12); added++; }
        else if (!strcmp(gyaml, "DgnObj_WarpPoint_Zonau_A_01") && s->nwarps < LANDMARK_WARPS) { memcpy(s->warp[s->nwarps++], xyz, 12); added++; }
    }
    return added;
}
static inline float landmark_distance(const float *a, const float *b) {
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; return sqrtf(x * x + y * y + z * z);
}
static inline int landmark_nearest(const float (*list)[3], int count, const float *to, float within) {
    int best = -1; float bd = within;
    for (int i = 0; i < count; i++) { float d = landmark_distance(list[i], to); if (d <= bd) { bd = d; best = i; } }
    return best;
}
/* Where each teleport lands: a tower's floor 1.5 m in from its door, 1 m up (the door is in the tower's wall, ~5.5 m
   from its centre); a shrine's travel point (where fast travel puts the player, in front of the entrance), 1.5 m up.
   Without a door or travel point: 3 m above the tower's centre / the entrance. Returns how many are placed. */
static inline int landmarks_place(const LandmarkScan *s, Landmark *out) {
    int placed = 0;
    for (int k = 0; k < LANDMARKS; k++) {
        Landmark *l = &out[k]; int tower = k < LANDMARK_TOWERS, i = tower ? k : k - LANDMARK_TOWERS;
        const float *base = tower ? s->centre[i] : s->entrance[i];
        if (!(l->placed = tower ? s->has_centre[i] : s->has_entrance[i])) continue;
        placed++;
        int near = tower ? landmark_nearest(s->door, s->ndoors, base, 10) : landmark_nearest(s->warp, s->nwarps, base, 5);
        if (near < 0) { l->at[0] = base[0]; l->at[1] = base[1] + 3; l->at[2] = base[2]; continue; }
        const float *p = tower ? s->door[near] : s->warp[near];
        float dx = base[0] - p[0], dz = base[2] - p[2], len = sqrtf(dx * dx + dz * dz), in = tower && len > 0.1f ? 1.5f / len : 0;
        l->at[0] = p[0] + dx * in; l->at[1] = p[1] + (tower ? 1.0f : 1.5f); l->at[2] = p[2] + dz * in;
    }
    return placed;
}

/* A SARC archive's member (little-endian); NULL when it is not there. */
static inline const uint8_t *landmark_sarc_member(const uint8_t *d, size_t n, const char *name, size_t *size) {
    if (n < 0x20 || memcmp(d, "SARC", 4) || d[6] != 0xFF || d[7] != 0xFE || memcmp(d + 0x14, "SFAT", 4)) return NULL;
    uint32_t data = landmark_le32(d + 0x0C); unsigned count = d[0x1A] | d[0x1B] << 8; size_t sfnt = 0x20 + (size_t)count * 16;
    if (sfnt + 8 > n || memcmp(d + sfnt, "SFNT", 4)) return NULL;
    for (unsigned k = 0; k < count; k++) {
        const uint8_t *node = d + 0x20 + (size_t)k * 16; uint32_t attr = landmark_le32(node + 4), start = landmark_le32(node + 8), end = landmark_le32(node + 12);
        size_t at = sfnt + 8 + (size_t)(attr & 0xFFFF) * 4;
        if (!(attr >> 24) || at >= n || !memchr(d + at, 0, n - at) || strcmp((const char *)d + at, name)) continue;
        if (start > end || (uint64_t)data + end > n) return NULL;
        *size = end - start; return d + data + start;
    }
    return NULL;
}
/* An MSBT message's text as UTF-8, its tags (furigana, colour, ...) left out; 0 when the label is not there. */
static inline int landmark_msbt_text(const uint8_t *d, size_t n, const char *label, char *out, size_t size) {
    if (n < 0x20 || memcmp(d, "MsgStdBn", 8) || d[8] != 0xFF || d[9] != 0xFE || d[0x0C] != 1 || !size) return 0;   /* little-endian UTF-16 */
    unsigned sections = d[0x0E] | d[0x0F] << 8; size_t at = 0x20, lbl = 0, txt = 0, txt_size = 0;
    for (unsigned s = 0; s < sections && at + 16 <= n; s++) {
        uint32_t len = landmark_le32(d + at + 4);
        if (!memcmp(d + at, "LBL1", 4)) lbl = at + 16; else if (!memcmp(d + at, "TXT2", 4)) { txt = at + 16; txt_size = len; }
        at += 16 + ((len + 15) & ~(size_t)15);
    }
    if (!lbl || !txt || txt + txt_size > n || txt_size < 4) return 0;
    uint32_t slots = landmark_le32(d + lbl), index = UINT32_MAX; size_t want = strlen(label);
    for (uint32_t s = 0; s < slots && lbl + 12 + 8ull * s <= n && index == UINT32_MAX; s++) {
        uint32_t count = landmark_le32(d + lbl + 4 + 8 * s); size_t p = lbl + landmark_le32(d + lbl + 8 + 8 * s);
        for (uint32_t i = 0; i < count && p < n; i++) {
            size_t len = d[p];
            if (p + 1 + len + 4 > n) break;
            if (len == want && !memcmp(d + p + 1, label, len)) { index = landmark_le32(d + p + 1 + len); break; }
            p += 1 + len + 4;
        }
    }
    uint32_t texts = landmark_le32(d + txt);
    if (index >= texts || 4 + 4ull * texts > txt_size) return 0;
    size_t p = txt + landmark_le32(d + txt + 4 + 4 * index), end = index + 1 < texts ? txt + landmark_le32(d + txt + 8 + 4 * index) : txt + txt_size, o = 0;
    if (end > txt + txt_size) end = txt + txt_size;
    while (p + 2 <= end) {
        uint32_t c = d[p] | d[p + 1] << 8; p += 2;
        if (!c) break;
        if (c == 0x0E) { if (p + 6 > end) break; p += 6 + (d[p + 4] | d[p + 5] << 8); continue; }   /* a tag: group, type, size, data */
        if (c == 0x0F) { p += 4; continue; }   /* a closing tag */
        if (c >= 0xD800 && c < 0xDC00 && p + 2 <= end) { uint32_t lo = d[p] | d[p + 1] << 8; if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); p += 2; } }
        char u[4]; int k = c < 0x80 ? (u[0] = (char)c, 1) : c < 0x800 ? (u[0] = (char)(0xC0 | c >> 6), u[1] = (char)(0x80 | (c & 63)), 2)
                  : c < 0x10000 ? (u[0] = (char)(0xE0 | c >> 12), u[1] = (char)(0x80 | (c >> 6 & 63)), u[2] = (char)(0x80 | (c & 63)), 3)
                  : (u[0] = (char)(0xF0 | c >> 18), u[1] = (char)(0x80 | (c >> 12 & 63)), u[2] = (char)(0x80 | (c >> 6 & 63)), u[3] = (char)(0x80 | (c & 63)), 4);
        if (o + k >= size) break;
        memcpy(out + o, u, k); o += k;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
    return o > 0;
}

/* The RomFS as the runtime serves it (romfs.c: the image's files, a mod's copy where one replaces a file). */
typedef struct {
    size_t (*count)(void); const char *(*name)(size_t i); uint8_t *(*read)(const char *path, size_t *n);
    uint8_t *(*unzs)(const uint8_t *src, size_t n, size_t *out);
} LandmarkRomfs;
static inline int landmark_ends(const char *s, const char *t) { size_t a = strlen(s), b = strlen(t); return a >= b && !strcmp(s + a - b, t); }
static inline uint8_t *landmark_read(const LandmarkRomfs *r, const char *path, size_t *n) {
    size_t zn = 0; uint8_t *z = r->read(path, &zn);
    if (!z || !landmark_ends(path, ".zs")) { *n = zn; return z; }
    uint8_t *plain = r->unzs(z, zn, n); free(z); return plain;
}
/* Every tower and shrine with its name and landing place (state LANDMARK_UNKNOWN); returns how many are placed (0: not
   this game, or its map could not be read). A few dozen milliseconds: ~390 files, 26 MB once decompressed. */
static inline int landmarks_load(const LandmarkRomfs *r, Landmark *out) {
    memset(out, 0, sizeof(Landmark) * LANDMARKS);
    LandmarkScan *s = calloc(1, sizeof *s); if (!s) return 0;
    char mals[256] = "";
    for (size_t i = 0, count = r->count(); i < count; i++) {
        const char *p = r->name(i); if (!p) continue;
        if (!strncmp(p, "Mals/USen.Product.", 18) && landmark_ends(p, ".sarc.zs")) snprintf(mals, sizeof mals, "%s", p);
        if (strncmp(p, "Banc/MainField/", 15) || !landmark_ends(p, "_Static.bcett.byml.zs")) continue;
        size_t n; uint8_t *d = landmark_read(r, p, &n);
        if (d) landmarks_scan_byml(d, n, s);
        free(d);
    }
    int placed = landmarks_place(s, out); free(s);
    size_t n = 0, mn = 0; uint8_t *pack = mals[0] ? landmark_read(r, mals, &n) : NULL;
    const uint8_t *msg = pack ? landmark_sarc_member(pack, n, "LocationMsg/Location.msbt", &mn) : NULL;
    for (int k = 0; k < LANDMARKS; k++) {
        char label[16]; landmark_label(k, label, sizeof label);
        if (!msg || !landmark_msbt_text(msg, mn, label, out[k].name, sizeof out[k].name))
            snprintf(out[k].name, sizeof out[k].name, k < LANDMARK_TOWERS ? "Skyview Tower %d" : "Shrine %d", k < LANDMARK_TOWERS ? k + 1 : k - LANDMARK_TOWERS);
    }
    free(pack);
    return placed;
}

/* Each landmark's state in a progress.sav image (map_pins.h has the layout); returns how many flags were found (0: none,
   every state LANDMARK_UNKNOWN). */
static inline int landmarks_state_parse(const uint8_t *b, size_t n, Landmark *lm) {
    for (int k = 0; k < LANDMARKS; k++) lm[k].state = LANDMARK_UNKNOWN;
    if (n < 0x30 || landmark_le32(b) != 0x01020304) return 0;
    uint32_t visit[LANDMARKS], done[LANDMARKS], vv[LANDMARKS], dv[LANDMARKS]; uint8_t got[LANDMARKS] = {0};
    for (int k = 0; k < LANDMARKS; k++) {
        char label[16], name[64]; landmark_label(k, label, sizeof label);
        snprintf(name, sizeof name, "IsVisitLocation.%s", label); visit[k] = dish_murmur3(name);
        snprintf(name, sizeof name, k < LANDMARK_TOWERS ? "IsActivateCannon.%s" : "DungeonState.%s", label); done[k] = dish_murmur3(name);
    }
    int found = 0;
    for (size_t i = 0x28; i + 8 <= n; i += 8) {
        uint32_t h = landmark_le32(b + i), v = landmark_le32(b + i + 4);
        if (h == 0xa3db7114u) break;
        for (int k = 0; k < LANDMARKS; k++) {
            if (h == visit[k]) { vv[k] = v; got[k] |= 1; found++; }
            else if (h == done[k]) { dv[k] = v; got[k] |= 2; found++; }
        }
    }
    uint32_t clear = dish_murmur3("Clear"), open = dish_murmur3("Open"), enter = dish_murmur3("Enter");
    for (int k = 0; k < LANDMARKS; k++) {
        if (got[k] != 3) continue;
        int is_done = k < LANDMARK_TOWERS ? dv[k] == 1 : dv[k] == clear, seen = vv[k] == 1 || (k >= LANDMARK_TOWERS && (dv[k] == open || dv[k] == enter));
        lm[k].state = is_done ? LANDMARK_DONE : seen ? LANDMARK_FOUND : LANDMARK_NOT_FOUND;
    }
    return found;
}
/* The states from the newest save of profile; returns how many flags were found, -1 when there is no save. */
static inline int landmarks_state_read(const char *profile, Landmark *lm) {
    for (int k = 0; k < LANDMARKS; k++) lm[k].state = LANDMARK_UNKNOWN;
    char path[1800]; map_pins_newest_save(profile, path, sizeof path);
    if (!path[0]) return -1;
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    uint8_t *b = malloc(4 << 20); size_t n = b ? fread(b, 1, 4 << 20, f) : 0; fclose(f);
    int got = b ? landmarks_state_parse(b, n, lm) : 0;
    free(b);
    return got;
}
