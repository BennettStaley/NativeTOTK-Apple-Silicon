/* The places Mac Tools › Teleport, Towers and Shrines fly to: the 15 Skyview Towers, the 152 shrines and the story's places (Lookout Landing,
   the five temples), read from the game's own RomFS and the player's newest save; nothing of the game is kept in the port.
   Places: the static actors of the surface and the Depths (the Banc/MainField and Banc/MinusField files ending
   _Static.bcett.byml.zs, BYML v7). A tower is FldObj_Cannon_A_01 with Dynamic.TowerNo "Tower01".."Tower15" and its door
   FldObj_CannonDoor_A_01 a few metres out; a shrine is DgnObj_ZonauShrineEntrance_A_01 with Dynamic.DungeonIndexStr
   "Dungeon000".."Dungeon151" and its travel point DgnObj_WarpPoint_Zonau_A_01 within 2 m (1.4.3, 2026-10-27: 15 towers,
   152 entrances, each with a door / travel point). A story place is where the map's own place list puts its name
   (Banc/<field>/LocationArea/<field>.locationarea.byml.zs) and lands at the travel point nearest it: the temples'
   DgnObj_WarpPoint_Zonau_A_02 / _A_03 / LDungeonSoulEntrance, Lookout Landing's tower point DgnObj_WarpPoint_A_01 (20 to
   117 m from the markers; the next travel point is 348 m or more away). Hyrule Castle has no travel point of its own near
   its marker and is left out. A place from the MinusField list is in the Depths (the Fire and Spirit Temples).
   Names: LocationMsg/Location.msbt of Mals/USen.Product.*.sarc.zs, labelled TowerNN, DungeonNNN and the place's name.
   State: progress.sav (map_pins.h): IsVisitLocation.<label> (Bool) for all; IsActivateCannon.TowerNN (Bool);
   DungeonState.DungeonNNN (Enum: Hidden, Appear, Open, Enter, Clear); a temple's region quest Step_SageOfWind / Fire /
   Zora / Gerudo / Soul (Enum: NotReady, Ready, then its steps). The story so far: Step_MainChallenge (Enum; past
   GoToBackOfTempleOfTimeAfterWD the player has left Great Sky Island) or IsGet.Parasail; the Depths: IsVisitLocation.
   MinusField, a Lightroot's IsVisitLocation.CheckPointNNN or ArrivalPointState.CheckPointNNN = Open (names and enum
   values: marcrobledo/savegame-editors zelda-totk, hashes.csv by MacSpazzy and MrCheeze, MIT; the game's own events test
   IsActivateCannon.TowerNN, MrCheeze/totk-tools). Plain C, header only (tests/dish/probe.c checks it). */
#pragma once
#include "map_pins.h"   /* map_pins_newest_save, dish_murmur3 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Depths' Lightroots: CheckPoint000..147 (120 of them used, 1.4.3), each a DgnObj_WarpPoint_Zonau_A_04 with
   Dynamic.CheckPointNo; lit: ArrivalPointState.CheckPointNNN = Open. */
/* The Dragon's Tears: the map's place list markers DragonTears01..12 (each at a FldObj_DragonTears_01); seen:
   IsVisitLocation.DragonTearsNN. The game's messages have no names for them. */
enum { LANDMARK_TOWERS = 15, LANDMARK_SHRINES = 152, LANDMARK_STORY = 6, LANDMARK_LIGHTROOTS = 148, LANDMARK_TEARS = 12, LANDMARK_FIRST_SHRINE = LANDMARK_TOWERS,
       LANDMARK_FIRST_STORY = LANDMARK_TOWERS + LANDMARK_SHRINES, LANDMARK_FIRST_LIGHTROOT = LANDMARK_FIRST_STORY + LANDMARK_STORY,
       LANDMARK_FIRST_TEAR = LANDMARK_FIRST_LIGHTROOT + LANDMARK_LIGHTROOTS, LANDMARKS = LANDMARK_FIRST_TEAR + LANDMARK_TEARS };
enum { LANDMARK_UNKNOWN, LANDMARK_NOT_FOUND, LANDMARK_FOUND, LANDMARK_DONE };   /* from the save: done = activated / completed */
/* Why the story lock (landmarks_lock) greys a place; LANDMARK_OPEN: it does not. */
enum { LANDMARK_OPEN, LANDMARK_LOCK_ISLAND, LANDMARK_LOCK_DEPTHS, LANDMARK_LOCK_QUEST };
static const char *const LANDMARK_LOCK_TEXT[] = {"", "Locked until the story leaves Great Sky Island", "Locked until you have been to the Depths",
                                                 "Locked until its quest gets there in the story"};
/* Landmark k: towers first (k = Tower k+1), then shrines (k - 15 = Dungeon index), then the story's places
   (LANDMARK_STORY_PLACES), then the Lightroots (k - LANDMARK_FIRST_LIGHTROOT = CheckPoint index), then the Dragon's Tears
   (k - LANDMARK_FIRST_TEAR + 1 = DragonTears number). at: where a teleport lands, placed: at is known; island: on Great Sky Island, depths: in the
   Depths (from the map); state, opened (the story has sent the player there), lock: from the save. */
typedef struct { char name[64]; float at[3]; int placed, state, island, depths, opened, lock; } Landmark;

/* The story's places: the map's location name (also the message label and the IsVisitLocation flag) and the quest whose
   step opens and completes it. opens: the steps from which the place is there to go to (NULL: any step after Ready, the
   quest has started); done: the steps after its boss. The Lightning Temple lies buried in the sand until the quest finds it. */
typedef struct { const char *location, *quest; const char *const *opens, *const *done; } LandmarkStoryPlace;
static const char *const LANDMARK_WIND_DONE[] = {"DefeatBoss", "TouchSecretStone", "Finish", "Complete", NULL};
static const char *const LANDMARK_FIRE_DONE[] = {"DefeatBoss", "TouchSecretStone", "Finale", "Complete", NULL};
static const char *const LANDMARK_GERUDO_OPENS[] = {"DungeonFind", "DungeonStart", "DungeonBossDead", "DungeonEnd", "Complete", NULL};
static const char *const LANDMARK_GERUDO_DONE[] = {"DungeonBossDead", "DungeonEnd", "Complete", NULL};
static const char *const LANDMARK_SOUL_DONE[] = {"Dungeon_DefeatedBoss", "Complete", NULL};
static const LandmarkStoryPlace LANDMARK_STORY_PLACES[LANDMARK_STORY] = {
    {"City_BaseCamp", NULL, NULL, NULL},
    {"LargeDungeonWind", "Step_SageOfWind", NULL, LANDMARK_WIND_DONE},
    {"LargeDungeonFire", "Step_SageOfFire", NULL, LANDMARK_FIRE_DONE},
    {"LargeDungeonWater", "Step_SageOfZora", NULL, LANDMARK_WIND_DONE},   /* the same steps after the boss as the Wind Temple's */
    {"LargeDungeonThunder", "Step_SageOfGerudo", LANDMARK_GERUDO_OPENS, LANDMARK_GERUDO_DONE},
    {"LargeDungeonSoul", "Step_SageOfSoul", NULL, LANDMARK_SOUL_DONE},
};
/* The Teleport page's categories, in page order; fold: a long list behind a Show / Hide row. More (the Depths' Lightroots,
   caves, places) go here as ranges of their own. */
typedef struct { const char *title; int first, count, fold; } LandmarkGroup;
static const LandmarkGroup LANDMARK_GROUPS[] = {{"Story", LANDMARK_FIRST_STORY, LANDMARK_STORY, 0}, {"Skyview Towers", 0, LANDMARK_TOWERS, 1},
                                                {"Shrines", LANDMARK_FIRST_SHRINE, LANDMARK_SHRINES, 1}, {"Lightroots", LANDMARK_FIRST_LIGHTROOT, LANDMARK_LIGHTROOTS, 1},
                                                {"Dragon's Tears", LANDMARK_FIRST_TEAR, LANDMARK_TEARS, 1}};
enum { LANDMARK_NGROUPS = sizeof LANDMARK_GROUPS / sizeof *LANDMARK_GROUPS };
static inline int landmark_is_story(int k) { return k >= LANDMARK_FIRST_STORY && k < LANDMARK_FIRST_LIGHTROOT; }
static inline int landmark_is_lightroot(int k) { return k >= LANDMARK_FIRST_LIGHTROOT && k < LANDMARK_FIRST_TEAR; }
static inline int landmark_is_tear(int k) { return k >= LANDMARK_FIRST_TEAR && k < LANDMARKS; }
static inline int landmark_is_shrine(int k) { return k >= LANDMARK_FIRST_SHRINE && k < LANDMARK_FIRST_STORY; }

static inline void landmark_label(int k, char *out, size_t size) {
    if (k < LANDMARK_TOWERS) snprintf(out, size, "Tower%02d", k + 1);
    else if (k < LANDMARK_FIRST_STORY) snprintf(out, size, "Dungeon%03d", k - LANDMARK_FIRST_SHRINE);
    else if (k < LANDMARK_FIRST_LIGHTROOT) snprintf(out, size, "%s", LANDMARK_STORY_PLACES[k - LANDMARK_FIRST_STORY].location);
    else if (k < LANDMARK_FIRST_TEAR) snprintf(out, size, "CheckPoint%03d", k - LANDMARK_FIRST_LIGHTROOT);
    else snprintf(out, size, "DragonTears%02d", k - LANDMARK_FIRST_TEAR + 1);
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
/* An array of three floats (a Translate, a marker's Trans entry) into xyz; 0 when node is not one or holds a NaN. */
static inline int landmark_byml_vec3(const LandmarkByml *b, uint32_t node, float *xyz) {
    uint32_t v; if (landmark_byml_array(b, node, &v) != 3) return 0;
    for (int c = 0; c < 3; c++) { if (b->d[node + 4 + c] != 0xD2) return 0; uint32_t w = landmark_le32(b->d + v + 4 * c); memcpy(&xyz[c], &w, 4); }
    return xyz[0] == xyz[0] && xyz[1] == xyz[1] && xyz[2] == xyz[2];
}

/* What one pass over the static actors found: towers' centres and doors, shrine entrances and travel points (shrine-type
   Zonau_A_01), the Lightroots' (Zonau_A_04, by CheckPointNo) and the other travel points (temples, towers). */
enum { LANDMARK_DOORS = 64, LANDMARK_WARPS = 512, LANDMARK_TRAVELS = 64 };
typedef struct {
    float centre[LANDMARK_TOWERS][3], entrance[LANDMARK_SHRINES][3], door[LANDMARK_DOORS][3], warp[LANDMARK_WARPS][3], travel[LANDMARK_TRAVELS][3];
    float root[LANDMARK_LIGHTROOTS][3];
    uint8_t has_centre[LANDMARK_TOWERS], has_entrance[LANDMARK_SHRINES], has_root[LANDMARK_LIGHTROOTS]; int ndoors, nwarps, ntravels;
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
        uint32_t actor = landmark_le32(b.d + values + 4 * i), tr, dyn = 0;
        const char *gyaml = landmark_byml_text(&b, actor, "Gyaml"); float xyz[3];
        if (!gyaml || !landmark_byml_get(&b, actor, "Translate", &t, &tr) || t != 0xC0 || !landmark_byml_vec3(&b, tr, xyz)) continue;
        landmark_byml_get(&b, actor, "Dynamic", &t, &dyn);
        const char *id; int k;
        if (!strcmp(gyaml, "FldObj_Cannon_A_01") && (id = landmark_byml_text(&b, dyn, "TowerNo")) && sscanf(id, "Tower%d", &k) == 1 && k >= 1 && k <= LANDMARK_TOWERS) {
            memcpy(s->centre[k - 1], xyz, 12); s->has_centre[k - 1] = 1; added++;
        } else if (!strcmp(gyaml, "DgnObj_ZonauShrineEntrance_A_01") && (id = landmark_byml_text(&b, dyn, "DungeonIndexStr")) && sscanf(id, "Dungeon%d", &k) == 1 && k >= 0 && k < LANDMARK_SHRINES) {
            memcpy(s->entrance[k], xyz, 12); s->has_entrance[k] = 1; added++;
        } else if (!strcmp(gyaml, "FldObj_CannonDoor_A_01") && s->ndoors < LANDMARK_DOORS) { memcpy(s->door[s->ndoors++], xyz, 12); added++; }
        else if (!strcmp(gyaml, "DgnObj_WarpPoint_Zonau_A_01") && s->nwarps < LANDMARK_WARPS) { memcpy(s->warp[s->nwarps++], xyz, 12); added++; }
        else if (!strcmp(gyaml, "DgnObj_WarpPoint_Zonau_A_04")) {
            if ((id = landmark_byml_text(&b, dyn, "CheckPointNo")) && sscanf(id, "CheckPoint%d", &k) == 1 && k >= 0 && k < LANDMARK_LIGHTROOTS) {
                memcpy(s->root[k], xyz, 12); s->has_root[k] = 1; added++;
            }
        }
        else if (!strncmp(gyaml, "DgnObj_WarpPoint_", 17) && strcmp(gyaml, "DgnObj_WarpPoint_Zonau_A_04") && s->ntravels < LANDMARK_TRAVELS) {
            memcpy(s->travel[s->ntravels++], xyz, 12); added++;
        }
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
/* Where each tower and shrine teleport lands: a tower's floor 1.5 m in from its door, 1 m up (the door is in the tower's
   wall, ~5.5 m from its centre); a shrine's travel point (where fast travel puts the player, in front of the entrance),
   1.5 m up. Without a door or travel point: 3 m above the tower's centre / the entrance. Returns how many are placed. */
static inline int landmarks_place(const LandmarkScan *s, Landmark *out) {
    int placed = 0;
    for (int k = 0; k < LANDMARK_FIRST_STORY; k++) {
        Landmark *l = &out[k]; int tower = k < LANDMARK_TOWERS, i = tower ? k : k - LANDMARK_FIRST_SHRINE;
        const float *base = tower ? s->centre[i] : s->entrance[i];
        if (!(l->placed = tower ? s->has_centre[i] : s->has_entrance[i])) continue;
        placed++;
        int near = tower ? landmark_nearest(s->door, s->ndoors, base, 10) : landmark_nearest(s->warp, s->nwarps, base, 5);
        if (near < 0) { l->at[0] = base[0]; l->at[1] = base[1] + 3; l->at[2] = base[2]; continue; }
        const float *p = tower ? s->door[near] : s->warp[near];
        float dx = base[0] - p[0], dz = base[2] - p[2], len = sqrtf(dx * dx + dz * dz), in = tower && len > 0.1f ? 1.5f / len : 0;
        l->at[0] = p[0] + dx * in; l->at[1] = p[1] + (tower ? 1.0f : 1.5f); l->at[2] = p[2] + dz * in;
    }
    for (int i = 0; i < LANDMARK_LIGHTROOTS; i++) {   /* a Lightroot lands on its travel point, as fast travel does, 1.5 m up */
        Landmark *l = &out[LANDMARK_FIRST_LIGHTROOT + i];
        if (!(l->placed = s->has_root[i])) continue;
        placed++; l->depths = 1; l->at[0] = s->root[i][0]; l->at[1] = s->root[i][1] + 1.5f; l->at[2] = s->root[i][2];
    }
    return placed;
}
/* The markers of location name in the map's place list (a .locationarea.byml: a dictionary of marker kinds, each an array of
   {LocationName "Work/Location/<name>.game__location__Location.gyml", Trans [[x, y, z], ...]}); returns how many. */
static inline int landmark_markers(const uint8_t *d, size_t n, const char *name, float (*out)[3], int max) {
    if (n < 16 || d[0] != 'Y' || d[1] != 'B') return 0;
    LandmarkByml b = {d, n, landmark_le32(d + 4), landmark_le32(d + 8)};
    uint32_t root = landmark_le32(d + 12); size_t want = strlen(name); int got = 0;
    if ((size_t)root + 4 > n || d[root] != 0xC1) return 0;
    uint32_t kinds = landmark_le32(d + root) >> 8;
    if ((size_t)root + 4 + 8ull * kinds > n) return 0;
    for (uint32_t i = 0; i < kinds; i++) {
        const uint8_t *e = d + root + 4 + 8 * i; uint32_t list = landmark_le32(e + 4), lv;
        long count = e[3] == 0xC0 ? landmark_byml_array(&b, list, &lv) : -1;
        for (long j = 0; j < count; j++) {
            if (d[list + 4 + j] != 0xC1) continue;
            uint32_t place = landmark_le32(d + lv + 4 * j), tr, tv; uint8_t t;
            const char *ln = landmark_byml_text(&b, place, "LocationName"), *base = ln ? strrchr(ln, '/') : NULL;
            base = base ? base + 1 : ln;
            if (!base || strncmp(base, name, want) || base[want] != '.' || !landmark_byml_get(&b, place, "Trans", &t, &tr) || t != 0xC0) continue;
            long nt = landmark_byml_array(&b, tr, &tv);
            for (long m = 0; m < nt && got < max; m++)
                if (d[tr + 4 + m] == 0xC0 && landmark_byml_vec3(&b, landmark_le32(d + tv + 4 * m), out[got])) got++;
        }
    }
    return got;
}
/* A story place's landing: the travel point (shrine-type or other) nearest any of its markers, within 150 m (1.4.3: the
   temples' are 20 to 117 m from theirs, Lookout Landing's 53 m, the second nearest 348 m or more; Hyrule Castle's nearest is
   a shrine's, 165 m), 1.5 m up. Returns 1 when placed. */
static inline int landmark_place_story(const LandmarkScan *s, const float (*marker)[3], int nmarkers, Landmark *l) {
    float best = 150; const float *at = NULL;
    for (int list = 0; list < 2; list++) {
        const float (*p)[3] = list ? s->travel : s->warp; int n = list ? s->ntravels : s->nwarps;
        for (int i = 0; i < n; i++)
            for (int m = 0; m < nmarkers; m++) { float d = landmark_distance(p[i], marker[m]); if (d <= best) { best = d; at = p[i]; } }
    }
    if (!at) return l->placed = 0;
    l->at[0] = at[0]; l->at[1] = at[1] + 1.5f; l->at[2] = at[2];
    return l->placed = 1;
}
/* On Great Sky Island: within 800 m across of its marker (BeginningIsland) and less than 800 m below it. Its four shrines
   are 340 to 440 m out and from 420 m below to 470 m above; the nearest place outside it is a surface shrine 1800 m below. */
static inline int landmark_on_island(const float *island, const float *at) {
    float dx = at[0] - island[0], dz = at[2] - island[2];
    return dx * dx + dz * dz < 800.0f * 800.0f && at[1] > island[1] - 800;
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
/* Every tower, shrine and story place with its name and landing place (state LANDMARK_UNKNOWN); returns how many are
   placed (0: not this game, or its map could not be read). A few dozen milliseconds: ~475 files, 30 MB once decompressed. */
static inline int landmarks_load(const LandmarkRomfs *r, Landmark *out) {
    memset(out, 0, sizeof(Landmark) * LANDMARKS);
    LandmarkScan *s = calloc(1, sizeof *s); if (!s) return 0;
    char mals[256] = "";
    for (size_t i = 0, count = r->count(); i < count; i++) {
        const char *p = r->name(i); if (!p) continue;
        if (!strncmp(p, "Mals/USen.Product.", 18) && landmark_ends(p, ".sarc.zs")) snprintf(mals, sizeof mals, "%s", p);
        if ((strncmp(p, "Banc/MainField/", 15) && strncmp(p, "Banc/MinusField/", 16)) || !landmark_ends(p, "_Static.bcett.byml.zs")) continue;
        size_t n; uint8_t *d = landmark_read(r, p, &n);
        if (d) landmarks_scan_byml(d, n, s);
        free(d);
    }
    int placed = landmarks_place(s, out);
    size_t sn = 0, dn = 0; float island[1][3], mk[16][3];
    uint8_t *surface = landmark_read(r, "Banc/MainField/LocationArea/MainField.locationarea.byml.zs", &sn),
            *depths = landmark_read(r, "Banc/MinusField/LocationArea/MinusField.locationarea.byml.zs", &dn);
    int has_island = surface && landmark_markers(surface, sn, "BeginningIsland", island, 1);
    for (int k = LANDMARK_FIRST_STORY; k < LANDMARK_FIRST_LIGHTROOT; k++) {
        const char *name = LANDMARK_STORY_PLACES[k - LANDMARK_FIRST_STORY].location;
        int nm = surface ? landmark_markers(surface, sn, name, mk, 16) : 0;
        if (!nm && depths && (nm = landmark_markers(depths, dn, name, mk, 16))) out[k].depths = 1;
        placed += nm && landmark_place_story(s, (const float (*)[3])mk, nm, &out[k]);
    }
    for (int k = LANDMARK_FIRST_TEAR; k < LANDMARKS && surface; k++) {   /* a Dragon's Tear: on its marker, 1.5 m up */
        char label[32]; landmark_label(k, label, sizeof label);
        if (!landmark_markers(surface, sn, label, mk, 1)) continue;
        out[k].placed = 1; out[k].at[0] = mk[0][0]; out[k].at[1] = mk[0][1] + 1.5f; out[k].at[2] = mk[0][2]; placed++;
    }
    for (int k = 0; k < LANDMARKS && has_island; k++) out[k].island = out[k].placed && !out[k].depths && landmark_on_island(island[0], out[k].at);
    free(surface); free(depths); free(s);
    size_t n = 0, mn = 0; uint8_t *pack = mals[0] ? landmark_read(r, mals, &n) : NULL;
    const uint8_t *msg = pack ? landmark_sarc_member(pack, n, "LocationMsg/Location.msbt", &mn) : NULL;
    for (int k = 0; k < LANDMARKS; k++) {
        char label[32]; landmark_label(k, label, sizeof label);
        if (msg && landmark_msbt_text(msg, mn, label, out[k].name, sizeof out[k].name)) continue;
        if (k < LANDMARK_TOWERS) snprintf(out[k].name, sizeof out[k].name, "Skyview Tower %d", k + 1);
        else if (k < LANDMARK_FIRST_STORY) snprintf(out[k].name, sizeof out[k].name, "Shrine %d", k - LANDMARK_FIRST_SHRINE);
        else if (landmark_is_lightroot(k)) snprintf(out[k].name, sizeof out[k].name, "Lightroot %d", k - LANDMARK_FIRST_LIGHTROOT);
        else if (landmark_is_tear(k)) snprintf(out[k].name, sizeof out[k].name, "Dragon's Tear %d", k - LANDMARK_FIRST_TEAR + 1);
        else snprintf(out[k].name, sizeof out[k].name, "%s", label);
    }
    free(pack);
    return placed;
}

/* What the save says of the story so far. known: Step_MainChallenge is there (a save of this game); island_done: the player
   has left Great Sky Island; depths_found: has been to the Depths. */
typedef struct { int known, island_done, depths_found; } LandmarkProgress;
/* The flags the save is searched for: each landmark's visit and done flags, then the story's (LANDMARK_SLOT_...). */
enum { LANDMARK_SLOT_MAIN = 2 * LANDMARKS, LANDMARK_SLOT_PARASAIL, LANDMARK_SLOT_DEPTHS, LANDMARK_SLOTS };
typedef struct { uint32_t hash; int slot; } LandmarkWant;
static inline int landmark_want_cmp(const void *a, const void *b) {
    uint32_t x = ((const LandmarkWant *)a)->hash, y = ((const LandmarkWant *)b)->hash; return x < y ? -1 : x > y;
}
static inline int landmark_step_in(uint32_t value, const char *const *steps) {
    for (; steps && *steps; steps++) if (value == dish_murmur3(*steps)) return 1;
    return 0;
}
/* Each landmark's state in a progress.sav image (map_pins.h has the layout) and, into progress (may be NULL), the story so
   far; returns how many of the landmarks' flags were found (0: none, every state LANDMARK_UNKNOWN). */
static inline int landmarks_state_parse(const uint8_t *b, size_t n, Landmark *lm, LandmarkProgress *progress) {
    LandmarkProgress pr = {0, 0, 0};
    for (int k = 0; k < LANDMARKS; k++) { lm[k].state = LANDMARK_UNKNOWN; lm[k].opened = 1; }
    if (progress) *progress = pr;
    if (n < 0x30 || landmark_le32(b) != 0x01020304) return 0;
    enum { WANTS = LANDMARK_SLOTS };
    LandmarkWant want[WANTS]; int nw = 0; char label[32], name[96];
    for (int k = 0; k < LANDMARKS; k++) {
        landmark_label(k, label, sizeof label);
        snprintf(name, sizeof name, "IsVisitLocation.%s", label); want[nw++] = (LandmarkWant){dish_murmur3(name), k};
        const char *quest = landmark_is_story(k) ? LANDMARK_STORY_PLACES[k - LANDMARK_FIRST_STORY].quest : NULL;
        if (k < LANDMARK_FIRST_STORY) snprintf(name, sizeof name, k < LANDMARK_TOWERS ? "IsActivateCannon.%s" : "DungeonState.%s", label);
        else if (landmark_is_lightroot(k)) snprintf(name, sizeof name, "ArrivalPointState.%s", label);
        else if (quest) snprintf(name, sizeof name, "%s", quest);
        else continue;
        want[nw++] = (LandmarkWant){dish_murmur3(name), LANDMARKS + k};
    }
    want[nw++] = (LandmarkWant){dish_murmur3("Step_MainChallenge"), LANDMARK_SLOT_MAIN};
    want[nw++] = (LandmarkWant){dish_murmur3("IsGet.Parasail"), LANDMARK_SLOT_PARASAIL};
    want[nw++] = (LandmarkWant){dish_murmur3("IsVisitLocation.MinusField"), LANDMARK_SLOT_DEPTHS};
    qsort(want, nw, sizeof *want, landmark_want_cmp);
    uint32_t value[LANDMARK_SLOTS] = {0}; uint8_t got[LANDMARK_SLOTS] = {0};
    uint32_t open = dish_murmur3("Open");
    int found = 0, depths = 0;
    for (size_t i = 0x28; i + 8 <= n; i += 8) {
        uint32_t h = landmark_le32(b + i), v = landmark_le32(b + i + 4);
        if (h == 0xa3db7114u) break;
        LandmarkWant key = {h, 0}, *w = bsearch(&key, want, nw, sizeof *want, landmark_want_cmp);
        if (!w) continue;
        if (landmark_is_lightroot(w->slot)) depths |= v == 1;   /* a Lightroot visited or lit: the Depths are found */
        else if (w->slot >= LANDMARKS && landmark_is_lightroot(w->slot - LANDMARKS)) depths |= v == open;
        value[w->slot] = v; got[w->slot] = 1; found += w->slot < LANDMARK_SLOT_MAIN;
    }
    uint32_t clear = dish_murmur3("Clear"), enter = dish_murmur3("Enter"), not_ready = dish_murmur3("NotReady"), ready = dish_murmur3("Ready");
    for (int k = 0; k < LANDMARKS; k++) {
        int has_visit = got[k], has_done = got[LANDMARKS + k]; uint32_t vv = value[k], dv = value[LANDMARKS + k];
        if (landmark_is_tear(k)) {   /* reached (its memory seen): done */
            if (has_visit) lm[k].state = vv == 1 ? LANDMARK_DONE : LANDMARK_NOT_FOUND;
            continue;
        }
        if (landmark_is_lightroot(k)) {   /* lit: done; visited (its light seen up close) without being lit: found */
            if (!has_visit && !has_done) continue;
            lm[k].state = has_done && dv == open ? LANDMARK_DONE : has_visit && vv == 1 ? LANDMARK_FOUND : LANDMARK_NOT_FOUND;
            continue;
        }
        if (!landmark_is_story(k)) {
            if (!has_visit || !has_done) continue;
            int is_done = k < LANDMARK_TOWERS ? dv == 1 : dv == clear, seen = vv == 1 || (k >= LANDMARK_TOWERS && (dv == open || dv == enter));
            lm[k].state = is_done ? LANDMARK_DONE : seen ? LANDMARK_FOUND : LANDMARK_NOT_FOUND;
            continue;
        }
        const LandmarkStoryPlace *sp = &LANDMARK_STORY_PLACES[k - LANDMARK_FIRST_STORY];
        if (!has_visit && !has_done) continue;
        int visited = has_visit && vv == 1, is_done = has_done && landmark_step_in(dv, sp->done);
        int started = !sp->quest || !has_done || (sp->opens ? landmark_step_in(dv, sp->opens) : dv != not_ready && dv != ready);
        lm[k].state = is_done ? LANDMARK_DONE : visited ? LANDMARK_FOUND : LANDMARK_NOT_FOUND;
        lm[k].opened = started || visited || is_done;
    }
    static const char *const island_steps[] = {"NotReady", "Ready", "GetZonauMajic", "GoToAlterAfterGetRR", "WarpToCaveOfAwake",
                                               "GoToBackToGolemInTemple", "GoToBackOfTempleOfTimeAfterWD", NULL};
    pr.known = got[LANDMARK_SLOT_MAIN];
    pr.island_done = (pr.known && !landmark_step_in(value[LANDMARK_SLOT_MAIN], island_steps)) || (got[LANDMARK_SLOT_PARASAIL] && value[LANDMARK_SLOT_PARASAIL] == 1);
    pr.depths_found = depths || (got[LANDMARK_SLOT_DEPTHS] && value[LANDMARK_SLOT_DEPTHS] == 1);
    if (progress) *progress = pr;
    return found;
}
/* The story lock: with on and a save that says where the story is, each place's reason to be greyed (LANDMARK_LOCK_...).
   Before Great Sky Island is left only its own places are open; the Depths wait for the first visit; a story place for its
   quest (LANDMARK_STORY_PLACES). Map pins are not landmarks and never locked. */
static inline void landmarks_lock(Landmark *lm, const LandmarkProgress *p, int on) {
    for (int k = 0; k < LANDMARKS; k++) {
        Landmark *l = &lm[k]; l->lock = LANDMARK_OPEN;
        if (!on || !p || !p->known) continue;
        if (!p->island_done && !l->island) l->lock = LANDMARK_LOCK_ISLAND;
        else if (l->depths && !p->depths_found) l->lock = LANDMARK_LOCK_DEPTHS;
        else if (!l->opened) l->lock = LANDMARK_LOCK_QUEST;
    }
}
/* The states from the newest save of profile; returns how many flags were found, -1 when there is no save. */
static inline int landmarks_state_read(const char *profile, Landmark *lm, LandmarkProgress *progress) {
    for (int k = 0; k < LANDMARKS; k++) { lm[k].state = LANDMARK_UNKNOWN; lm[k].opened = 1; }
    if (progress) *progress = (LandmarkProgress){0, 0, 0};
    char path[1800]; map_pins_newest_save(profile, path, sizeof path);
    if (!path[0]) return -1;
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    uint8_t *b = malloc(4 << 20); size_t n = b ? fread(b, 1, 4 << 20, f) : 0; fclose(f);
    int got = b ? landmarks_state_parse(b, n, lm, progress) : 0;
    free(b);
    return got;
}
