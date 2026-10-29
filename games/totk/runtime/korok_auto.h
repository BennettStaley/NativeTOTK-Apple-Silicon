/* Auto korok (Mac Places › Koroks; world_survey.c runs it): the hidden koroks of the surface and the sky, read from the
   game's own RomFS, which of them the newest save has found, and the run's pure choices (which ones it does, which one is
   next, the steps for each kind of puzzle). The game finds each korok itself; nothing here writes a flag.
   What the files say (1.4.3, read 2026-10-28; Banc/MainField/HiddenKorok/MainField.hiddenkorok.byml.zs lists the same 800):
   A korok is an Npc_HiddenKorokGround or Npc_HiddenKorokFly placed in the surface's and the sky's static map files
   (Banc/MainField/<square>_Static and Banc/MainField/Sky/<island>_Static; 8 more in LargeDungeon/LargeDungeonHyruleCastle, left out),
   with Dynamic.HideType, the kind of puzzle (absent: 0). Its puzzle is an AiGroup of the same file (the map file's
   "AiGroups": a Logic module and References, each an actor of the file by its Hash, or a sub-group "<dir>/AiGroup" with
   its own Logic): the korok, the Areas it watches and the props. Every Logic module watches an Area with
   SensorHitInfo (the player's body in it) and some ask the player's state too (strings of Logic/HiddenKorok_*.ainb):
     0 LiftUpRock      the rock (Obj_LiftRock*_Korok_A_01) right on the korok is lifted (ActorLogicKorokLiftRock
                       "Lifted"); the UnderIvy / InIce / UnderThorns / UnderPushRock kinds first need the ivy burnt, the
                       ice melted, ... (left out here).
     1 LeafEffect      the player in the Area (a sphere, Dynamic.RigidBodyShapeType 0, as big as its Scale, its top on the
                       korok): the stationary sparkle.
     2 Diving          the player in the Area (a flat cylinder at the water) in the WaterDive state: a dive from high up.
     3 TrackingFlower  an Area at each flower: the First (Obj_Plant_Korok_A_01 TrackingFlowerType 1), the ones on the way,
                       the Last (2); each one touched disappears and shows the next.
     4 TimeLimitRing   the start stump's Area (TimeLimitRingStart), then the goal ring's (TimeLimitRingGoal) within the
                       goal's Dynamic.TimeLimit seconds.
     11 AirlyJump      the start block's Area while Grounded, then the goal block's Area while Grounded, touching no
                       ground on the way.
     18 StickWall      the player in a small Area on a block.
     23 CeilingClipper the Lower Area (under a ceiling; its hit holds a little while after he leaves it) and then the
                       Upper Area (on top of it): Ascend through the ceiling.
   An Area's part in the puzzle is the Logic of the nearest sub-group above it in the path (its own folder first):
   ...Start / Goal / Lower / Upper / First / OnTheWay / Last.
   Once a puzzle is solved the korok's own AI requests DmF_SY_HideCharaDiscovery with itself as the starter (the flow
   keeps the player lifting the rock: EventTriggerPlayerRequestKeepLifting; in the air it opens the paraglider); its
   messages (Npc_HiddenKorokGround Talk03 / Talk06, the seed's Get screen, Talk07) have no choices and A or B advance them;
   it sets IsAppearKorok.<the korok's Hash in decimal> (Bool) in the game data, in the save after the next autosave.
   Plain C, header only (tests/dish/probe.c checks it). */
#pragma once
#include "landmarks.h"

enum { KOROKS = 1024, KOROK_PARTS = 12 };
/* HideType values of the eight kinds a run does (tier 1); the other 14 kinds are left out. */
enum { KOROK_HIDE_ROCK = 0, KOROK_HIDE_LEAF = 1, KOROK_HIDE_DIVING = 2, KOROK_HIDE_FLOWERS = 3, KOROK_HIDE_RING = 4,
       KOROK_HIDE_JUMP = 11, KOROK_HIDE_STICK = 18, KOROK_HIDE_CEILING = 23 };
/* The kinds in the Koroks tab's order: HideType, the word the input file uses ("autokorok type rock off"), the tab's name. */
typedef struct { int hide; const char *word, *name, *what; } KorokType;
static const KorokType KOROK_TYPES[] = {
    {KOROK_HIDE_ROCK, "rock", "Rock", "Lift the rock (plain rocks only)"},
    {KOROK_HIDE_LEAF, "leaf", "Sparkle", "Touch the stationary sparkle"},
    {KOROK_HIDE_DIVING, "dive", "Dive", "Dive into the water through the lily pads"},
    {KOROK_HIDE_FLOWERS, "flowers", "Flowers", "Follow the flowers"},
    {KOROK_HIDE_RING, "ring", "Race", "Race from the stump to the ring in time"},
    {KOROK_HIDE_JUMP, "jump", "Block jump", "Jump from the start block to the goal block"},
    {KOROK_HIDE_STICK, "stickwall", "Wall target", "Reach the target block"},
    {KOROK_HIDE_CEILING, "ceiling", "Ceiling", "Ascend through the ceiling"},
};
enum { KOROK_NTYPES = sizeof KOROK_TYPES / sizeof *KOROK_TYPES };
static inline int korok_type_of(int hide) { for (int t = 0; t < KOROK_NTYPES; t++) if (KOROK_TYPES[t].hide == hide) return t; return -1; }
static inline int korok_type_word(const char *w) { for (int t = 0; t < KOROK_NTYPES; t++) if (w && !strcmp(KOROK_TYPES[t].word, w)) return t; return -1; }

/* A puzzle's part: an Area (its role in the puzzle, its shape and scale) or the rock. */
enum { KOROK_PART_AREA, KOROK_PART_ROCK };
enum { KOROK_ROLE_NONE, KOROK_ROLE_START, KOROK_ROLE_GOAL, KOROK_ROLE_LOWER, KOROK_ROLE_UPPER, KOROK_ROLE_FIRST, KOROK_ROLE_WAY, KOROK_ROLE_LAST };
static const char *const KOROK_ROLE_TEXT[] = {"", "start", "goal", "lower", "upper", "first", "on the way", "last"};
typedef struct { float pos[3], scale[3]; uint8_t kind, role, shape; } KorokPart;
/* One korok: hash (its placement Hash, the IsAppearKorok flag's name), pos, hide (HideType), type (KOROK_TYPES index,
   -1 for the kinds a run leaves out), fly (Npc_HiddenKorokFly), sky (a sky island's file), island (on Great Sky Island),
   blocked (ivy, ice, thorns or a push rock on it), time_limit (TimeLimitRing's goal). */
typedef struct {
    uint64_t hash; float pos[3], time_limit; int hide, type, fly, sky, island, blocked, nparts;
    KorokPart part[KOROK_PARTS];
} KorokInfo;

static inline const char *korok_base(const char *path) { const char *s = path ? strrchr(path, '/') : NULL; return s ? s + 1 : path; }
/* The role a sub-group's Logic gives the Areas under it (KOROK_ROLE_NONE: none, look further up). */
static inline int korok_role_of(const char *logic) {
    static const struct { const char *word; int role; } words[] = {{"OnTheWay", KOROK_ROLE_WAY}, {"First", KOROK_ROLE_FIRST}, {"Last", KOROK_ROLE_LAST},
        {"Lower", KOROK_ROLE_LOWER}, {"Upper", KOROK_ROLE_UPPER}, {"Start", KOROK_ROLE_START}, {"Goal", KOROK_ROLE_GOAL}};
    const char *b = korok_base(logic);
    for (size_t i = 0; b && i < sizeof words / sizeof *words; i++) if (strstr(b, words[i].word)) return words[i].role;
    return KOROK_ROLE_NONE;
}
static inline int korok_blocking_logic(const char *logic) {
    const char *b = korok_base(logic);
    return b && (strstr(b, "UnderIvy") || strstr(b, "InIce") || strstr(b, "Thorns") || strstr(b, "PushRock"));
}
/* A BYML uint64 (0xD5) or int64 (0xD4): its value word is the offset of the eight bytes. */
static inline int korok_byml_u64(const LandmarkByml *b, uint32_t node, const char *key, uint64_t *out) {
    uint8_t t; uint32_t v;
    if (!landmark_byml_get(b, node, key, &t, &v) || (t != 0xD5 && t != 0xD4) || (size_t)v + 8 > b->n) return 0;
    *out = (uint64_t)landmark_le32(b->d + v) | (uint64_t)landmark_le32(b->d + v + 4) << 32;
    return 1;
}
static inline int korok_byml_number(const LandmarkByml *b, uint32_t node, const char *key, float *out) {   /* an int or a float */
    uint8_t t; uint32_t v;
    if (!landmark_byml_get(b, node, key, &t, &v)) return 0;
    if (t == 0xD1) { *out = (float)(int32_t)v; return 1; }
    if (t == 0xD2) { memcpy(out, &v, 4); return 1; }
    return 0;
}
typedef struct { uint64_t hash; uint32_t node; } KorokActorRef;
static inline int korok_ref_cmp(const void *a, const void *b) {
    uint64_t x = ((const KorokActorRef *)a)->hash, y = ((const KorokActorRef *)b)->hash; return x < y ? -1 : x > y;
}
/* The folder of a reference's Path ("_0d76/_c28f/Npc_..." -> "_0d76/_c28f"; none: ""), into out. */
static inline void korok_dir(const char *path, char *out, size_t size) {
    const char *s = path ? strrchr(path, '/') : NULL; size_t n = s ? (size_t)(s - path) : 0;
    if (n >= size) n = size - 1;
    memcpy(out, path ? path : "", n); out[n] = 0;
}
/* The koroks of one decompressed map file (.bcett.byml) into out (count so far in *count, at most max); sky: a sky file.
   Returns how many it added, -1 when it is not a map file. */
static inline int korok_scan_file(const uint8_t *d, size_t n, int sky, KorokInfo *out, int *count, int max) {
    if (n < 16 || d[0] != 'Y' || d[1] != 'B') return -1;
    LandmarkByml b = {d, n, landmark_le32(d + 4), landmark_le32(d + 8)};
    uint8_t t; uint32_t actors, groups, av, gv, root = landmark_le32(d + 12);
    if (!landmark_byml_get(&b, root, "Actors", &t, &actors) || t != 0xC0) return -1;
    long na = landmark_byml_array(&b, actors, &av), ng;
    if (!landmark_byml_get(&b, root, "AiGroups", &t, &groups) || t != 0xC0 || (ng = landmark_byml_array(&b, groups, &gv)) <= 0 || na <= 0) return 0;
    KorokActorRef *index = malloc(sizeof *index * (size_t)na); int ni = 0, added = 0;
    if (!index) return 0;
    for (long i = 0; i < na; i++) {
        if (b.d[actors + 4 + i] != 0xC1) continue;
        uint32_t actor = landmark_le32(b.d + av + 4 * i); uint64_t h;
        if (korok_byml_u64(&b, actor, "Hash", &h)) index[ni++] = (KorokActorRef){h, actor};
    }
    qsort(index, (size_t)ni, sizeof *index, korok_ref_cmp);
    for (long g = 0; g < ng && *count < max; g++) {
        if (b.d[groups + 4 + g] != 0xC1) continue;
        uint32_t group = landmark_le32(b.d + gv + 4 * g), refs, rv;
        if (!landmark_byml_get(&b, group, "References", &t, &refs) || t != 0xC0) continue;
        long nr = landmark_byml_array(&b, refs, &rv);
        enum { SUBS = 32 }; struct { char dir[96]; const char *logic; } sub[SUBS]; int nsub = 0;
        uint32_t found[64]; char fpath[64][96]; int nf = 0, korok = -1;
        for (long r = 0; r < nr; r++) {   /* the actors it names, the korok among them, and its sub-groups' folders */
            if (b.d[refs + 4 + r] != 0xC1) continue;
            uint32_t ref = landmark_le32(b.d + rv + 4 * r); uint64_t h;
            const char *path = landmark_byml_text(&b, ref, "Path"), *id = landmark_byml_text(&b, ref, "Id");
            if (id && !strcmp(id, "AiGroup")) {
                if (nsub < SUBS) { korok_dir(path, sub[nsub].dir, sizeof sub[nsub].dir); sub[nsub++].logic = landmark_byml_text(&b, ref, "Logic"); }
                continue;
            }
            KorokActorRef key = {0, 0}, *hit;
            if (!korok_byml_u64(&b, ref, "Reference", &h) || nf >= 64) continue;
            key.hash = h; hit = bsearch(&key, index, (size_t)ni, sizeof *index, korok_ref_cmp);
            if (!hit) continue;
            const char *gyaml = landmark_byml_text(&b, hit->node, "Gyaml");
            if (gyaml && !strncmp(gyaml, "Npc_HiddenKorok", 15) && korok < 0) korok = nf;
            found[nf] = hit->node; korok_dir(path, fpath[nf], sizeof fpath[nf]); nf++;
        }
        if (korok < 0) continue;
        KorokInfo *k = &out[*count]; memset(k, 0, sizeof *k);
        uint32_t kn = found[korok], tr, dyn = 0;
        const char *kg = landmark_byml_text(&b, kn, "Gyaml");
        if (!korok_byml_u64(&b, kn, "Hash", &k->hash) || !landmark_byml_get(&b, kn, "Translate", &t, &tr) || t != 0xC0 || !landmark_byml_vec3(&b, tr, k->pos)) continue;
        float hide = 0;
        if (landmark_byml_get(&b, kn, "Dynamic", &t, &dyn) && t == 0xC1) korok_byml_number(&b, dyn, "HideType", &hide);
        k->hide = (int)hide; k->type = korok_type_of(k->hide); k->fly = kg && strstr(kg, "Fly") != NULL; k->sky = sky;
        k->blocked = korok_blocking_logic(landmark_byml_text(&b, group, "Logic"));
        for (int s = 0; s < nsub; s++) k->blocked |= korok_blocking_logic(sub[s].logic);
        for (int f = 0; f < nf; f++) {
            if (f == korok) continue;
            const char *gyaml = landmark_byml_text(&b, found[f], "Gyaml"); float xyz[3], tl;
            if (!gyaml || !landmark_byml_get(&b, found[f], "Translate", &t, &tr) || t != 0xC0 || !landmark_byml_vec3(&b, tr, xyz)) continue;
            uint32_t pd = 0; int has_dyn = landmark_byml_get(&b, found[f], "Dynamic", &t, &pd) && t == 0xC1;
            if (has_dyn && korok_byml_number(&b, pd, "TimeLimit", &tl) && tl > 0) k->time_limit = tl;
            if (strstr(gyaml, "IceWall") || strstr(gyaml, "Ivy")) { k->blocked = 1; continue; }
            int kind = !strcmp(gyaml, "Area") ? KOROK_PART_AREA : !strncmp(gyaml, "Obj_LiftRock", 12) ? KOROK_PART_ROCK : -1;
            if (kind < 0 || k->nparts >= KOROK_PARTS) continue;
            KorokPart *p = &k->part[k->nparts++]; memcpy(p->pos, xyz, 12); p->kind = (uint8_t)kind; p->scale[0] = p->scale[1] = p->scale[2] = 1;
            uint32_t sc; float shape = 0;
            if (landmark_byml_get(&b, found[f], "Scale", &t, &sc) && t == 0xC0) landmark_byml_vec3(&b, sc, p->scale);
            if (has_dyn) korok_byml_number(&b, pd, "RigidBodyShapeType", &shape);
            p->shape = (uint8_t)shape;
            char dir[96]; snprintf(dir, sizeof dir, "%s", fpath[f]);   /* the nearest sub-group above it that names a role */
            for (;;) {
                for (int s = 0; s < nsub && !p->role; s++) if (!strcmp(sub[s].dir, dir)) p->role = (uint8_t)korok_role_of(sub[s].logic);
                char *slash = strrchr(dir, '/');
                if (p->role || !dir[0]) break;
                if (slash) *slash = 0; else dir[0] = 0;
            }
        }
        (*count)++; added++;
    }
    free(index);
    return added;
}
/* The parts of a korok with that kind and role (role -1: any); returns how many, their indices into at (may be NULL). */
static inline int korok_parts(const KorokInfo *k, int kind, int role, int *at, int max) {
    int n = 0;
    for (int i = 0; i < k->nparts; i++) if (k->part[i].kind == kind && (role < 0 || k->part[i].role == role)) { if (at && n < max) at[n] = i; n++; }
    return n;
}
static inline const KorokPart *korok_part(const KorokInfo *k, int kind, int role) {
    int i; return korok_parts(k, kind, role, &i, 1) ? &k->part[i] : NULL;
}
/* Whether the run has what a korok's kind needs: a plain rock within 0.5 m of the korok (its rock sits right on it, at
   most 0.1 m off in 1.4.3), an Area, the start and goal, the Lower and Upper, the First and Last flower. */
static inline int korok_complete(const KorokInfo *k) {
    switch (k->hide) {
    case KOROK_HIDE_ROCK: { const KorokPart *r = korok_part(k, KOROK_PART_ROCK, -1); return korok_parts(k, KOROK_PART_ROCK, -1, NULL, 0) == 1 && landmark_distance(r->pos, k->pos) < 0.5f; }
    case KOROK_HIDE_LEAF: case KOROK_HIDE_DIVING: case KOROK_HIDE_STICK: return korok_parts(k, KOROK_PART_AREA, -1, NULL, 0) > 0;
    case KOROK_HIDE_RING: case KOROK_HIDE_JUMP: return korok_part(k, KOROK_PART_AREA, KOROK_ROLE_START) && korok_part(k, KOROK_PART_AREA, KOROK_ROLE_GOAL);
    case KOROK_HIDE_CEILING: return korok_part(k, KOROK_PART_AREA, KOROK_ROLE_LOWER) && korok_part(k, KOROK_PART_AREA, KOROK_ROLE_UPPER);
    case KOROK_HIDE_FLOWERS: return korok_part(k, KOROK_PART_AREA, KOROK_ROLE_FIRST) && korok_part(k, KOROK_PART_AREA, KOROK_ROLE_LAST);
    default: return 0;
    }
}

/* Why the run leaves a korok out (KOROK_GO: it does it). Text in KOROK_SKIP_TEXT. */
enum { KOROK_GO, KOROK_SKIP_FOUND, KOROK_SKIP_KIND, KOROK_SKIP_OFF, KOROK_SKIP_BLOCKED, KOROK_SKIP_NO_DATA, KOROK_SKIP_LOCKED,
       KOROK_SKIP_DONE, KOROK_SKIP_FAILED, KOROK_SKIPS };
static const char *const KOROK_SKIP_TEXT[KOROK_SKIPS] = {
    "", "found already", "a kind of puzzle the run does not do yet", "its kind is switched off", "its rock is under ivy, ice, thorns or a push rock",
    "its puzzle's parts are not in the map file", "locked by the story lock (Great Sky Island first)", "found in this run", "failed in this run"};
/* found: the save says found; types_on: KOROK_TYPES bits switched on; locked: the story lock greys it; run: 0, _DONE, _FAILED. */
static inline int korok_skip(const KorokInfo *k, int found, unsigned types_on, int locked, int run) {
    if (found) return KOROK_SKIP_FOUND;
    if (run) return run;
    if (k->type < 0) return KOROK_SKIP_KIND;
    if (!(types_on >> k->type & 1)) return KOROK_SKIP_OFF;
    if (k->hide == KOROK_HIDE_ROCK && k->blocked) return KOROK_SKIP_BLOCKED;
    if (!korok_complete(k)) return KOROK_SKIP_NO_DATA;
    if (locked) return KOROK_SKIP_LOCKED;
    return KOROK_GO;
}
/* The korok to do next: the nearest to from among those with skip[i] == KOROK_GO; -1 when none. */
static inline int korok_next(const KorokInfo *k, int n, const uint8_t *skip, const float from[3]) {
    int best = -1; float bd = 0;
    for (int i = 0; i < n; i++) {
        if (skip[i] != KOROK_GO) continue;
        float d = landmark_distance(k[i].pos, from);
        if (best < 0 || d < bd) { best = i; bd = d; }
    }
    return best;
}
/* The save's IsAppearKorok.<hash> flags (progress.sav, map_pins.h has the layout): found[i] = 1 when korok i is found;
   returns how many of the koroks' flags the save has (0: none or not a save). */
typedef struct { uint32_t hash; int index; } KorokWant;
static inline int korok_want_cmp(const void *a, const void *b) {
    uint32_t x = ((const KorokWant *)a)->hash, y = ((const KorokWant *)b)->hash; return x < y ? -1 : x > y;
}
static inline uint32_t korok_flag_hash(uint64_t hash) {
    char name[48]; snprintf(name, sizeof name, "IsAppearKorok.%llu", (unsigned long long)hash); return dish_murmur3(name);
}
static inline int korok_found_parse(const uint8_t *b, size_t n, const KorokInfo *k, int count, uint8_t *found) {
    memset(found, 0, (size_t)count);
    if (n < 0x30 || landmark_le32(b) != 0x01020304 || count <= 0) return 0;
    KorokWant *want = malloc(sizeof *want * (size_t)count); int got = 0;
    if (!want) return 0;
    for (int i = 0; i < count; i++) want[i] = (KorokWant){korok_flag_hash(k[i].hash), i};
    qsort(want, (size_t)count, sizeof *want, korok_want_cmp);
    for (size_t i = 0x28; i + 8 <= n; i += 8) {
        uint32_t h = landmark_le32(b + i), v = landmark_le32(b + i + 4);
        if (h == 0xa3db7114u) break;
        KorokWant key = {h, 0}, *w = bsearch(&key, want, (size_t)count, sizeof *want, korok_want_cmp);
        if (w) { found[w->index] = v == 1; got++; }
    }
    free(want);
    return got;
}
static inline int korok_found_read(const char *profile, const KorokInfo *k, int count, uint8_t *found) {
    memset(found, 0, (size_t)count);
    char path[1800]; if (!profile) return -1;
    map_pins_newest_save(profile, path, sizeof path);
    FILE *f = path[0] ? fopen(path, "rb") : NULL; if (!f) return -1;
    uint8_t *b = malloc(4 << 20); size_t n = b ? fread(b, 1, 4 << 20, f) : 0; fclose(f);
    int got = b ? korok_found_parse(b, n, k, count, found) : 0;
    free(b);
    return got;
}

/* A korok's steps (korok_plan; world_survey.c carries them out, noclip doing the moving). Every step is one way of
   solving the puzzle or a part of one; the discovery event is watched for all along, and the log names the step (and
   its method) it came in, which tells what the game counts (a body held there by noclip, landed there, walked in):
     FLY    noclip to at (a flight when far: up, across, down; else a straight line), then held there secs while the
            place loads
     HOLD   held at at for secs (reached in a straight line)
     SNAP   put at at at once and held there secs (the CeilingClipper Lower Area's hit lasts only a little while)
     SWEEP  held, moved from at to to at 3 m/s
     LAND   noclip off over at: the soft landing lowers him until he stands; then secs standing
     WALK   walks (the left stick) toward at until within to[0] m, stopped or secs are up
     LIFT   faces at (the camera behind him, a short push of the stick toward it) and holds A secs
     DROP   let go in the air without the soft landing (the falls are not reset for it), secs to fall
     WAIT   secs */
enum { KS_FLY, KS_HOLD, KS_SNAP, KS_SWEEP, KS_LAND, KS_WALK, KS_LIFT, KS_DROP, KS_WAIT, KS_OPS };
static const char *const KOROK_STEP_TEXT[KS_OPS] = {"FLY", "HOLD", "SNAP", "SWEEP", "LAND", "WALK", "LIFT", "DROP", "WAIT"};
enum { KOROK_STEPS = 32 };
typedef struct { int op; float at[3], to[3], secs; const char *method; } KorokStep;
typedef struct { KorokStep s[KOROK_STEPS]; int n; } KorokPlan;
static inline void korok_add(KorokPlan *p, int op, const float at[3], float dy, float secs, const char *method) {
    if (p->n >= KOROK_STEPS) return;
    KorokStep *s = &p->s[p->n++]; memset(s, 0, sizeof *s);
    s->op = op; s->secs = secs; s->method = method;
    if (at) { memcpy(s->at, at, 12); s->at[1] += dy; }
}
/* The steps for korok k, Link coming from from. Heights: Link's position is his feet; an Area's Translate is its centre:
   a flower's sphere sits 0.8 m under the flower (so he stands 0.8 m above it), the flat cylinders on blocks and stumps
   at the block's top. */
static inline void korok_plan(const KorokInfo *k, const float from[3], KorokPlan *p) {
    memset(p, 0, sizeof *p);
    const KorokPart *a = korok_part(k, KOROK_PART_AREA, -1);
    switch (k->hide) {
    case KOROK_HIDE_ROCK: {   /* stand 1.6 m off the rock on the side he comes from (then the other side), walk up, lift */
        const KorokPart *r = korok_part(k, KOROK_PART_ROCK, -1); if (!r) return;
        float dir[2] = {from[0] - r->pos[0], from[2] - r->pos[2]}, l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]), side[2][3];
        if (l > 0.01f) { dir[0] /= l; dir[1] /= l; } else { dir[0] = 0; dir[1] = 1; }
        for (int s = 0; s < 2; s++) { float m = s ? -1.6f : 1.6f; side[s][0] = r->pos[0] + m * dir[0]; side[s][1] = r->pos[1]; side[s][2] = r->pos[2] + m * dir[1]; }
        for (int s = 0; s < 2; s++) {
            korok_add(p, KS_FLY, side[s], 2.5f, s ? 1.0f : 2.0f, s ? "to the rock's other side" : "to beside the rock");
            korok_add(p, KS_LAND, side[s], 0.5f, 1.0f, "set down beside the rock");
            korok_add(p, KS_WALK, r->pos, 0, 4, "walked up to the rock"); p->s[p->n - 1].to[0] = 1.1f;
            korok_add(p, KS_LIFT, r->pos, 0, 1.5f, s ? "A held 1.5 s, from the other side" : "A held 1.5 s");
            korok_add(p, KS_WAIT, NULL, 0, 4, "waiting after the lift");
            if (!s) { korok_add(p, KS_LIFT, r->pos, 0, 0.2f, "A tapped"); korok_add(p, KS_WAIT, NULL, 0, 3, "waiting after the tap"); }
        }
        break;
    }
    case KOROK_HIDE_LEAF: {   /* the sparkle: his body on it, then inside its Area, then a slow pass from one to the other, then landed on it.
        The Area is mostly a sphere as big as its Scale right under the korok, but 15 of 73 sit up to 16 m to one side: the
        point inside it is 0.4 of its Scale from its centre toward the korok, inside whether the Scale is the radius or the
        diameter, and never the centre itself, which can be in the ground */
        float s = a ? a->scale[0] : 2, in[3]; memcpy(in, k->pos, 12);
        if (a) {
            float d = landmark_distance(k->pos, a->pos), f = d > 0.01f ? fminf(1, 0.4f * s / d) : 0;
            for (int c = 0; c < 3; c++) in[c] = a->pos[c] + (k->pos[c] - a->pos[c]) * f;
        }
        korok_add(p, KS_FLY, k->pos, 3, 2, "above the sparkle");
        korok_add(p, KS_HOLD, k->pos, -0.9f, 2, "held with his body on the sparkle");
        if (landmark_distance(in, k->pos) > 0.5f) korok_add(p, KS_HOLD, in, -0.9f, 2, "held with his body inside the sparkle's Area");
        korok_add(p, KS_SWEEP, k->pos, 1, 0, "moved slowly from above the sparkle into its Area"); memcpy(p->s[p->n - 1].to, in, 12); p->s[p->n - 1].to[1] -= 0.9f;
        korok_add(p, KS_LAND, k->pos, 0.5f, 2, "landed on the sparkle");
        korok_add(p, KS_WAIT, NULL, 0, 3, "waiting on the sparkle");
        break;
    }
    case KOROK_HIDE_DIVING:   /* a dive into the Area from 25 m up; else held in it */
        if (!a) return;
        korok_add(p, KS_FLY, a->pos, 25, 2.5f, "25 m above the dive's Area");
        korok_add(p, KS_DROP, NULL, 0, 10, "fell from 25 m into the water (a dive)");
        korok_add(p, KS_WAIT, NULL, 0, 3, "waiting in the water");
        korok_add(p, KS_FLY, a->pos, 3, 1, "back above the Area");
        korok_add(p, KS_HOLD, a->pos, 0, 2, "held in the Area (no dive)");
        break;
    case KOROK_HIDE_FLOWERS: {   /* First, then the nearest flower left each time, then the Last; twice */
        int at[KOROK_PARTS], way = korok_parts(k, KOROK_PART_AREA, KOROK_ROLE_WAY, at, KOROK_PARTS), order[KOROK_PARTS], n = 0, used[KOROK_PARTS] = {0};
        const KorokPart *first = korok_part(k, KOROK_PART_AREA, KOROK_ROLE_FIRST), *last = korok_part(k, KOROK_PART_AREA, KOROK_ROLE_LAST);
        if (!first || !last) return;
        if (way > KOROK_PARTS) way = KOROK_PARTS;
        const float *cur = first->pos;
        for (int i = 0; i < way; i++) {
            int best = -1; float bd = 0;
            for (int j = 0; j < way; j++) { if (used[j]) continue; float d = landmark_distance(k->part[at[j]].pos, cur); if (best < 0 || d < bd) { best = j; bd = d; } }
            used[best] = 1; order[n++] = at[best]; cur = k->part[at[best]].pos;
        }
        korok_add(p, KS_FLY, first->pos, 3, 2, "above the first flower");
        for (int pass = 0; pass < 2; pass++) {
            korok_add(p, KS_HOLD, first->pos, 0.8f, 1.2f, pass ? "second pass: the first flower" : "held at the first flower");
            for (int i = 0; i < n; i++) korok_add(p, KS_HOLD, k->part[order[i]].pos, 0.8f, 1.2f, pass ? "second pass: a flower on the way" : "held at a flower on the way");
            korok_add(p, KS_HOLD, last->pos, 0.8f, 1.5f, pass ? "second pass: the last flower" : "held at the last flower");
            if (!pass) korok_add(p, KS_LAND, last->pos, 1.2f, 3, "landed at the last flower");
        }
        korok_add(p, KS_WAIT, NULL, 0, 3, "waiting after the flowers");
        break;
    }
    case KOROK_HIDE_RING: {   /* the stump (held, then stood on), each time straight on to the goal ring */
        const KorokPart *st = korok_part(k, KOROK_PART_AREA, KOROK_ROLE_START), *goal = korok_part(k, KOROK_PART_AREA, KOROK_ROLE_GOAL);
        if (!st || !goal) return;
        korok_add(p, KS_FLY, st->pos, 3, 2, "above the start stump");
        korok_add(p, KS_HOLD, st->pos, 0.2f, 1.5f, "held on the start stump");
        korok_add(p, KS_FLY, goal->pos, 0, 0, "to the goal ring");
        korok_add(p, KS_HOLD, goal->pos, 0, 2, "held in the goal ring");
        korok_add(p, KS_FLY, st->pos, 3, 1, "back above the start stump");
        korok_add(p, KS_LAND, st->pos, 0.5f, 1.5f, "stood on the start stump");
        korok_add(p, KS_FLY, goal->pos, 0, 0, "to the goal ring after standing on the stump");
        korok_add(p, KS_HOLD, goal->pos, 0, 2, "held in the goal ring after standing on the stump");
        korok_add(p, KS_WAIT, NULL, 0, 3, "waiting at the goal ring");
        break;
    }
    case KOROK_HIDE_JUMP: {   /* Grounded on both blocks: landed on the start block, flown across, landed on the goal block */
        const KorokPart *st = korok_part(k, KOROK_PART_AREA, KOROK_ROLE_START), *goal = korok_part(k, KOROK_PART_AREA, KOROK_ROLE_GOAL);
        if (!st || !goal) return;
        korok_add(p, KS_FLY, st->pos, 3, 2, "above the start block");
        korok_add(p, KS_LAND, st->pos, 0.5f, 1.5f, "stood on the start block");
        korok_add(p, KS_FLY, goal->pos, 2, 0.3f, "across to above the goal block");
        korok_add(p, KS_LAND, goal->pos, 0.5f, 2, "landed on the goal block");
        korok_add(p, KS_WAIT, NULL, 0, 3, "waiting on the goal block");
        break;
    }
    case KOROK_HIDE_STICK:
        if (!a) return;
        korok_add(p, KS_FLY, a->pos, 3, 2, "above the target block");
        korok_add(p, KS_HOLD, a->pos, 0.1f, 2, "held in the target's Area");
        korok_add(p, KS_LAND, a->pos, 1, 2, "landed on the target block");
        korok_add(p, KS_WAIT, NULL, 0, 3, "waiting on the target block");
        break;
    case KOROK_HIDE_CEILING: {   /* the Lower Area, then at once the Upper one; then stood below and landed on top */
        const KorokPart *lo = korok_part(k, KOROK_PART_AREA, KOROK_ROLE_LOWER), *up = korok_part(k, KOROK_PART_AREA, KOROK_ROLE_UPPER);
        if (!lo || !up) return;
        korok_add(p, KS_FLY, lo->pos, 1, 2, "under the ceiling, in the Lower Area");
        korok_add(p, KS_HOLD, lo->pos, 1, 1.5f, "held in the Lower Area");
        korok_add(p, KS_SNAP, up->pos, 0.3f, 2, "put straight from the Lower into the Upper Area");
        korok_add(p, KS_FLY, lo->pos, 1, 0.5f, "back down into the Lower Area");
        korok_add(p, KS_LAND, lo->pos, 1, 1.5f, "stood in the Lower Area");
        korok_add(p, KS_SNAP, up->pos, 0.3f, 0.3f, "then put in the Upper Area");
        korok_add(p, KS_LAND, up->pos, 0.3f, 2, "then landed on the ceiling's top (Upper Area)");
        korok_add(p, KS_WAIT, NULL, 0, 3, "waiting on the ceiling");
        break;
    }
    default: break;
    }
}
/* Where the puzzle is, for the nearest-first choice and the first flight: the first step's place (else the korok). */
static inline void korok_where(const KorokInfo *k, const KorokPlan *p, float out[3]) { memcpy(out, p->n ? p->s[0].at : k->pos, 12); }

/* The RomFS's koroks (the surface's and the sky's static map files; not the caves, the Depths' holes, the castle and the
   temples) and which are on Great Sky Island. Returns how many (800 in 1.4.3), 0 when this is not the game. A few
   hundred files, as shrines_load. */
static inline int koroks_load(const LandmarkRomfs *r, KorokInfo *out, int max) {
    int count = 0;
    for (size_t i = 0, files = r->count(); i < files; i++) {
        const char *p = r->name(i); if (!p) continue;
        if (strncmp(p, "Banc/MainField/", 15) || !landmark_ends(p, "_Static.bcett.byml.zs")) continue;
        const char *rest = p + 15, *slash = strchr(rest, '/'); int sky = !strncmp(rest, "Sky/", 4);
        if (slash && !sky) continue;   /* Cave/, DeepHole/, LargeDungeon/, Castle/ */
        size_t n; uint8_t *d = landmark_read(r, p, &n);
        if (d) korok_scan_file(d, n, sky, out, &count, max);
        free(d);
    }
    size_t sn = 0; float island[1][3];
    uint8_t *surface = landmark_read(r, "Banc/MainField/LocationArea/MainField.locationarea.byml.zs", &sn);
    if (surface && landmark_markers(surface, sn, "BeginningIsland", island, 1))
        for (int i = 0; i < count; i++) out[i].island = landmark_on_island(island[0], out[i].pos);
    free(surface);
    return count;
}
