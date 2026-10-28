/* Auto shrine (Mac Tools › Shrines › Complete shrines automatically; world_survey.c runs it): what a run needs of each of
   the 152 shrines, read from the game's own RomFS, and the run's pure choices (which shrines it can do, which one is next,
   where Link stands for each step). The game completes each shrine itself; nothing here writes a flag.
   What the files say (1.4.3, read 2026-10-27: the Banc map files and the Event/EventFlow flows):
   Outside (the Banc/MainField static map files): the entrance DgnObj_ZonauShrineEntrance_A_01 (Dynamic.DungeonIndexStr "DungeonNNN";
   IsHiddenSmallDungeon on 8, IsKeyCrystalAuthSmallDungeon on 25), its travel point DgnObj_WarpPoint_Zonau_A_01 1.9 m out
   in front, its gate DgnObj_ZonauShrineEntranceGate_A_01 (an A prompt the first time: event DmF_SY_SmallDungeon1Open, Link
   authenticates and the shrine opens), and inside the doorway the lift: FldObj_EnterAttetionActor (Dynamic.DungeonIndexStr,
   RequestEventName DmF_SY_SmallDungeon2Go; an A prompt) with DestinationAnchor "EnterSmallDungeonPos" within 3 m, 12 m in
   from the entrance. 2Go walks Link onto the lift, fades out and changes the map to Banc/SmallDungeon/DungeonNNN; there
   DmF_SY_SmallDungeon3Arrive puts him on StartPos "OnElevator" (on Great Sky Island it first plays the ability's scene).
   Inside (Banc/SmallDungeon/DungeonNNN_Static / _Dynamic, every shrine): DgnObj_Hrl_GoalSystem_B_01 (the altar with the
   monk), DgnObj_GoalExit 7 m in front of it (an A prompt, "DungeonGoalExit"), DestinationAnchor "SmallDungeonExit" 0.75 m
   behind that, two EventRequestActor DmF_SY_SmallDungeonGoal; the start lift FldObj_EnterAttetionActor
   DmF_SY_SmallDungeon4Return with DestinationAnchor "LeaveSmallDungeonPos"; chests TBox_Dungeon_Stone / _Iron / _Wood
   (1 in 142 shrines, more in 10). Nothing links the altar to a puzzle: the goal works wherever Link reaches it.
   DmF_SY_SmallDungeonGoal, not cleared yet: the blessing (Touch, Light of Blessing, the "Get" screen), then
   EventTriggerSetDungeonClear and EventReserveBancChangeExitSmallDungeon: the game leaves the shrine by itself (boot event
   DmF_SY_SmallDungeon5Out: Link in front of the entrance, then an autosave; on Great Sky Island the island's story scenes
   instead). Cleared already: a Yes / No question to leave. 4Return (the start lift) leaves without clearing.
   The kind: Scene/Component/SmallDungeonParam/DungeonNNN.game__scene__SmallDungeonParam.bgyml Type (none: puzzle;
   Gimmick: puzzle; EnemyWithCombine: a fight; Enemy: Proving Grounds, the gear taken at the door and given back by the
   goal; Treasure: the reward of a challenge outside, Rauru's Blessing).
   The save: DungeonState.DungeonNNN (Enum Hidden, Appear, Open, Enter, Clear); GameData/GameDataList.Product.140 gives 119
   shrines Appear and 33 Hidden (the 8 hidden and the 25 crystal shrines) at the start. Plain C, header only
   (tests/dish/probe.c checks it). */
#pragma once
#include "landmarks.h"

enum { SHRINES = LANDMARK_SHRINES, SHRINE_CHESTS = 6 };
enum { SHRINE_PUZZLE, SHRINE_COMBAT, SHRINE_PROVING, SHRINE_BLESSING };
static const char *const SHRINE_KIND_TEXT[] = {"puzzle", "fight", "Proving Grounds", "Rauru's Blessing"};
/* DungeonState.DungeonNNN in the save; SHRINE_STATE_UNKNOWN: not in it (or no save). */
enum { SHRINE_STATE_UNKNOWN, SHRINE_STATE_HIDDEN, SHRINE_STATE_APPEAR, SHRINE_STATE_OPEN, SHRINE_STATE_ENTER, SHRINE_STATE_CLEAR };
static const char *const SHRINE_STATE_NAMES[] = {"", "Hidden", "Appear", "Open", "Enter", "Clear"};
/* One shrine. Outside, map coordinates: entrance, warp (its travel point; has_warp), lift (the inner lift's A prompt) and
   lift_floor (EnterSmallDungeonPos, where 2Go puts Link). Inside, the shrine's own coordinates: start (OnElevator), goal
   (the altar), goal_exit (its A prompt), goal_floor (SmallDungeonExit), back / back_floor (the start lift's A prompt and
   LeaveSmallDungeonPos), chests with each one's front (unit x, z). */
enum { SHRINE_GOT_START = 1, SHRINE_GOT_GOAL = 2, SHRINE_GOT_GOAL_EXIT = 4, SHRINE_GOT_GOAL_FLOOR = 8, SHRINE_GOT_BACK = 16, SHRINE_GOT_BACK_FLOOR = 32,
       SHRINE_GOT_NEEDED = 15 };
typedef struct {
    int outside, inside, has_warp, has_lift, cave, hidden, crystal, kind, nchests, got;   /* got: SHRINE_GOT_... found inside */
    float entrance[3], warp[3], lift[3], lift_floor[3];
    float start[3], goal[3], goal_exit[3], goal_floor[3], back[3], back_floor[3];
    float chest[SHRINE_CHESTS][3], chest_front[SHRINE_CHESTS][2];
} ShrineInfo;

/* The front of a placed actor (its local +z) from its Rotate (radians, x then y then z: R = Rz Ry Rx), as unit x, z. */
static inline void shrine_front(const float r[3], float out[2]) {
    float ca = cosf(r[0]), sa = sinf(r[0]), sb = sinf(r[1]), cb = cosf(r[1]), cc = cosf(r[2]), sc = sinf(r[2]);
    float x = ca * sb * cc + sa * sc, z = ca * cb, l = sqrtf(x * x + z * z);
    if (l < 1e-4f) { out[0] = 0; out[1] = 1; return; }
    out[0] = x / l; out[1] = z / l;
}
static inline int shrine_index(const char *s) {   /* "DungeonNNN" -> NNN, -1 otherwise */
    int k; char end;
    return s && sscanf(s, "Dungeon%d%c", &k, &end) == 1 && k >= 0 && k < SHRINES ? k : -1;
}
static inline int shrine_byml_bool(const LandmarkByml *b, uint32_t node, const char *key) {
    uint8_t t; uint32_t v; return landmark_byml_get(b, node, key, &t, &v) && t == 0xD0 && v;
}

/* What the passes over the map files collect before the shrines are placed: the entrances and their lifts by shrine, and
   the lift floors and travel points by position. */
enum { SHRINE_FLOORS = 256, SHRINE_WARPS = 512 };
typedef struct {
    ShrineInfo s[SHRINES];
    float floor[SHRINE_FLOORS][3], warp[SHRINE_WARPS][3]; int nfloors, nwarps;
} ShrineScan;
/* One decompressed map file (.bcett.byml); cave: it is under Banc/MainField/Cave/. Returns the actors it took, -1 when it
   is not a map file. */
static inline int shrine_scan_field(const uint8_t *d, size_t n, int cave, ShrineScan *sc) {
    if (n < 16 || d[0] != 'Y' || d[1] != 'B') return -1;
    LandmarkByml b = {d, n, landmark_le32(d + 4), landmark_le32(d + 8)};
    uint8_t t; uint32_t actors, values;
    if (!landmark_byml_get(&b, landmark_le32(d + 12), "Actors", &t, &actors) || t != 0xC0) return -1;
    long count = landmark_byml_array(&b, actors, &values); int took = 0;
    for (long i = 0; i < count; i++) {
        if (b.d[actors + 4 + i] != 0xC1) continue;
        uint32_t actor = landmark_le32(b.d + values + 4 * i), tr, dyn = 0;
        const char *gyaml = landmark_byml_text(&b, actor, "Gyaml"); float xyz[3];
        if (!gyaml || !landmark_byml_get(&b, actor, "Translate", &t, &tr) || t != 0xC0 || !landmark_byml_vec3(&b, tr, xyz)) continue;
        if (!landmark_byml_get(&b, actor, "Dynamic", &t, &dyn) || t != 0xC1) dyn = 0;
        int k;
        if (!strcmp(gyaml, "DgnObj_ZonauShrineEntrance_A_01") && (k = shrine_index(landmark_byml_text(&b, dyn, "DungeonIndexStr"))) >= 0) {
            ShrineInfo *s = &sc->s[k]; memcpy(s->entrance, xyz, 12); s->outside = 1; s->cave = cave;
            s->hidden = shrine_byml_bool(&b, dyn, "IsHiddenSmallDungeon"); s->crystal = shrine_byml_bool(&b, dyn, "IsKeyCrystalAuthSmallDungeon"); took++;
        } else if (!strcmp(gyaml, "FldObj_EnterAttetionActor") && (k = shrine_index(landmark_byml_text(&b, dyn, "DungeonIndexStr"))) >= 0) {
            const char *ev = landmark_byml_text(&b, dyn, "RequestEventName");
            if (ev && !strcmp(ev, "DmF_SY_SmallDungeon2Go")) { memcpy(sc->s[k].lift, xyz, 12); sc->s[k].has_lift = 1; took++; }
        } else if (!strcmp(gyaml, "DestinationAnchor") && sc->nfloors < SHRINE_FLOORS) {
            const char *name = landmark_byml_text(&b, actor, "Name");
            if (name && !strcmp(name, "EnterSmallDungeonPos")) { memcpy(sc->floor[sc->nfloors++], xyz, 12); took++; }
        } else if (!strcmp(gyaml, "DgnObj_WarpPoint_Zonau_A_01") && sc->nwarps < SHRINE_WARPS) { memcpy(sc->warp[sc->nwarps++], xyz, 12); took++; }
    }
    return took;
}
/* Each shrine's travel point (within 5 m of its entrance) and lift floor (within 4 m of its lift); returns how many
   shrines have an entrance and a lift. Without a floor the lift's own place, 1 m down, stands in. */
static inline int shrine_place_field(ShrineScan *sc) {
    int ok = 0;
    for (int k = 0; k < SHRINES; k++) {
        ShrineInfo *s = &sc->s[k];
        if (!s->outside) continue;
        int w = landmark_nearest((const float (*)[3])sc->warp, sc->nwarps, s->entrance, 5);
        if ((s->has_warp = w >= 0)) memcpy(s->warp, sc->warp[w], 12);
        if (!s->has_lift) continue;
        int f = landmark_nearest((const float (*)[3])sc->floor, sc->nfloors, s->lift, 4);
        if (f >= 0) memcpy(s->lift_floor, sc->floor[f], 12);
        else { memcpy(s->lift_floor, s->lift, 12); s->lift_floor[1] -= 1; }
        ok++;
    }
    return ok;
}
/* One of a shrine's own map files (DungeonNNN_Static / _Dynamic, decompressed): the start, the goal, the start lift and
   the chests. Returns the actors it took, -1 when it is not a map file. s->inside is set once the start and the goal with
   its prompt and floor are known (the start is in _Dynamic, the goal in _Static). */
static inline int shrine_scan_inside(const uint8_t *d, size_t n, ShrineInfo *s) {
    if (n < 16 || d[0] != 'Y' || d[1] != 'B') return -1;
    LandmarkByml b = {d, n, landmark_le32(d + 4), landmark_le32(d + 8)};
    uint8_t t; uint32_t actors, values;
    if (!landmark_byml_get(&b, landmark_le32(d + 12), "Actors", &t, &actors) || t != 0xC0) return -1;
    long count = landmark_byml_array(&b, actors, &values); int took = 0;
    for (long i = 0; i < count; i++) {
        if (b.d[actors + 4 + i] != 0xC1) continue;
        uint32_t actor = landmark_le32(b.d + values + 4 * i), tr, rt, dyn = 0;
        const char *gyaml = landmark_byml_text(&b, actor, "Gyaml"), *name = landmark_byml_text(&b, actor, "Name"); float xyz[3], rot[3] = {0, 0, 0};
        if (!gyaml || !landmark_byml_get(&b, actor, "Translate", &t, &tr) || t != 0xC0 || !landmark_byml_vec3(&b, tr, xyz)) continue;
        if (landmark_byml_get(&b, actor, "Rotate", &t, &rt) && t == 0xC0) landmark_byml_vec3(&b, rt, rot);
        if (!landmark_byml_get(&b, actor, "Dynamic", &t, &dyn) || t != 0xC1) dyn = 0;
        const char *ev = landmark_byml_text(&b, dyn, "RequestEventName");
        float *to = NULL; int got = 0;
        if (!strcmp(gyaml, "StartPos") && name && !strcmp(name, "OnElevator")) { to = s->start; got = SHRINE_GOT_START; }
        else if (!strcmp(gyaml, "DgnObj_Hrl_GoalSystem_B_01")) { to = s->goal; got = SHRINE_GOT_GOAL; }
        else if (!strcmp(gyaml, "DgnObj_GoalExit")) { to = s->goal_exit; got = SHRINE_GOT_GOAL_EXIT; }
        else if (!strcmp(gyaml, "DestinationAnchor") && name && !strcmp(name, "SmallDungeonExit")) { to = s->goal_floor; got = SHRINE_GOT_GOAL_FLOOR; }
        else if (!strcmp(gyaml, "FldObj_EnterAttetionActor") && ev && !strcmp(ev, "DmF_SY_SmallDungeon4Return")) { to = s->back; got = SHRINE_GOT_BACK; }
        else if (!strcmp(gyaml, "DestinationAnchor") && name && !strcmp(name, "LeaveSmallDungeonPos")) { to = s->back_floor; got = SHRINE_GOT_BACK_FLOOR; }
        if (to) { memcpy(to, xyz, 12); s->got |= got; took++; }
        else if (!strncmp(gyaml, "TBox_", 5) && s->nchests < SHRINE_CHESTS) {
            memcpy(s->chest[s->nchests], xyz, 12); shrine_front(rot, s->chest_front[s->nchests]); s->nchests++; took++;
        }
    }
    s->inside = (s->got & SHRINE_GOT_NEEDED) == SHRINE_GOT_NEEDED;
    return took;
}
/* The kind from a SmallDungeonParam file (a BYML dictionary; Type missing: a puzzle). */
static inline int shrine_kind_parse(const uint8_t *d, size_t n) {
    if (n < 16 || d[0] != 'Y' || d[1] != 'B') return SHRINE_PUZZLE;
    LandmarkByml b = {d, n, landmark_le32(d + 4), landmark_le32(d + 8)};
    const char *type = landmark_byml_text(&b, landmark_le32(d + 12), "Type");
    if (!type) return SHRINE_PUZZLE;
    return !strcmp(type, "Enemy") ? SHRINE_PROVING : !strcmp(type, "EnemyWithCombine") ? SHRINE_COMBAT : !strcmp(type, "Treasure") ? SHRINE_BLESSING : SHRINE_PUZZLE;
}

/* Why the run leaves a shrine out (SHRINE_GO: it does it). Text in SHRINE_SKIP_TEXT. */
enum { SHRINE_GO, SHRINE_SKIP_NO_DATA, SHRINE_SKIP_CLEAR, SHRINE_SKIP_LOCKED, SHRINE_SKIP_HIDDEN, SHRINE_SKIP_CRYSTAL, SHRINE_SKIP_CAVE,
       SHRINE_SKIP_DONE, SHRINE_SKIP_FAILED, SHRINE_SKIPS };
static const char *const SHRINE_SKIP_TEXT[SHRINE_SKIPS] = {
    "", "its entrance or goal is not in the game's map files", "completed", "locked by the story lock",
    "not revealed yet (its quest raises it)", "its crystal has not been brought yet", "in a cave: the flight down would pass through rock",
    "done in this run", "failed in this run"};
/* state: SHRINE_STATE_...; lock: landmarks.h LANDMARK_LOCK_... of the shrine; run: 0, SHRINE_SKIP_DONE or _FAILED. A shrine
   the save does not mention is taken to be in its starting state (hidden and crystal shrines start Hidden). */
static inline int shrine_skip(const ShrineInfo *s, int state, int lock, int run) {
    if (!s->outside || !s->has_lift || !s->inside) return SHRINE_SKIP_NO_DATA;
    if (state == SHRINE_STATE_CLEAR) return SHRINE_SKIP_CLEAR;
    if (run) return run;
    if (lock != LANDMARK_OPEN) return SHRINE_SKIP_LOCKED;
    int hidden = state == SHRINE_STATE_HIDDEN || (state == SHRINE_STATE_UNKNOWN && (s->hidden || s->crystal));
    if (hidden) return s->crystal ? SHRINE_SKIP_CRYSTAL : SHRINE_SKIP_HIDDEN;
    if (s->cave) return SHRINE_SKIP_CAVE;
    return SHRINE_GO;
}
/* The shrine to do next: the nearest entrance to from among those with skip[k] == SHRINE_GO; -1 when none. */
static inline int shrine_next(const ShrineInfo *s, const int *skip, const float from[3]) {
    int best = -1; float bd = 0;
    for (int k = 0; k < SHRINES; k++) {
        if (skip[k] != SHRINE_GO) continue;
        float d = landmark_distance(s[k].entrance, from);
        if (best < 0 || d < bd) { best = k; bd = d; }
    }
    return best;
}
/* Each shrine's DungeonState in a progress.sav image (map_pins.h has the layout); returns how many were found. */
static inline int shrine_states_parse(const uint8_t *b, size_t n, uint8_t *state) {
    memset(state, SHRINE_STATE_UNKNOWN, SHRINES);
    if (n < 0x30 || landmark_le32(b) != 0x01020304) return 0;
    uint32_t want[SHRINES], value[5]; char name[40];
    for (int k = 0; k < SHRINES; k++) { snprintf(name, sizeof name, "DungeonState.Dungeon%03d", k); want[k] = dish_murmur3(name); }
    for (int v = 0; v < 5; v++) value[v] = dish_murmur3(SHRINE_STATE_NAMES[v + 1]);
    int found = 0;
    for (size_t i = 0x28; i + 8 <= n; i += 8) {
        uint32_t h = landmark_le32(b + i), v = landmark_le32(b + i + 4);
        if (h == 0xa3db7114u) break;
        for (int k = 0; k < SHRINES; k++) {
            if (h != want[k]) continue;
            for (int e = 0; e < 5; e++) if (v == value[e]) { state[k] = (uint8_t)(SHRINE_STATE_HIDDEN + e); found++; }
            break;
        }
    }
    return found;
}

/* Where Link lands for each step and what he faces (stand, 0.3 m above the floor; face: the A prompt's actor).
   The gate: on the travel point, facing the entrance. The lift: 1 m out from its prompt toward the travel point, on the
   lift's floor, facing the prompt. The goal: on SmallDungeonExit (where the goal event wants him), facing the altar. The
   start lift: on LeaveSmallDungeonPos, facing its prompt. A chest: 1.3 m in front of it, facing it. */
enum { SHRINE_AT_GATE, SHRINE_AT_LIFT, SHRINE_AT_GOAL, SHRINE_AT_BACK, SHRINE_AT_CHEST };
static inline void shrine_stand(const ShrineInfo *s, int where, int chest, float stand[3], float face[3]) {
    switch (where) {
    case SHRINE_AT_GATE:
        memcpy(stand, s->has_warp ? s->warp : s->entrance, 12); memcpy(face, s->entrance, 12);
        if (!s->has_warp) stand[2] += 2;
        break;
    case SHRINE_AT_LIFT: {
        const float *out = s->has_warp ? s->warp : s->entrance;
        float dx = out[0] - s->lift[0], dz = out[2] - s->lift[2], l = sqrtf(dx * dx + dz * dz);
        stand[0] = s->lift[0] + (l > 0.1f ? dx / l : 0); stand[1] = s->lift_floor[1]; stand[2] = s->lift[2] + (l > 0.1f ? dz / l : 0);
        memcpy(face, s->lift, 12);
        break;
    }
    case SHRINE_AT_GOAL: memcpy(stand, s->goal_floor, 12); memcpy(face, s->goal, 12); break;
    case SHRINE_AT_BACK: memcpy(stand, s->back_floor, 12); memcpy(face, s->back, 12); break;
    default: {
        const float *c = s->chest[chest], *f = s->chest_front[chest];
        stand[0] = c[0] + 1.3f * f[0]; stand[1] = c[1]; stand[2] = c[2] + 1.3f * f[1]; memcpy(face, c, 12);
    }
    }
    stand[1] += 0.3f;
}

/* The RomFS's shrine data: the map's static files (entrances, lifts, travel points), each shrine's own two map files and
   its kind. Returns how many shrines have everything (152 in 1.4.3), 0 when this is not the game. About as long as
   landmarks_load: a few hundred files. */
static inline int shrines_load(const LandmarkRomfs *r, ShrineInfo *out) {
    ShrineScan *sc = calloc(1, sizeof *sc); if (!sc) return 0;
    for (size_t i = 0, count = r->count(); i < count; i++) {
        const char *p = r->name(i); if (!p) continue;
        if (strncmp(p, "Banc/MainField/", 15) || !landmark_ends(p, "_Static.bcett.byml.zs")) continue;
        size_t n; uint8_t *d = landmark_read(r, p, &n);
        if (d) shrine_scan_field(d, n, !strncmp(p + 15, "Cave/", 5), sc);
        free(d);
    }
    shrine_place_field(sc);
    int ready = 0;
    for (int k = 0; k < SHRINES; k++) {
        ShrineInfo *s = &sc->s[k]; char path[128]; size_t n;
        for (int f = 0; f < 2; f++) {
            snprintf(path, sizeof path, "Banc/SmallDungeon/Dungeon%03d_%s.bcett.byml.zs", k, f ? "Dynamic" : "Static");
            uint8_t *d = landmark_read(r, path, &n);
            if (d) shrine_scan_inside(d, n, s);
            free(d);
        }
        snprintf(path, sizeof path, "Scene/Component/SmallDungeonParam/Dungeon%03d.game__scene__SmallDungeonParam.bgyml", k);
        uint8_t *d = landmark_read(r, path, &n);
        s->kind = d ? shrine_kind_parse(d, n) : SHRINE_PUZZLE;
        free(d);
        ready += s->outside && s->has_lift && s->inside;
    }
    memcpy(out, sc->s, sizeof sc->s);
    free(sc);
    return ready;
}
