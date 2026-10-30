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
   instead). Cleared already: talk00 "Do you want to leave?", choices "I'm ready to go." (0: the default and what B picks) and
   "I'll stay." (1); 0 leaves. The monk's lines (talk01..talk11) are messages that A or B advances. GetUI is where the
   Light of Blessing is given (MelonSpeedruns/TotkRandomizer cuts it there, Event4 -> Event52, to remove the orb). 4Return (the start lift) leaves without clearing.
   The kind: Scene/Component/SmallDungeonParam/DungeonNNN.game__scene__SmallDungeonParam.bgyml Type (none: puzzle;
   Gimmick: puzzle; EnemyWithCombine: a fight; Enemy: Proving Grounds, the gear taken at the door and given back by the
   goal; Treasure: the reward of a challenge outside, Rauru's Blessing).
   Activating: the gate is the only A prompt outside (DgnObj_WarpPoint_Zonau_A_01 and the entrance have none). Its 1Open
   is what the game calls activating the shrine: Link is stood on DestinationAnchor "AuthenticationPos" (1.85 m in from
   the entrance, 3.75 m from the travel point, for all 152), touches the mark, the travel point turns on,
   ShrineRegisterNumber goes up, SetOpenOwnGameData (DungeonState Appear -> Open), the entrance opens, an autosave.
   How a player gets in (field test, 2026-10-27): the lift's 2Go starts when Link walks in through the doorway; set down
   there by noclip he does not trigger it. The start lift inside (the same actor class) is walked into the same way.
   Skipping (the flows' EventTriggerSkipAcceptStart / SkipEnd marks; MrCheeze/totk-tools has the same flows as
   text): a case 0 of a yes / no query is "yes" (3Arrive plays Dm_OP_0011 only on the case "Ultrahand not in the pouch").
   1Open is skippable only when the player has seen it before (HavePlayedEvent); 3Arrive's walk-in always; the goal
   (Animation_04, every shrine but Great Sky Island's 060-062, whose Animation_01 is skippable once the goal has been
   seen) always from the touch on. 2Go, 4Return and 5Out have no skippable part. A skip only jumps to SkipEnd: the gate's
   SetOpenOwnGameData and autosave, SetDungeonEnter, the Light of Blessing (GetUI on both branches), SetDungeonClear and
   the exit all come after it on every branch.
   The save: DungeonState.DungeonNNN (Enum Hidden, Appear, Open, Enter, Clear); GameData/GameDataList.Product.140 gives 119
   shrines Appear and 33 Hidden (the 8 hidden and the 25 crystal shrines) at the start. Plain C, header only
   (tests/dish/probe.c checks it). */
#pragma once
#include "landmarks.h"
#include <time.h>

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
    int outside, inside, has_warp, has_lift, has_auth, cave, hidden, crystal, kind, nchests, got;   /* got: SHRINE_GOT_... found inside */
    int sludge;   /* sludge (FldObj_Tar_*) lies over its entrance until the Water Temple's quest is past TouchSecretStone */
    float entrance[3], warp[3], lift[3], lift_floor[3], auth[3];   /* auth: AuthenticationPos, where 1Open stands Link to activate it */
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
enum { SHRINE_FLOORS = 256, SHRINE_WARPS = 512, SHRINE_TARS = 64 };
typedef struct {
    ShrineInfo s[SHRINES];
    float floor[SHRINE_FLOORS][3], warp[SHRINE_WARPS][3], auth[SHRINE_FLOORS][3]; int nfloors, nwarps, nauths;
    float tar[SHRINE_TARS][3]; int ntars;   /* the Zora region's sludge (FldObj_Tar_*) */
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
            else if (name && !strcmp(name, "AuthenticationPos") && sc->nauths < SHRINE_FLOORS) { memcpy(sc->auth[sc->nauths++], xyz, 12); took++; }
        } else if (!strcmp(gyaml, "DgnObj_WarpPoint_Zonau_A_01") && sc->nwarps < SHRINE_WARPS) { memcpy(sc->warp[sc->nwarps++], xyz, 12); took++; }
        else if (!strncmp(gyaml, "FldObj_Tar_", 11) && sc->ntars < SHRINE_TARS) { memcpy(sc->tar[sc->ntars++], xyz, 12); took++; }
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
        int a = landmark_nearest((const float (*)[3])sc->auth, sc->nauths, s->entrance, 3);
        if ((s->has_auth = a >= 0)) memcpy(s->auth, sc->auth[a], 12);
        s->sludge = landmark_nearest((const float (*)[3])sc->tar, sc->ntars, s->entrance, 10) >= 0;
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
       SHRINE_SKIP_DONE, SHRINE_SKIP_FAILED, SHRINE_SKIP_FIGHT, SHRINE_SKIP_PROVING, SHRINE_SKIP_SLUDGE, SHRINE_SKIP_GIVEN_UP, SHRINE_SKIPS };
static const char *const SHRINE_SKIP_TEXT[SHRINE_SKIPS] = {
    "", "its entrance or goal is not in the game's map files", "completed", "locked by the story lock",
    "not revealed yet (its quest raises it)", "its crystal has not been brought yet", "in a cave: the flight down would pass through rock",
    "done in this run", "failed in this run", "needs a fight: the altar's gate opens only after the construct is beaten",
    "Proving Grounds: the altar's gate opens only after the fight",
    "covered in sludge until the Water Temple is done (or wash it off: Splash Fruit, Chuchu Jelly)", "given up earlier"};
/* shrine_skip's flags: SHRINE_GIVEN_UP an earlier run gave it up and the save still says what it said then
   (shrine_given_up_*, below); SHRINE_SLUDGE_ON the save says the Zora region's sludge is still there
   (shrine_sludge_gone). */
enum { SHRINE_GIVEN_UP = 1, SHRINE_SLUDGE_ON = 2 };
/* state: SHRINE_STATE_...; lock: landmarks.h LANDMARK_LOCK_... of the shrine; run: 0, SHRINE_SKIP_DONE or _FAILED; flags:
   SHRINE_GIVEN_UP, SHRINE_SLUDGE_ON. A shrine the save does not mention is taken to be in its starting state (hidden and
   crystal shrines start Hidden). Sludge over the entrance matters only before the gate is opened (Appear or not known). */
static inline int shrine_skip(const ShrineInfo *s, int state, int lock, int run, int flags) {
    if (!s->outside || !s->has_lift || !s->inside) return SHRINE_SKIP_NO_DATA;
    if (state == SHRINE_STATE_CLEAR) return SHRINE_SKIP_CLEAR;
    if (run) return run;
    if (lock != LANDMARK_OPEN) return SHRINE_SKIP_LOCKED;
    int hidden = state == SHRINE_STATE_HIDDEN || (state == SHRINE_STATE_UNKNOWN && (s->hidden || s->crystal));
    if (hidden) return s->crystal ? SHRINE_SKIP_CRYSTAL : SHRINE_SKIP_HIDDEN;
    if (s->cave) return SHRINE_SKIP_CAVE;
    /* A fight's altar answers nothing until the construct is beaten (Miryotanog Shrine, 2026-10-28: A on the altar's floor
       five times, no event) */
    if (s->kind == SHRINE_COMBAT) return SHRINE_SKIP_FIGHT;
    if (s->kind == SHRINE_PROVING) return SHRINE_SKIP_PROVING;
    if (s->sludge && (flags & SHRINE_SLUDGE_ON) && (state == SHRINE_STATE_APPEAR || state == SHRINE_STATE_UNKNOWN)) return SHRINE_SKIP_SLUDGE;
    if (flags & SHRINE_GIVEN_UP) return SHRINE_SKIP_GIVEN_UP;   /* last: a reason above says more */
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
/* Shrines given up in earlier runs (2026-10-29): autoshrine-given-up.txt beside the profile (the folder that holds
   TOTK_PROFILE_DIR, as spawn-picks.txt and cheats-enabled.txt; TOTK_AUTOSHRINE_GIVEN_UP overrides), one line per shrine:
   "DungeonNNN<TAB>reason<TAB>date<TAB>state", state the DungeonState the save had when it was given up. Later runs leave
   it out (SHRINE_SKIP_GIVEN_UP) while the save still says that state; once it changes (the gate opened, the shrine done
   by hand) the line is dropped and the shrine tried again. Places › Shrines' "Retry given-up shrines" removes the file. */
enum { SHRINE_GIVEN_UP_REASON = 200, SHRINE_GIVEN_UP_DATE = 24 };
typedef struct {
    uint8_t on[SHRINES], state[SHRINES];   /* state: SHRINE_STATE_... then */
    char reason[SHRINES][SHRINE_GIVEN_UP_REASON], date[SHRINES][SHRINE_GIVEN_UP_DATE];
} ShrineGivenUp;
static inline int shrine_given_up_path(char *out, size_t n) {
    const char *e = getenv("TOTK_AUTOSHRINE_GIVEN_UP"), *p = getenv("TOTK_PROFILE_DIR");
    if (e && *e) return snprintf(out, n, "%s", e) < (int)n;
    if (!p || !*p) return 0;
    size_t len = strlen(p);
    while (len > 1 && p[len - 1] == '/') len--;
    while (len > 0 && p[len - 1] != '/') len--;   /* the folder that holds the profile */
    if (!len) return snprintf(out, n, "autoshrine-given-up.txt") < (int)n;
    return snprintf(out, n, "%.*sautoshrine-given-up.txt", (int)len, p) < (int)n;
}
/* One field of a line (len bytes of from): tabs and line ends become spaces. */
static inline void shrine_given_up_field(char *out, size_t n, const char *from, size_t len) {
    size_t o = 0;
    for (size_t i = 0; i < len && from[i] && o + 1 < n; i++) out[o++] = from[i] == '\t' || from[i] == '\n' || from[i] == '\r' ? ' ' : from[i];
    if (n) out[o] = 0;
}
/* The file's text (NULL: none) into g; returns how many shrines it names. Lines that do not start with DungeonNNN are skipped. */
static inline int shrine_given_up_parse(ShrineGivenUp *g, const char *text) {
    memset(g, 0, sizeof *g);
    int count = 0;
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n'), *stop = end ? end : line + strlen(line);
        const char *tab = memchr(line, '\t', (size_t)(stop - line)); char name[16];
        shrine_given_up_field(name, sizeof name, line, (size_t)((tab ? tab : stop) - line));
        int k = shrine_index(name);
        if (k >= 0) {
            const char *f[3] = {"", "", ""}; size_t fl[3] = {0, 0, 0}; const char *at = tab;
            for (int i = 0; i < 3 && at; i++) {
                f[i] = at + 1; const char *next = memchr(f[i], '\t', (size_t)(stop - f[i]));
                fl[i] = (size_t)((next ? next : stop) - f[i]); at = next;
            }
            while (fl[2] && (f[2][fl[2] - 1] == '\r' || f[2][fl[2] - 1] == ' ')) fl[2]--;
            count += !g->on[k];
            g->on[k] = 1; g->state[k] = SHRINE_STATE_UNKNOWN;
            shrine_given_up_field(g->reason[k], sizeof g->reason[k], f[0], fl[0]);
            shrine_given_up_field(g->date[k], sizeof g->date[k], f[1], fl[1]);
            for (int v = SHRINE_STATE_HIDDEN; v <= SHRINE_STATE_CLEAR; v++)
                if (fl[2] == strlen(SHRINE_STATE_NAMES[v]) && !strncmp(f[2], SHRINE_STATE_NAMES[v], fl[2])) g->state[k] = (uint8_t)v;
        }
        line = end ? end + 1 : NULL;
    }
    return count;
}
/* g as the file's text into out (n bytes; NULL to measure); returns the length it needs, without the final 0. */
static inline int shrine_given_up_format(const ShrineGivenUp *g, char *out, size_t n) {
    int len = 0;
    for (int k = 0; k < SHRINES; k++) {
        if (!g->on[k]) continue;
        int room = out && (size_t)len < n;
        int w = snprintf(room ? out + len : NULL, room ? n - (size_t)len : 0, "Dungeon%03d\t%s\t%s\t%s\n", k, g->reason[k], g->date[k],
                         g->state[k] <= SHRINE_STATE_CLEAR ? SHRINE_STATE_NAMES[g->state[k]] : "");
        if (w > 0) len += w;
    }
    return len;
}
/* Whether shrine k stays left out: given up, and the save says now what it said then (not known then: whatever it says). */
static inline int shrine_given_up_holds(const ShrineGivenUp *g, int k, int state_now) {
    return k >= 0 && k < SHRINES && g->on[k] && (g->state[k] == SHRINE_STATE_UNKNOWN || g->state[k] == state_now);
}
/* Records shrine k: the reason, the date (local, YYYY-MM-DD HH:MM) and the save's state now. */
static inline void shrine_given_up_add(ShrineGivenUp *g, int k, const char *reason, int state_now, time_t when) {
    if (k < 0 || k >= SHRINES) return;
    struct tm tm; char date[SHRINE_GIVEN_UP_DATE] = "";
    if (localtime_r(&when, &tm)) strftime(date, sizeof date, "%Y-%m-%d %H:%M", &tm);
    if (!reason) reason = "";
    g->on[k] = 1; g->state[k] = (uint8_t)(state_now >= 0 && state_now <= SHRINE_STATE_CLEAR ? state_now : SHRINE_STATE_UNKNOWN);
    shrine_given_up_field(g->reason[k], sizeof g->reason[k], reason, strlen(reason));
    snprintf(g->date[k], sizeof g->date[k], "%s", date);
}
/* The file into g (none: empty); returns how many shrines it names. */
static inline int shrine_given_up_load(ShrineGivenUp *g) {
    char path[1024]; FILE *f = shrine_given_up_path(path, sizeof path) ? fopen(path, "rb") : NULL;
    char *text = f ? malloc(64 << 10) : NULL; size_t n = text ? fread(text, 1, (64 << 10) - 1, f) : 0;
    if (f) fclose(f);
    if (text) text[n] = 0;
    int count = shrine_given_up_parse(g, text);
    free(text);
    return count;
}
/* g into the file through a temporary one (a crash leaves the old one); none left: the file removed. 0 on success. */
static inline int shrine_given_up_save(const ShrineGivenUp *g) {
    char path[1024], tmp[1040];
    if (!shrine_given_up_path(path, sizeof path)) return -1;
    int len = shrine_given_up_format(g, NULL, 0);
    if (!len) { remove(path); return 0; }
    char *text = malloc((size_t)len + 1); if (!text) return -1;
    shrine_given_up_format(g, text, (size_t)len + 1);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); int ok = f && fwrite(text, 1, (size_t)len, f) == (size_t)len;
    if (f && fclose(f)) ok = 0;
    free(text);
    if (!ok || rename(tmp, path)) { remove(tmp); return -1; }
    return 0;
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
/* The Zora region's sludge (researched 2026-10-29): the FldObj_Tar_* actors (the Mucktorok's sludge) are placed only while
   SageOfZora_IsAfter_TouchSecretStone_Exp is false (their Presence flag with IsNegation; radar-totk.zeldamods.org,
   object 0x47704d9ad4a6ef8c), which is Step_SageOfZora > TouchSecretStone (MrCheeze/totk-tools boolexp.c): the steps after
   it are Finish and Complete (as the Wind Temple's, landmarks.h LANDMARK_WIND_DONE). In 1.4.3 one lies over a shrine's
   entrance, 5 m from it: Ihen-a Shrine (Dungeon041, Upland Zorana); every other one is 55 m or more from a shrine. Before
   the Water Temple a player washes it off (Splash Fruit, Chuchu Jelly, a hydrant). 1: gone, 0: still there, -1: the save
   does not say. */
static inline int shrine_sludge_gone(const uint8_t *b, size_t n) {
    if (n < 0x30 || landmark_le32(b) != 0x01020304) return -1;
    uint32_t want = dish_murmur3("Step_SageOfZora"), finish = dish_murmur3("Finish"), complete = dish_murmur3("Complete");
    for (size_t i = 0x28; i + 8 <= n; i += 8) {
        uint32_t h = landmark_le32(b + i), v = landmark_le32(b + i + 4);
        if (h == 0xa3db7114u) break;
        if (h == want) return v == finish || v == complete;
    }
    return -1;
}

/* Where Link lands for each step and what he faces (stand, 0.3 m above the floor; face: the A prompt's actor).
   The gate: on the travel point, facing the entrance. The lift: 1 m out from its prompt toward the travel point, on the
   lift's floor, facing the prompt. The goal: on SmallDungeonExit (where the goal event wants him), facing the altar. The
   start lift: on LeaveSmallDungeonPos, facing its prompt. A chest: 1.3 m in front of it, facing it.
   The approach (walking in): 1 m further out than the travel point on the line from the lift out through the entrance,
   facing the lift (face is where he walks to). The start lift's approach: 3 m back from its floor toward the shrine's
   start, facing the lift's prompt. Activating: 0.5 m out from AuthenticationPos (without it, 1.85 m in from the entrance),
   facing in (a point 5 m further in); a place he walks to, never one he is set down at: AuthenticationPos is inside the
   closed entrance, and set down there the game fades out and puts him back (field test, 2026-10-28, Ukouh Shrine).
   The travel point: where every shrine visit lands (the Shrines tab's Go, landmarks.h): the travel point 1.5 m up,
   facing the entrance's AuthenticationPos. The altar's approach: 2.5 m back from SmallDungeonExit, away from the altar,
   facing its prompt (DgnObj_GoalExit); he walks up from there (the same three actors in every shrine, Rauru's Blessing
   ones included). */
enum { SHRINE_AT_GATE, SHRINE_AT_LIFT, SHRINE_AT_GOAL, SHRINE_AT_BACK, SHRINE_AT_CHEST, SHRINE_AT_APPROACH, SHRINE_AT_BACK_APPROACH, SHRINE_AT_ACTIVATE, SHRINE_AT_TRAVEL, SHRINE_AT_GOAL_APPROACH };
static inline void shrine_unit_xz(const float from[3], const float to[3], float out[2]) {
    float dx = to[0] - from[0], dz = to[2] - from[2], l = sqrtf(dx * dx + dz * dz);
    out[0] = l > 1e-3f ? dx / l : 0; out[1] = l > 1e-3f ? dz / l : 1;
}
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
    case SHRINE_AT_APPROACH: {
        const float *out = s->has_warp ? s->warp : s->entrance; float in[2]; shrine_unit_xz(s->entrance, s->lift, in);
        stand[0] = out[0] - in[0]; stand[1] = out[1]; stand[2] = out[2] - in[1]; memcpy(face, s->lift_floor, 12);
        break;
    }
    case SHRINE_AT_ACTIVATE: {
        float in[2]; shrine_unit_xz(s->entrance, s->lift, in);
        if (s->has_auth) memcpy(stand, s->auth, 12);
        else { stand[0] = s->entrance[0] + 1.85f * in[0]; stand[1] = s->entrance[1] + 0.45f; stand[2] = s->entrance[2] + 1.85f * in[1]; }
        stand[0] -= 0.5f * in[0]; stand[2] -= 0.5f * in[1];
        face[0] = stand[0] + 5 * in[0]; face[1] = stand[1]; face[2] = stand[2] + 5 * in[1];
        break;
    }
    case SHRINE_AT_TRAVEL: {
        float in[2]; shrine_unit_xz(s->entrance, s->lift, in);
        if (s->has_warp) memcpy(stand, s->warp, 12);
        else { stand[0] = s->entrance[0] - 1.9f * in[0]; stand[1] = s->entrance[1]; stand[2] = s->entrance[2] - 1.9f * in[1]; }
        stand[1] += 1.2f;   /* 1.5 m up with the 0.3 below */
        if (s->has_auth) memcpy(face, s->auth, 12); else memcpy(face, s->entrance, 12);
        break;
    }
    case SHRINE_AT_GOAL_APPROACH: {
        float in[2]; shrine_unit_xz(s->goal_floor, s->goal, in);
        stand[0] = s->goal_floor[0] - 2.5f * in[0]; stand[1] = s->goal_floor[1]; stand[2] = s->goal_floor[2] - 2.5f * in[1]; memcpy(face, s->goal_exit, 12);
        break;
    }
    case SHRINE_AT_BACK_APPROACH: {
        float in[2]; shrine_unit_xz(s->start, s->back, in);
        stand[0] = s->back_floor[0] - 3 * in[0]; stand[1] = s->back_floor[1]; stand[2] = s->back_floor[2] - 3 * in[1]; memcpy(face, s->back, 12);
        break;
    }
    default: {
        const float *c = s->chest[chest], *f = s->chest_front[chest];
        stand[0] = c[0] + 1.3f * f[0]; stand[1] = c[1]; stand[2] = c[2] + 1.3f * f[1]; memcpy(face, c, 12);
    }
    }
    stand[1] += 0.3f;
}

/* The left stick as the keyboard gives it (W A S D, eight directions): the keys that walk Link toward dir, given the
   camera's forward and right (all unit x, z). SHRINE_STICK_... bits; 0 when dir is unknown. */
enum { SHRINE_STICK_UP = 1, SHRINE_STICK_DOWN = 2, SHRINE_STICK_LEFT = 4, SHRINE_STICK_RIGHT = 8 };
static inline int shrine_stick(const float fwd[2], const float right[2], const float dir[2]) {
    float y = dir[0] * fwd[0] + dir[1] * fwd[1], x = dir[0] * right[0] + dir[1] * right[1]; int k = 0;
    if (y > 0.38f) k |= SHRINE_STICK_UP; else if (y < -0.38f) k |= SHRINE_STICK_DOWN;
    if (x > 0.38f) k |= SHRINE_STICK_RIGHT; else if (x < -0.38f) k |= SHRINE_STICK_LEFT;
    return k;
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
