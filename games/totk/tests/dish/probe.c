/* Unit test for runtime/dish_give.h and the Dishes parts of runtime/spawn_list.h: the GameData name hash against values
   the game and the save editors use, request limits, the change-queue room check, and the actor list's dishes and
   @effect lines (a menu that cannot give dishes leaves them out). Also map_pins.h and landmarks.h (towers, shrines and
   the story's places: a small map file, place list, message file and save made here; the story flags and the lock), and
   shrine_auto.h (a shrine's outside and inside map files, its kind, the save's DungeonState, which shrines a run does and
   in which order, where Link stands and walks), and korok_auto.h (koroks' puzzles from a map file's AiGroups, the save's
   IsAppearKorok flags, which koroks a run does and in which order, each kind's steps). Prints one JSON line; exit 1 on failure. */
#include "dish_give.h"
#include "spawn_list.h"
#include "map_pins.h"
#include "landmarks.h"
#include "shrine_auto.h"
#include "korok_auto.h"
#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* A little-endian BYML / MSBT / SARC made here, as the game's files are laid out. */
static uint8_t buf[0x4000]; static size_t at;
static uint32_t put(const void *p, size_t n) { uint32_t o = (uint32_t)at; memcpy(buf + at, p, n); at += n; return o; }
static uint32_t put32(uint32_t v) { return put(&v, 4); }
static void align(size_t a) { while (at % a) buf[at++] = 0; }
static uint32_t byml_table(const char *const *s, int n) {   /* a string table: 0xC2, count, offsets, strings */
    align(4); uint32_t t = put32(0xC2 | (uint32_t)n << 8), offs = (uint32_t)at; at += 4u * n + 4;
    for (int i = 0; i < n; i++) { uint32_t o = (uint32_t)at - t; memcpy(buf + offs + 4 * i, &o, 4); put(s[i], strlen(s[i]) + 1); }
    return t;
}
static uint32_t byml_dict(int n, const uint32_t *key, const uint8_t *type, const uint32_t *value) {
    align(4); uint32_t d = put32(0xC1 | (uint32_t)n << 8);
    for (int i = 0; i < n; i++) { put32(key[i] | (uint32_t)type[i] << 24); put32(value[i]); }
    return d;
}
static uint32_t byml_array(int n, const uint8_t *type, const uint32_t *value) {
    align(4); uint32_t a = put32(0xC0 | (uint32_t)n << 8); put(type, n); align(4);
    for (int i = 0; i < n; i++) put32(value[i]);
    return a;
}
static uint32_t f32(float f) { uint32_t w; memcpy(&w, &f, 4); return w; }
/* A map file of actors for shrine_auto.h: keys and strings below; each actor a Gyaml, a Translate, and optionally a Name,
   a Rotate and up to two Dynamic entries (a string or a bool). */
enum { K_ACTORS, K_DYNAMIC, K_INDEX, K_GYAML, K_TRANSLATE, K_NAME, K_EVENT, K_CRYSTAL, K_HIDDEN, K_ROTATE, K_TYPE, NKEYS };
static const char *const shrine_keys[NKEYS] = {"Actors", "Dynamic", "DungeonIndexStr", "Gyaml", "Translate", "Name", "RequestEventName",
                                               "IsKeyCrystalAuthSmallDungeon", "IsHiddenSmallDungeon", "Rotate", "Type"};
enum { S_ENTRANCE, S_LIFT, S_ANCHOR, S_WARP, S_D7, S_GO, S_ENTER_POS, S_START, S_ON_ELEVATOR, S_GOAL, S_GOAL_EXIT, S_EXIT_POS, S_TBOX, S_RETURN, S_LEAVE_POS, S_ENEMY, S_D200, S_AUTH, NSTRS };
static const char *const shrine_strs[NSTRS] = {"DgnObj_ZonauShrineEntrance_A_01", "FldObj_EnterAttetionActor", "DestinationAnchor", "DgnObj_WarpPoint_Zonau_A_01",
                                               "Dungeon007", "DmF_SY_SmallDungeon2Go", "EnterSmallDungeonPos", "StartPos", "OnElevator", "DgnObj_Hrl_GoalSystem_B_01",
                                               "DgnObj_GoalExit", "SmallDungeonExit", "TBox_Dungeon_Stone", "DmF_SY_SmallDungeon4Return", "LeaveSmallDungeonPos", "Enemy", "Dungeon200",
                                               "AuthenticationPos"};
typedef struct { int gyaml; float pos[3]; int name, ndyn, rotate; int dkey[2]; uint8_t dtype[2]; uint32_t dval[2]; float rot[3]; } TestActor;
static uint32_t vec3(const float *v) { const uint8_t ft[3] = {0xD2, 0xD2, 0xD2}; const uint32_t fv[3] = {f32(v[0]), f32(v[1]), f32(v[2])}; return byml_array(3, ft, fv); }
static size_t shrine_map(const TestActor *a, int n) {
    at = 16; memset(buf, 0, sizeof buf); memcpy(buf, "YB\x07\0", 4);
    uint32_t kt = byml_table(shrine_keys, NKEYS), st = byml_table(shrine_strs, NSTRS), actor[16];
    for (int i = 0; i < n; i++) {
        uint32_t k[5], v[5]; uint8_t t[5]; int m = 0;
        k[m] = K_GYAML; t[m] = 0xA0; v[m++] = (uint32_t)a[i].gyaml;
        k[m] = K_TRANSLATE; t[m] = 0xC0; v[m++] = vec3(a[i].pos);
        if (a[i].name >= 0) { k[m] = K_NAME; t[m] = 0xA0; v[m++] = (uint32_t)a[i].name; }
        if (a[i].rotate) { k[m] = K_ROTATE; t[m] = 0xC0; v[m++] = vec3(a[i].rot); }
        if (a[i].ndyn) {
            uint32_t dk[2] = {(uint32_t)a[i].dkey[0], (uint32_t)a[i].dkey[1]};
            k[m] = K_DYNAMIC; t[m] = 0xC1; v[m++] = byml_dict(a[i].ndyn, dk, a[i].dtype, a[i].dval);
        }
        actor[i] = byml_dict(m, k, t, v);
    }
    uint8_t types[16]; memset(types, 0xC1, sizeof types);
    uint32_t list = byml_array(n, types, actor);
    const uint32_t rk[] = {K_ACTORS}, rv[] = {list}; const uint8_t rt[] = {0xC0};
    uint32_t root = byml_dict(1, rk, rt, rv);
    memcpy(buf + 4, &kt, 4); memcpy(buf + 8, &st, 4); memcpy(buf + 12, &root, 4);
    return at;
}

/* A map file of koroks' puzzles for korok_auto.h: Actors (a Hash, a Gyaml, a Translate, a Scale and one Dynamic entry
   when given) and AiGroups (a Logic, References to actors by Hash or sub-groups "<dir>/AiGroup" with their own Logic). */
enum { KK_ACTORS, KK_GROUPS, KK_HASH, KK_GYAML, KK_TRANSLATE, KK_SCALE, KK_DYNAMIC, KK_HIDE, KK_SHAPE, KK_TIME, KK_LOGIC, KK_REFS, KK_ID, KK_PATH, KK_REF, NKK };
static const char *const korok_keys[NKK] = {"Actors", "AiGroups", "Hash", "Gyaml", "Translate", "Scale", "Dynamic", "HideType", "RigidBodyShapeType",
                                            "TimeLimit", "Logic", "References", "Id", "Path", "Reference"};
enum { KT_GROUND, KT_FLY, KT_ROCK, KT_AREA, KT_GOALOBJ, KT_ICE, KT_AIGROUP, KT_PART, KT_L_ROCK, KT_L_BASE, KT_L_RING, KT_L_GOAL, KT_L_START, KT_L_ICE,
       KT_L_CEIL, KT_L_LOWER, KT_L_UPPER, KT_L_OTHER, KT_P_ROCK, KT_P_KOROK_A, KT_P_BASE_A, KT_P_KOROK_B, KT_P_BASE_B, KT_P_GOALOBJ, KT_P_GOAL, KT_P_GOALGROUP,
       KT_P_START, KT_P_STARTGROUP, KT_P_ICEROCK, KT_P_KOROK_C, KT_P_ICE, KT_P_ICEGROUP, KT_P_LOWER, KT_P_LOWERGROUP, KT_P_KOROK_D, KT_P_UPPER, KT_P_UPPERGROUP, KT_P_OTHER, NKT };
static const char *const korok_strs[NKT] = {"Npc_HiddenKorokGround", "Npc_HiddenKorokFly", "Obj_LiftRockWhite_Korok_A_01", "Area", "FldObj_KorokGoal_A_01", "IceWall",
    "AiGroup", "Part", "Logic/Module/HiddenKorok_LiftUpRock/HiddenKorok_LiftUpRock_2a3e.logic.module.ain", "Logic/Module/HiddenKorok_Base/HiddenKorok_Base_1e88.logic.module.ain",
    "Logic/Module/HiddenKorok_TimeLimitRing/HiddenKorok_TimeLimitRing_3cf9.logic.module.ain", "Logic/Module/HiddenKorok_TimeLimitRingGoal/HiddenKorok_TimeLimitRingGoal_f0f9.logic.module.ain",
    "Logic/Module/HiddenKorok_TimeLimitRingStart/HiddenKorok_TimeLimitRingStart_c312.logic.module.ain", "Logic/Module/HiddenKorok_LiftUpRockInIce/HiddenKorok_LiftUpRockInIce_21bf.logic.module.ain",
    "Logic/Module/HiddenKorok_CeilingClipper/HiddenKorok_CeilingClipper_795d.logic.module.ain", "Logic/Module/HiddenKorok_CeilingClipper_Lower/HiddenKorok_CeilingClipper_Lower_dd75.logic.module.ain",
    "Logic/Module/HiddenKorok_CeilingClipper_Upper_Ground/HiddenKorok_CeilingClipper_Upper_Ground_afbb.logic.module.ain", "Logic/Module/Other/Other_0000.logic.module.ain",
    "Obj_LiftRockWhite_A_01_e058", "_c838/Npc_HiddenKorokGround_5be7", "_c838/AiGroup", "_0d76/_c28f/Npc_HiddenKorokGround_5be7", "_0d76/_c28f/AiGroup",
    "_0d76/FldObj_KorokGoal_A_01_2bb6", "_0d76/Area_315c", "_0d76/AiGroup", "_3faa/Area_c710", "_3faa/AiGroup", "_7aab/Obj_LiftRockWhite_A_01_e058",
    "_7aab/_f8dd/Npc_HiddenKorokGround_5be7", "IceWall_e4c9", "_7aab/AiGroup", "_39e5/_076e/Area_5d81", "_39e5/_076e/AiGroup", "_39e5/_00bd/_22d0/Npc_HiddenKorokGround_5be7",
    "_5342/_70a3/Area_ff32", "_5342/AiGroup", "Area_0000"};
static uint32_t put64(uint64_t v) { align(8); return put(&v, 8); }
typedef struct { uint64_t hash; int gyaml; float pos[3], scale[3]; int dkey; uint8_t dtype; uint32_t dval; } KorokTestActor;
typedef struct { int id, path; uint64_t ref; int logic; } KorokTestRef;   /* ref 0: a sub-group with logic */
static size_t korok_test_map(void) {
    static const KorokTestActor a[] = {
        {1001, KT_GROUND, {10, 100, 10}, {0}, -1, 0, 0}, {2001, KT_ROCK, {10, 100.1f, 10}, {0}, -1, 0, 0},
        {1002, KT_FLY, {50, 100, 50}, {0}, KK_HIDE, 0xD1, 4}, {2002, KT_AREA, {52, 100, 50}, {1.7f, 1.7f, 1.7f}, KK_SHAPE, 0xD1, 0},
        {2003, KT_GOALOBJ, {52, 99.8f, 50}, {0}, KK_TIME, 0xD2, 0x42f00000 /* 120.0f */}, {2004, KT_AREA, {80, 110, 50}, {0.7f, 0.7f, 0.7f}, KK_SHAPE, 0xD1, 3},
        {1003, KT_GROUND, {200, 100, 200}, {0}, -1, 0, 0}, {2005, KT_ROCK, {200, 100.1f, 200}, {0}, -1, 0, 0}, {2006, KT_ICE, {200, 100, 200}, {0}, -1, 0, 0},
        {1004, KT_FLY, {300, 150, 300}, {0}, KK_HIDE, 0xD1, 23}, {2007, KT_AREA, {300, 124, 300}, {0.9f, 15, 0.9f}, KK_SHAPE, 0xD1, 3},
        {2008, KT_AREA, {300, 157, 300}, {0}, -1, 0, 0}, {2009, KT_AREA, {0, 0, 0}, {0}, -1, 0, 0}};
    static const KorokTestRef g0[] = {{KT_PART, KT_P_ROCK, 2001, 0}, {KT_PART, KT_P_KOROK_A, 1001, 0}, {KT_AIGROUP, KT_P_BASE_A, 0, KT_L_BASE}},
        g1[] = {{KT_PART, KT_P_KOROK_B, 1002, 0}, {KT_AIGROUP, KT_P_BASE_B, 0, KT_L_BASE}, {KT_PART, KT_P_GOALOBJ, 2003, 0}, {KT_PART, KT_P_GOAL, 2002, 0},
                {KT_AIGROUP, KT_P_GOALGROUP, 0, KT_L_GOAL}, {KT_PART, KT_P_START, 2004, 0}, {KT_AIGROUP, KT_P_STARTGROUP, 0, KT_L_START}},
        g2[] = {{KT_PART, KT_P_ICEROCK, 2005, 0}, {KT_PART, KT_P_KOROK_C, 1003, 0}, {KT_PART, KT_P_ICE, 2006, 0}, {KT_AIGROUP, KT_P_ICEGROUP, 0, KT_L_ROCK}},
        g3[] = {{KT_PART, KT_P_LOWER, 2007, 0}, {KT_AIGROUP, KT_P_LOWERGROUP, 0, KT_L_LOWER}, {KT_PART, KT_P_KOROK_D, 1004, 0}, {KT_PART, KT_P_UPPER, 2008, 0},
                {KT_AIGROUP, KT_P_UPPERGROUP, 0, KT_L_UPPER}},
        g4[] = {{KT_PART, KT_P_OTHER, 2009, 0}};
    static const struct { int logic, n; const KorokTestRef *r; } g[] = {{KT_L_ROCK, 3, g0}, {KT_L_RING, 7, g1}, {KT_L_ICE, 4, g2}, {KT_L_CEIL, 5, g3}, {KT_L_OTHER, 1, g4}};
    enum { NA = sizeof a / sizeof *a, NG = sizeof g / sizeof *g };
    at = 16; memset(buf, 0, sizeof buf); memcpy(buf, "YB\x07\0", 4);
    uint32_t kt = byml_table(korok_keys, NKK), st = byml_table(korok_strs, NKT), actor[NA], group[NG];
    for (int i = 0; i < NA; i++) {
        uint32_t k[5], v[5]; uint8_t t[5]; int m = 0;
        k[m] = KK_HASH; t[m] = 0xD5; v[m++] = put64(a[i].hash);
        k[m] = KK_GYAML; t[m] = 0xA0; v[m++] = (uint32_t)a[i].gyaml;
        k[m] = KK_TRANSLATE; t[m] = 0xC0; v[m++] = vec3(a[i].pos);
        if (a[i].scale[0]) { k[m] = KK_SCALE; t[m] = 0xC0; v[m++] = vec3(a[i].scale); }
        if (a[i].dkey >= 0) { uint32_t dk = (uint32_t)a[i].dkey; k[m] = KK_DYNAMIC; t[m] = 0xC1; v[m++] = byml_dict(1, &dk, &a[i].dtype, &a[i].dval); }
        actor[i] = byml_dict(m, k, t, v);
    }
    for (int i = 0; i < NG; i++) {
        uint32_t ref[8];
        for (int r = 0; r < g[i].n; r++) {
            const KorokTestRef *x = &g[i].r[r];
            uint32_t k[3] = {KK_ID, KK_PATH, x->ref ? KK_REF : KK_LOGIC}, v[3] = {(uint32_t)x->id, (uint32_t)x->path, x->ref ? put64(x->ref) : (uint32_t)x->logic};
            uint8_t t[3] = {0xA0, 0xA0, x->ref ? 0xD5 : 0xA0};
            ref[r] = byml_dict(3, k, t, v);
        }
        uint8_t types[8]; memset(types, 0xC1, sizeof types);
        uint32_t list = byml_array(g[i].n, types, ref), k[2] = {KK_LOGIC, KK_REFS}, v[2] = {(uint32_t)g[i].logic, list}; uint8_t t[2] = {0xA0, 0xC0};
        group[i] = byml_dict(2, k, t, v);
    }
    uint8_t types[16]; memset(types, 0xC1, sizeof types);
    uint32_t actors = byml_array(NA, types, actor), groups = byml_array(NG, types, group);
    const uint32_t rk[] = {KK_ACTORS, KK_GROUPS}, rv[] = {actors, groups}; const uint8_t rt[] = {0xC0, 0xC0};
    uint32_t root = byml_dict(2, rk, rt, rv);
    memcpy(buf + 4, &kt, 4); memcpy(buf + 8, &st, 4); memcpy(buf + 12, &root, 4);
    return at;
}

int main(void) {
    static const struct { const char *name; uint32_t hash; } known[] = {
        {"Content", 0x4290322e}, {"Name", 0x25efa387}, {"LifeRecover", 0x515632a9}, {"Effect", 0xecff4f2a},
        {"None", 0xb6eede09}, {"AttackUp", 0xa9384c6c}, {"DefenseUp", 0xa0a00c0e}, {"LifeMaxUp", 0xc1db0965},
        {"StaminaRecover", 0xe9a30056},
    };
    for (size_t i = 0; i < sizeof known / sizeof *known; i++)
        CHECK(dish_murmur3(known[i].name) == known[i].hash, "murmur3(%s) = %08x, want %08x", known[i].name, dish_murmur3(known[i].name), known[i].hash);

    DishRequest r;
    CHECK(dish_request_make(&r, "Item_Cook_A_01", 20, "AttackUp", 3, 300, 30, 2) == SPAWN_OK, "a valid request");
    CHECK(r.effect == dish_effect_find("AttackUp") && r.level == 3 && r.time == 300 && r.count == 2, "request fields");
    CHECK(dish_request_make(&r, "Item_Cook_A_01", 20, NULL, 3, 300, 30, 1) == SPAWN_OK && r.level == 0 && r.time == 0, "no effect: level and time 0");
    CHECK(dish_request_make(&r, "Item_Roast_01", 20, NULL, 0, 0, 1, 1) == DISH_ERR_NOT_DISH, "not a dish");
    CHECK(dish_request_make(&r, "Item_Cook_A_01", 20, "TwiceJump", 1, 60, 1, 1) == DISH_ERR_EFFECT, "unknown effect");
    CHECK(dish_request_make(&r, "Item_Cook_A_01", DISH_LIFE_MAX + 1, NULL, 0, 0, 1, 1) == DISH_ERR_VALUE, "too many hearts");
    CHECK(dish_request_make(&r, "Item_Cook_A_01", 20, "AttackUp", 0, 60, 1, 1) == DISH_ERR_VALUE, "an effect needs a level");
    CHECK(dish_request_make(&r, "Item_Cook_A_01", 20, NULL, 0, 0, 1, 0) == SPAWN_ERR_COUNT, "count 0");

    DishQueue q = {0};
    for (int i = 0; i < DISH_QUEUE_MAX; i++) CHECK(dish_queue_push(&q, &r) == SPAWN_OK, "push %d", i);
    CHECK(dish_queue_push(&q, &r) == SPAWN_ERR_FULL, "queue full");
    dish_queue_drop(&q); CHECK(q.len == DISH_QUEUE_MAX - 1, "drop");

    CHECK(dish_store_has_room((DishStoreView){64, 0x1000, 10}, 5), "room");
    CHECK(!dish_store_has_room((DishStoreView){64, 0x1000, 60}, 5), "no room");
    CHECK(!dish_store_has_room((DishStoreView){64, 0, 0}, 1), "no buffer");
    CHECK(dish_store_has_room((DishStoreView){64, 0x1000, 0x100000 | 10}, 5), "upper bits of the control word ignored");

    const char *text = "# test\n"
                       "Item_Apple\titems\tpickup\tApple\tgame\n"
                       "Item_Cook_A_01\tdishes\tpouch\tMushroom Skewer\tgame\n"
                       "Item_Cook_Z_99\tdishes\tpickup\tWrong\tgame\n"            /* a dish must be pouch */
                       "Item_Roast_01\titems\tpouch\tWrong\tgame\n"               /* pouch only for dishes */
                       "@effect\tAttackUp\t1\t3\t20\n"
                       "@effect\tLifeMaxUp\t4\t108\t0\n"
                       "@effect\tBad\t3\t1\t0\n";                                /* max below min: skipped */
    SpawnList l;
    CHECK(spawn_list_parse(&l, text, strlen(text)) == 2, "two actors, got %d", l.count);
    CHECK(l.neffect == 2, "two effects, got %d", l.neffect);
    const SpawnEffect *e = spawn_list_effect(&l, "AttackUp");
    CHECK(e && e->min_level == 1 && e->max_level == 3 && e->base_time == 20, "AttackUp from the table");
    CHECK(spawn_list_has_dishes(&l), "has dishes");
    int out[8];
    CHECK(spawn_list_filter(&l, NULL, SPAWN_CAT_DISHES, "", 0, out, 8) == 1 && spawn_actor_is_dish(&l.actor[out[0]]), "Dishes category");
    CHECK(spawn_list_filter_dishes(&l, NULL, SPAWN_CAT_DISHES, "", 0, 0, out, 8) == 0, "dishes hidden without giving");
    CHECK(spawn_list_filter_dishes(&l, NULL, SPAWN_CAT_ITEMS, "skewer", 0, 0, out, 8) == 0, "search leaves dishes out without giving");
    CHECK(spawn_list_filter(&l, NULL, SPAWN_CAT_ITEMS, "skewer", 0, out, 8) == 1, "search finds the dish");
    spawn_list_free(&l);

    /* a save with two pin slots, the first red, the second unused (Invalid) */
    static uint8_t sav[0x200];
    uint32_t w[] = {0x01020304}; memcpy(sav, w, 4);
    uint32_t table[] = {0x5f93e70f, 0x100, 0x1b62aa42, 0x140, 0xa3db7114, 0};
    memcpy(sav + 0x28, table, sizeof table);
    uint32_t two = 2; float xyz[6] = {1.5f, 200.25f, -30.0f, 9, 9, 9};
    memcpy(sav + 0x100, &two, 4); memcpy(sav + 0x104, xyz, sizeof xyz);
    uint32_t types[3] = {2, dish_murmur3("Red"), dish_murmur3("Invalid")}; memcpy(sav + 0x140, types, sizeof types);
    MapPin pins[4];
    CHECK(map_pins_parse(sav, sizeof sav, pins, 4) == 1 && !strcmp(pins[0].colour, "Red") && pins[0].xyz[1] == 200.25f, "one red pin");
    sav[0] = 0; CHECK(map_pins_parse(sav, sizeof sav, pins, 4) == 0, "not a save");

    /* landmarks.h: a map file with one tower, its door 5 m out along x, one shrine entrance with its travel point, a
       temple's travel point and a Lightroot (not a travel point to land at) */
    static const char *const keys[] = {"Actors", "Dynamic", "DungeonIndexStr", "Gyaml", "TowerNo", "Translate"};
    static const char *const strs[] = {"DgnObj_WarpPoint_Zonau_A_01", "DgnObj_ZonauShrineEntrance_A_01", "Dungeon007", "FldObj_CannonDoor_A_01",
                                       "FldObj_Cannon_A_01", "Tower03", "DgnObj_WarpPoint_Zonau_A_02", "DgnObj_WarpPoint_Zonau_A_04"};
    at = 16; memcpy(buf, "YB\x07\0", 4);
    uint32_t kt = byml_table(keys, 6), st = byml_table(strs, 8), actor[6];
    const float pos[6][3] = {{100, 50, 200}, {105, 50.5f, 200}, {-10, 300, 40}, {-11.5f, 300.5f, 40}, {500, 1600, 500}, {510, 1600, 500}};
    const uint32_t gy[6] = {4, 3, 1, 0, 6, 7};
    for (int i = 0; i < 6; i++) {
        const uint8_t ft[3] = {0xD2, 0xD2, 0xD2}; const uint32_t fv[3] = {f32(pos[i][0]), f32(pos[i][1]), f32(pos[i][2])};
        uint32_t tr = byml_array(3, ft, fv), dyn = 0;
        if (i == 0) { const uint32_t k[] = {4}, v[] = {5}; const uint8_t t[] = {0xA0}; dyn = byml_dict(1, k, t, v); }
        if (i == 2) { const uint32_t k[] = {2}, v[] = {2}; const uint8_t t[] = {0xA0}; dyn = byml_dict(1, k, t, v); }
        const uint32_t k[] = {1, 3, 5}, v[] = {dyn, gy[i], tr}; const uint8_t t[] = {0xC1, 0xA0, 0xC0};
        actor[i] = dyn ? byml_dict(3, k, t, v) : byml_dict(2, k + 1, t + 1, v + 1);
    }
    const uint8_t at6[] = {0xC1, 0xC1, 0xC1, 0xC1, 0xC1, 0xC1};
    uint32_t list = byml_array(6, at6, actor);
    const uint32_t rk[] = {0}, rv[] = {list}; const uint8_t rt[] = {0xC0};
    uint32_t root = byml_dict(1, rk, rt, rv);
    memcpy(buf + 4, &kt, 4); memcpy(buf + 8, &st, 4); memcpy(buf + 12, &root, 4);
    static LandmarkScan scan; static Landmark lm[LANDMARKS];
    CHECK(landmarks_scan_byml(buf, at, &scan) == 5 && scan.has_centre[2] && scan.has_entrance[7] && scan.ndoors == 1 && scan.nwarps == 1 && scan.ntravels == 1,
          "map actors (the Lightroot left out)");
    CHECK(landmarks_scan_byml(buf, 8, &scan) == -1, "not a map file");
    CHECK(landmarks_place(&scan, lm) == 2 && lm[2].placed && !lm[0].placed && lm[LANDMARK_TOWERS + 7].placed, "two placed");
    CHECK(lm[2].at[0] == 103.5f && lm[2].at[1] == 51.5f && lm[2].at[2] == 200, "the tower: 1.5 m in from its door, 1 m up");
    CHECK(lm[LANDMARK_TOWERS + 7].at[0] == -11.5f && lm[LANDMARK_TOWERS + 7].at[1] == 302 && lm[LANDMARK_TOWERS + 7].at[2] == 40, "the shrine: its travel point, 1.5 m up");

    /* the map's place list: City_BaseCamp with one marker, LargeDungeonWind with two, under two marker kinds */
    static const char *const akeys[] = {"City", "Dungeon", "LocationName", "Trans"};
    static const char *const astrs[] = {"Work/Location/City_BaseCamp.game__location__Location.gyml", "Work/Location/LargeDungeonWind.game__location__Location.gyml"};
    at = 16; memset(buf, 0, sizeof buf); memcpy(buf, "YB\x07\0", 4);
    uint32_t akt = byml_table(akeys, 4), ast = byml_table(astrs, 2), kinds[2];
    const float apos[3][3] = {{-200, 100, -100}, {480, 1500, 500}, {490, 1490, 500}};
    for (int kind = 0; kind < 2; kind++) {
        uint32_t vec[2]; const uint8_t ft[3] = {0xD2, 0xD2, 0xD2}, vt[2] = {0xC0, 0xC0};
        for (int m = 0; m < 1 + kind; m++) { const uint32_t fv[3] = {f32(apos[kind + m][0]), f32(apos[kind + m][1]), f32(apos[kind + m][2])}; vec[m] = byml_array(3, ft, fv); }
        uint32_t trans = byml_array(1 + kind, vt, vec);
        const uint32_t k[] = {2, 3}, v[] = {(uint32_t)kind, trans}; const uint8_t t[] = {0xA0, 0xC0};
        uint32_t place = byml_dict(2, k, t, v); const uint8_t pt[] = {0xC1};
        kinds[kind] = byml_array(1, pt, &place);
    }
    const uint32_t ak[] = {0, 1}; const uint8_t aty[] = {0xC0, 0xC0};
    uint32_t aroot = byml_dict(2, ak, aty, kinds);
    memcpy(buf + 4, &akt, 4); memcpy(buf + 8, &ast, 4); memcpy(buf + 12, &aroot, 4);
    float mk[4][3];
    CHECK(landmark_markers(buf, at, "City_BaseCamp", mk, 4) == 1 && mk[0][0] == -200 && mk[0][2] == -100, "Lookout Landing's marker");
    CHECK(landmark_markers(buf, at, "LargeDungeonWind", mk, 4) == 2 && mk[1][1] == 1490, "the Wind Temple's two markers");
    CHECK(landmark_markers(buf, at, "City_Base", mk, 4) == 0 && landmark_markers(buf, at, "HyruleCastle", mk, 4) == 0, "no partial names, no such place");
    Landmark temple = {0};
    CHECK(landmark_place_story(&scan, (const float (*)[3])mk, 2, &temple) && temple.at[0] == 500 && temple.at[1] == 1601.5f, "the temple lands at its travel point, 1.5 m up");
    const float far[1][3] = {{-200, 100, -100}};
    CHECK(!landmark_place_story(&scan, far, 1, &temple) && !temple.placed, "no travel point within 150 m: not placed");
    const float island[3] = {394, 1937, 1260}, on[3] = {390, 2404, 1662}, low[3] = {275, 1565, 912}, below[3] = {400, 122, 1300}, off[3] = {1500, 2000, 1260};
    CHECK(landmark_on_island(island, on) && landmark_on_island(island, low) && !landmark_on_island(island, below) && !landmark_on_island(island, off), "Great Sky Island");

    /* an MSBT with "Tower03" = "Ab" around a tag and a closing tag, inside a SARC */
    at = 0; memset(buf, 0, sizeof buf);
    put("MsgStdBn\xff\xfe\0\0\x01\x03\x02\0", 16); at = 0x20;
    put("LBL1", 4); put32(24); at += 8; put32(1); put32(1); put32(12); put("\x07Tower03", 8); put32(0); align(16);
    const uint16_t utf16[] = {'A', 0x0E, 0, 3, 2, 0x55, 'b', 0x0F, 0, 3, ' ', 0};
    put("TXT2", 4); put32(8 + sizeof utf16); at += 8; put32(1); put32(8); put(utf16, sizeof utf16); align(16);
    size_t msbt_n = at; char name[64];
    CHECK(landmark_msbt_text(buf, msbt_n, "Tower03", name, sizeof name) && !strcmp(name, "Ab"), "message text: %s", name);
    CHECK(!landmark_msbt_text(buf, msbt_n, "Tower04", name, sizeof name), "no such label");
    static uint8_t sarc[0x400]; size_t sn = 0x20 + 16 + 8 + 32;
    memcpy(sarc, "SARC\x14\0\xff\xfe", 8); uint32_t data = (uint32_t)sn; memcpy(sarc + 0x0C, &data, 4);
    memcpy(sarc + 0x14, "SFAT", 4); sarc[0x1A] = 1;
    uint32_t node[4] = {0, 0x01000000, 0, (uint32_t)msbt_n}; memcpy(sarc + 0x20, node, 16);
    memcpy(sarc + 0x30, "SFNT", 4); memcpy(sarc + 0x38, "LocationMsg/Location.msbt", 26);
    memcpy(sarc + sn, buf, msbt_n); size_t mn = 0;
    const uint8_t *m = landmark_sarc_member(sarc, sn + msbt_n, "LocationMsg/Location.msbt", &mn);
    CHECK(m && mn == msbt_n && landmark_msbt_text(m, mn, "Tower03", name, sizeof name), "the message file in its archive");

    /* a save: Tower01 activated, Tower02 found, Dungeon000 cleared, Dungeon001 entered, Dungeon002 only shown */
    static uint8_t save2[0x400]; memcpy(save2, w, 4); size_t fe = 0x28;
    const struct { const char *name; uint32_t value; } flags[] = {
        {"IsActivateCannon.Tower01", 1}, {"IsVisitLocation.Tower01", 1}, {"IsActivateCannon.Tower02", 0}, {"IsVisitLocation.Tower02", 1},
        {"IsActivateCannon.Tower03", 0}, {"IsVisitLocation.Tower03", 0}, {"DungeonState.Dungeon000", dish_murmur3("Clear")},
        {"IsVisitLocation.Dungeon000", 1}, {"DungeonState.Dungeon001", dish_murmur3("Enter")}, {"IsVisitLocation.Dungeon001", 0},
        {"DungeonState.Dungeon002", dish_murmur3("Appear")}, {"IsVisitLocation.Dungeon002", 0}};
    for (size_t i = 0; i < sizeof flags / sizeof *flags; i++, fe += 8) { uint32_t h = dish_murmur3(flags[i].name); memcpy(save2 + fe, &h, 4); memcpy(save2 + fe + 4, &flags[i].value, 4); }
    uint32_t end = 0xa3db7114; memcpy(save2 + fe, &end, 4);
    CHECK(dish_murmur3("IsActivateCannon.Tower01") == 0xb8fb6253 && dish_murmur3("DungeonState.Dungeon000") == 0x3eb899d5 && dish_murmur3("IsVisitLocation.Dungeon000") == 0xa487c021,
          "flag hashes (savegame-editors)");
    LandmarkProgress pr;
    CHECK(landmarks_state_parse(save2, sizeof save2, lm, &pr) == 12, "twelve flags");
    CHECK(!pr.known && !pr.island_done && !pr.depths_found, "no story flags: nothing known");
    CHECK(lm[0].state == LANDMARK_DONE && lm[1].state == LANDMARK_FOUND && lm[2].state == LANDMARK_NOT_FOUND && lm[3].state == LANDMARK_UNKNOWN, "tower states");
    CHECK(lm[15].state == LANDMARK_DONE && lm[16].state == LANDMARK_FOUND && lm[17].state == LANDMARK_NOT_FOUND && lm[18].state == LANDMARK_UNKNOWN, "shrine states");

    /* the story: flag hashes from savegame-editors' hashes.csv, then a save past Great Sky Island with the Wind Temple's quest
       started and the temple visited, the Fire Temple done, the Lightning Temple's quest before the temple is found and no
       Depths yet */
    static const struct { const char *name; uint32_t hash; } story_flags[] = {
        {"Step_MainChallenge", 0x3ee80d28}, {"IsGet.Parasail", 0x3ab92852}, {"IsVisitLocation.MinusField", 0xdd37b5c8},
        {"Step_SageOfWind", 0x683b1363}, {"Step_SageOfFire", 0xeea87aec}, {"Step_SageOfZora", 0x16c8291d}, {"Step_SageOfGerudo", 0xc2bf4337},
        {"Step_SageOfSoul", 0x2b47272e}, {"IsVisitLocation.LargeDungeonWind", 0x30979f76}, {"IsVisitLocation.City_BaseCamp", 0xc93f34c5},
        {"IsVisitLocation.CheckPoint000", 0x7bce687d}, {"ArrivalPointState.CheckPoint000", 0x7983cce0}};
    for (size_t i = 0; i < sizeof story_flags / sizeof *story_flags; i++)
        CHECK(dish_murmur3(story_flags[i].name) == story_flags[i].hash, "murmur3(%s) = %08x, want %08x", story_flags[i].name, dish_murmur3(story_flags[i].name), story_flags[i].hash);
    enum { LOOKOUT = LANDMARK_FIRST_STORY, WIND, FIRE, WATER, THUNDER, SOUL };
    static uint8_t save3[0x400]; memcpy(save3, w, 4); size_t se = 0x28;
    const struct { const char *name, *step; uint32_t value; } story[] = {
        {"Step_MainChallenge", "LookingForZelda", 0}, {"IsGet.Parasail", NULL, 1}, {"IsVisitLocation.MinusField", NULL, 0},
        {"IsVisitLocation.City_BaseCamp", NULL, 1}, {"Step_SageOfWind", "MountainHut", 0}, {"IsVisitLocation.LargeDungeonWind", NULL, 1},
        {"Step_SageOfFire", "TouchSecretStone", 0}, {"Step_SageOfZora", "Ready", 0}, {"IsVisitLocation.LargeDungeonWater", NULL, 0},
        {"Step_SageOfGerudo", "Underground", 0}, {"Step_SageOfSoul", "Ready", 0}, {"IsVisitLocation.CheckPoint005", NULL, 0},
        {"ArrivalPointState.CheckPoint005", "Close", 0}, {"IsVisitLocation.Dungeon060", NULL, 0}};
    for (size_t i = 0; i < sizeof story / sizeof *story; i++, se += 8) {
        uint32_t h = dish_murmur3(story[i].name), v = story[i].step ? dish_murmur3(story[i].step) : story[i].value;
        memcpy(save3 + se, &h, 4); memcpy(save3 + se + 4, &v, 4);
    }
    memcpy(save3 + se, &end, 4);
    for (int k = 0; k < LANDMARKS; k++) { lm[k].placed = 1; lm[k].island = k == LANDMARK_FIRST_SHRINE + 60; lm[k].depths = k == FIRE || k == SOUL; }
    landmarks_state_parse(save3, sizeof save3, lm, &pr);
    CHECK(pr.known && pr.island_done && !pr.depths_found, "past the island, not in the Depths");
    CHECK(lm[LOOKOUT].state == LANDMARK_FOUND && lm[WIND].state == LANDMARK_FOUND && lm[FIRE].state == LANDMARK_DONE && lm[WATER].state == LANDMARK_NOT_FOUND,
          "story states");
    CHECK(lm[LOOKOUT].opened && lm[WIND].opened && lm[FIRE].opened && !lm[WATER].opened && !lm[THUNDER].opened && !lm[SOUL].opened, "opened by the quests");
    landmarks_lock(lm, &pr, 1);
    CHECK(lm[LOOKOUT].lock == LANDMARK_OPEN && lm[WIND].lock == LANDMARK_OPEN && lm[FIRE].lock == LANDMARK_LOCK_DEPTHS && lm[WATER].lock == LANDMARK_LOCK_QUEST
          && lm[THUNDER].lock == LANDMARK_LOCK_QUEST && lm[SOUL].lock == LANDMARK_LOCK_DEPTHS && lm[0].lock == LANDMARK_OPEN, "the lock past the island");
    landmarks_lock(lm, &pr, 0);
    int any = 0; for (int k = 0; k < LANDMARKS; k++) any |= lm[k].lock;
    CHECK(!any, "the lock off: nothing locked");
    const struct { const char *name, *step; } later[3][2] = {
        {{"ArrivalPointState.CheckPoint005", "Open"}, {"Step_SageOfGerudo", "DungeonFind"}},   /* a Lightroot lit, the Lightning Temple found */
        {{"Step_MainChallenge", "GoToBackOfTempleOfTimeAfterWD"}, {"IsGet.Parasail", NULL}},     /* back on Great Sky Island */
        {{"Step_MainChallenge", "Ready"}, {"IsGet.Parasail", "1"}}};                              /* the paraglider alone */
    for (int c = 0; c < 3; c++) {
        for (int j = 0; j < 2; j++) {
            uint32_t h = dish_murmur3(later[c][j].name), v = !later[c][j].step ? 0 : !strcmp(later[c][j].step, "1") ? 1 : dish_murmur3(later[c][j].step);
            for (size_t i = 0x28; i < se; i += 8) if (!memcmp(save3 + i, &h, 4)) memcpy(save3 + i + 4, &v, 4);
        }
        landmarks_state_parse(save3, sizeof save3, lm, &pr); landmarks_lock(lm, &pr, 1);
        if (c == 0) CHECK(pr.depths_found && lm[FIRE].lock == LANDMARK_OPEN && lm[THUNDER].lock == LANDMARK_OPEN && lm[SOUL].lock == LANDMARK_LOCK_QUEST,
                          "a lit Lightroot opens the Depths, the found temple its row");
        if (c == 1) CHECK(pr.known && !pr.island_done && lm[LANDMARK_FIRST_SHRINE + 60].lock == LANDMARK_OPEN && lm[0].lock == LANDMARK_LOCK_ISLAND
                          && lm[LOOKOUT].lock == LANDMARK_LOCK_ISLAND && lm[WIND].lock == LANDMARK_LOCK_ISLAND, "on the island: only its places");
        if (c == 2) CHECK(pr.island_done && lm[0].lock == LANDMARK_OPEN, "the paraglider: off the island");
    }

    /* shrine_auto.h: Dungeon007 outside (a crystal shrine): its entrance at the origin, the travel point 2 m out (+z), the lift
       12 m in (-z) with its floor 1 m below it, and a second shrine's lift that is not this one's */
    static ShrineScan sc;
    const TestActor field[] = {
        {S_ENTRANCE, {0, 100, 0}, -1, 2, 0, {K_INDEX, K_CRYSTAL}, {0xA0, 0xD0}, {S_D7, 1}, {0}},
        {S_WARP, {0, 100.4f, 2}, -1, 0, 0, {0}, {0}, {0}, {0}},
        {S_LIFT, {0, 102, -12}, -1, 2, 0, {K_INDEX, K_EVENT}, {0xA0, 0xA0}, {S_D7, S_GO}, {0}},
        {S_ANCHOR, {0, 101, -13}, S_ENTER_POS, 0, 0, {0}, {0}, {0}, {0}},
        {S_LIFT, {50, 102, -12}, -1, 2, 0, {K_INDEX, K_EVENT}, {0xA0, 0xA0}, {S_D200, S_GO}, {0}},   /* Dungeon200: no such shrine */
        {S_ANCHOR, {0, 100.44f, -1.85f}, S_AUTH, 0, 0, {0}, {0}, {0}, {0}},
    };
    size_t fn = shrine_map(field, 6);
    CHECK(shrine_scan_field(buf, fn, 1, &sc) == 5, "outside: entrance, lift, floor, travel point, AuthenticationPos (Dungeon200 left out)");
    CHECK(shrine_place_field(&sc) == 1 && sc.s[7].outside && sc.s[7].has_warp && sc.s[7].has_lift && sc.s[7].crystal && !sc.s[7].hidden && sc.s[7].cave,
          "Dungeon007 placed: a crystal shrine, in a cave");
    CHECK(sc.s[7].lift_floor[1] == 101 && sc.s[7].warp[2] == 2, "its lift floor and travel point");
    CHECK(shrine_scan_field(buf, 8, 0, &sc) == -1, "not a map file");
    /* inside: the start (in _Dynamic), then the goal, its prompt and floor and a chest turned a quarter (in _Static) */
    const TestActor dyn[] = {{S_START, {0, -2, 30}, S_ON_ELEVATOR, 0, 0, {0}, {0}, {0}, {0}}};
    const TestActor sta[] = {
        {S_GOAL, {0, 26, -282}, -1, 0, 0, {0}, {0}, {0}, {0}},
        {S_GOAL_EXIT, {0, 28.2f, -275}, -1, 0, 0, {0}, {0}, {0}, {0}},
        {S_ANCHOR, {0, 27.2f, -274.25f}, S_EXIT_POS, 0, 0, {0}, {0}, {0}, {0}},
        {S_LIFT, {0, -1, 44}, -1, 1, 0, {K_EVENT}, {0xA0}, {S_RETURN}, {0}},
        {S_ANCHOR, {0, -2, 41.15f}, S_LEAVE_POS, 0, 0, {0}, {0}, {0}, {0}},
        {S_TBOX, {-58, 26, -238}, -1, 0, 1, {0}, {0}, {0}, {0, 1.5707964f, 0}},
        {S_TBOX, {10, 0, 0}, -1, 0, 1, {0}, {0}, {0}, {3.1415927f, 0.5f, 3.1415927f}},
    };
    ShrineInfo *s7 = &sc.s[7];
    fn = shrine_map(dyn, 1);
    CHECK(shrine_scan_inside(buf, fn, s7) == 1 && !s7->inside, "the start alone: not enough");
    fn = shrine_map(sta, 7);
    CHECK(shrine_scan_inside(buf, fn, s7) == 7 && s7->inside && s7->nchests == 2, "inside: goal, prompt, floors, start lift, two chests");
    CHECK(s7->start[2] == 30 && s7->goal[2] == -282 && s7->goal_floor[2] == -274.25f && s7->back[2] == 44, "inside places");
    CHECK(fabsf(s7->chest_front[0][0] - 1) < 1e-4f && fabsf(s7->chest_front[0][1]) < 1e-4f, "a chest turned 90 degrees faces +x");
    CHECK(fabsf(s7->chest_front[1][0] - sinf(0.5f)) < 1e-4f && fabsf(s7->chest_front[1][1] + cosf(0.5f)) < 1e-4f, "x and z turned half round: yaw pi - y");
    /* where Link stands */
    float stand[3], face[3];
    shrine_stand(s7, SHRINE_AT_LIFT, 0, stand, face);
    CHECK(stand[0] == 0 && fabsf(stand[1] - 101.3f) < 1e-4f && stand[2] == -11 && face[2] == -12, "the lift: 1 m out from its prompt, on its floor");
    shrine_stand(s7, SHRINE_AT_GOAL, 0, stand, face);
    CHECK(stand[2] == -274.25f && fabsf(stand[1] - 27.5f) < 1e-4f && face[2] == -282, "the goal: on SmallDungeonExit facing the altar");
    shrine_stand(s7, SHRINE_AT_CHEST, 0, stand, face);
    CHECK(fabsf(stand[0] + 56.7f) < 1e-4f && stand[2] == -238 && face[0] == -58, "a chest: 1.3 m in front of it");
    shrine_stand(s7, SHRINE_AT_GATE, 0, stand, face);
    CHECK(stand[2] == 2 && face[1] == 100, "the gate: on the travel point facing the entrance");
    shrine_stand(s7, SHRINE_AT_ACTIVATE, 0, stand, face);
    CHECK(sc.s[7].has_auth && stand[0] == 0 && fabsf(stand[1] - 100.74f) < 1e-4f && fabsf(stand[2] + 1.35f) < 1e-4f && face[2] < stand[2] - 4.9f,
          "activating: 0.5 m out from AuthenticationPos, facing in");
    shrine_stand(s7, SHRINE_AT_GOAL_APPROACH, 0, stand, face);
    CHECK(stand[0] == 0 && fabsf(stand[2] + 271.75f) < 1e-4f && fabsf(stand[1] - 27.5f) < 1e-4f && face[2] == -275, "the altar: 2.5 m back from SmallDungeonExit, facing its prompt");
    shrine_stand(s7, SHRINE_AT_TRAVEL, 0, stand, face);
    CHECK(stand[0] == 0 && fabsf(stand[1] - 101.9f) < 1e-4f && stand[2] == 2 && face[2] == -1.85f, "landing: the travel point 1.5 m up, facing AuthenticationPos");
    shrine_stand(s7, SHRINE_AT_APPROACH, 0, stand, face);
    CHECK(stand[0] == 0 && fabsf(stand[1] - 100.7f) < 1e-4f && stand[2] == 3 && face[2] == -13, "walking in: 1 m out past the travel point, toward the lift floor");
    shrine_stand(s7, SHRINE_AT_BACK_APPROACH, 0, stand, face);
    CHECK(fabsf(stand[2] - 38.15f) < 1e-4f && fabsf(stand[1] + 1.7f) < 1e-4f && face[2] == 44, "the start lift: 3 m back from its floor toward the start");
    const float fwd[2] = {0, 1}, rgt[2] = {1, 0}, d1[2] = {0.7071f, 0.7071f}, d2[2] = {0, -1}, d3[2] = {-0.995f, 0.0995f};
    CHECK(shrine_stick(fwd, rgt, d1) == (SHRINE_STICK_UP | SHRINE_STICK_RIGHT) && shrine_stick(fwd, rgt, d2) == SHRINE_STICK_DOWN
          && shrine_stick(fwd, rgt, d3) == SHRINE_STICK_LEFT, "WASD toward a direction, from the camera's view");
    /* the kind */
    at = 16; memset(buf, 0, sizeof buf); memcpy(buf, "YB\x07\0", 4);
    { uint32_t kt2 = byml_table(shrine_keys, NKEYS), st2 = byml_table(shrine_strs, NSTRS); const uint32_t k[] = {K_TYPE}, v[] = {S_ENEMY}; const uint8_t t[] = {0xA0};
      uint32_t root2 = byml_dict(1, k, t, v); memcpy(buf + 4, &kt2, 4); memcpy(buf + 8, &st2, 4); memcpy(buf + 12, &root2, 4); }
    CHECK(shrine_kind_parse(buf, at) == SHRINE_PROVING, "Type Enemy: Proving Grounds");
    CHECK(shrine_kind_parse(buf, 4) == SHRINE_PUZZLE, "no file: a puzzle");
    /* which shrines a run does */
    ShrineInfo plain = *s7; plain.crystal = 0; plain.cave = 0;
    CHECK(shrine_skip(s7, SHRINE_STATE_UNKNOWN, LANDMARK_OPEN, 0) == SHRINE_SKIP_CRYSTAL, "a crystal shrine the save does not mention starts Hidden");
    CHECK(shrine_skip(s7, SHRINE_STATE_OPEN, LANDMARK_OPEN, 0) == SHRINE_SKIP_CAVE, "its crystal brought, but in a cave");
    CHECK(shrine_skip(&plain, SHRINE_STATE_APPEAR, LANDMARK_OPEN, 0) == SHRINE_GO, "a shrine found or not: done");
    CHECK(shrine_skip(&plain, SHRINE_STATE_HIDDEN, LANDMARK_OPEN, 0) == SHRINE_SKIP_HIDDEN, "Hidden: not raised yet");
    CHECK(shrine_skip(&plain, SHRINE_STATE_CLEAR, LANDMARK_OPEN, 0) == SHRINE_SKIP_CLEAR, "completed");
    CHECK(shrine_skip(&plain, SHRINE_STATE_APPEAR, LANDMARK_LOCK_ISLAND, 0) == SHRINE_SKIP_LOCKED, "the story lock");
    CHECK(shrine_skip(&plain, SHRINE_STATE_ENTER, LANDMARK_OPEN, SHRINE_SKIP_FAILED) == SHRINE_SKIP_FAILED, "failed in this run");
    ShrineInfo none = {0};
    CHECK(shrine_skip(&none, SHRINE_STATE_APPEAR, LANDMARK_OPEN, 0) == SHRINE_SKIP_NO_DATA, "no map data");
    ShrineInfo fight = plain; fight.kind = SHRINE_COMBAT;
    CHECK(shrine_skip(&fight, SHRINE_STATE_APPEAR, LANDMARK_OPEN, 0) == SHRINE_SKIP_FIGHT, "a fight: left out");
    fight.kind = SHRINE_PROVING;
    CHECK(shrine_skip(&fight, SHRINE_STATE_OPEN, LANDMARK_OPEN, 0) == SHRINE_SKIP_PROVING, "Proving Grounds: left out");
    /* nearest first, from where Link is, then from each shrine done */
    static ShrineInfo row[SHRINES]; int skip[SHRINES];
    for (int k = 0; k < SHRINES; k++) { row[k] = plain; skip[k] = SHRINE_SKIP_LOCKED; row[k].entrance[0] = 1000.0f * k; row[k].entrance[1] = 0; row[k].entrance[2] = 0; }
    skip[3] = skip[10] = skip[11] = SHRINE_GO; row[10].entrance[0] = 2500; row[11].entrance[0] = 9000;
    float from[3] = {2600, 0, 0}; int order[3];
    for (int i = 0; i < 3; i++) { order[i] = shrine_next(row, skip, from); memcpy(from, row[order[i]].entrance, 12); skip[order[i]] = SHRINE_SKIP_DONE; }
    CHECK(order[0] == 10 && order[1] == 3 && order[2] == 11 && shrine_next(row, skip, from) == -1, "nearest next: %d %d %d", order[0], order[1], order[2]);
    /* the save's DungeonState */
    static uint8_t save4[0x200], state[SHRINES]; memcpy(save4, w, 4); size_t ds = 0x28;
    const struct { int k; const char *v; } dstate[] = {{0, "Clear"}, {7, "Hidden"}, {60, "Appear"}, {151, "Enter"}};
    for (size_t i = 0; i < 4; i++, ds += 8) { char nm[40]; snprintf(nm, sizeof nm, "DungeonState.Dungeon%03d", dstate[i].k); uint32_t h = dish_murmur3(nm), v = dish_murmur3(dstate[i].v); memcpy(save4 + ds, &h, 4); memcpy(save4 + ds + 4, &v, 4); }
    memcpy(save4 + ds, &end, 4);
    CHECK(shrine_states_parse(save4, sizeof save4, state) == 4 && state[0] == SHRINE_STATE_CLEAR && state[7] == SHRINE_STATE_HIDDEN && state[60] == SHRINE_STATE_APPEAR
          && state[151] == SHRINE_STATE_ENTER && state[1] == SHRINE_STATE_UNKNOWN, "DungeonState from the save");

    /* korok_auto.h: a map file with four koroks' puzzles (AiGroups) and a group without a korok: a plain rock, a race
       (its goal and start Areas in sub-groups, the goal's TimeLimit), a rock in ice, and a ceiling whose Upper Area takes
       its role from the sub-group one folder up */
    static KorokInfo kor[8]; int nk = 0;
    size_t kn = korok_test_map();
    CHECK(korok_scan_file(buf, kn, 1, kor, &nk, 8) == 4 && nk == 4, "four koroks in the map file, got %d", nk);
    const KorokInfo *rock = &kor[0], *race = &kor[1], *iced = &kor[2], *ceil = &kor[3];
    CHECK(rock->hash == 1001 && rock->hide == KOROK_HIDE_ROCK && rock->type == 0 && !rock->fly && rock->sky && rock->nparts == 1
          && rock->part[0].kind == KOROK_PART_ROCK && korok_complete(rock), "a plain rock korok (no HideType: 0)");
    CHECK(race->hash == 1002 && race->hide == KOROK_HIDE_RING && race->fly && race->time_limit == 120 && korok_complete(race), "a race: Npc_HiddenKorokFly, its TimeLimit");
    const KorokPart *goal = korok_part(race, KOROK_PART_AREA, KOROK_ROLE_GOAL), *start = korok_part(race, KOROK_PART_AREA, KOROK_ROLE_START);
    CHECK(goal && start && goal->pos[0] == 52 && fabsf(goal->scale[0] - 1.7f) < 1e-5f && start->pos[0] == 80 && start->shape == 3, "its goal and start Areas by their sub-groups");
    CHECK(iced->blocked && korok_skip(iced, 0, ~0u, 0, 0) == KOROK_SKIP_BLOCKED, "a rock in ice: left out");
    CHECK(korok_part(ceil, KOROK_PART_AREA, KOROK_ROLE_LOWER) && korok_part(ceil, KOROK_PART_AREA, KOROK_ROLE_UPPER) && korok_complete(ceil),
          "the ceiling's Lower Area by its own folder, the Upper one by the folder above");
    CHECK(korok_scan_file(buf, 8, 0, kor, &nk, 8) == -1, "not a map file");
    /* which koroks a run does */
    CHECK(korok_skip(rock, 0, ~0u, 0, 0) == KOROK_GO, "a plain rock: done");
    CHECK(korok_skip(rock, 1, ~0u, 0, 0) == KOROK_SKIP_FOUND, "found in the save");
    CHECK(korok_skip(rock, 0, ~1u, 0, 0) == KOROK_SKIP_OFF, "its kind switched off");
    CHECK(korok_skip(rock, 0, ~0u, 1, 0) == KOROK_SKIP_LOCKED, "the story lock");
    CHECK(korok_skip(rock, 0, ~0u, 0, KOROK_SKIP_FAILED) == KOROK_SKIP_FAILED, "failed in this run");
    KorokInfo other = *rock; other.hide = 5; other.type = korok_type_of(5);
    CHECK(other.type < 0 && korok_skip(&other, 0, ~0u, 0, 0) == KOROK_SKIP_KIND, "a kind the run does not do");
    KorokInfo offset = *rock; offset.part[0].pos[0] += 2;
    CHECK(korok_skip(&offset, 0, ~0u, 0, 0) == KOROK_SKIP_NO_DATA, "a rock 2 m off its korok is not its korok's rock");
    uint8_t kskip[4] = {KOROK_GO, KOROK_GO, KOROK_SKIP_BLOCKED, KOROK_GO}; float kfrom[3] = {49, 100, 49};
    CHECK(korok_next(kor, 4, kskip, kfrom) == 1, "nearest first");
    kskip[1] = KOROK_SKIP_DONE; CHECK(korok_next(kor, 4, kskip, kfrom) == 0, "then the next nearest");
    /* the save's IsAppearKorok.<hash> (the real flag's hash: MacSpazzy and MrCheeze's table, 43a84d09) */
    CHECK(korok_flag_hash(10044045926791206854ull) == 0x43a84d09u, "IsAppearKorok.10044045926791206854 hashes to 43a84d09");
    static uint8_t save5[0x200], kfound[4]; memcpy(save5, w, 4); size_t ks = 0x28;
    const struct { uint64_t hash; uint32_t v; } kflags[] = {{1001, 1}, {1002, 0}, {1003, 0}};
    for (size_t i = 0; i < 3; i++, ks += 8) { uint32_t h = korok_flag_hash(kflags[i].hash); memcpy(save5 + ks, &h, 4); memcpy(save5 + ks + 4, &kflags[i].v, 4); }
    memcpy(save5 + ks, &end, 4);
    CHECK(korok_found_parse(save5, sizeof save5, kor, 4, kfound) == 3 && kfound[0] == 1 && !kfound[1] && !kfound[2] && !kfound[3], "found from the save");
    /* the steps */
    KorokPlan plan; float kfrom2[3] = {10, 100, 20};
    korok_plan(rock, kfrom2, &plan);
    CHECK(plan.n == 12 && plan.s[0].op == KS_FLY && fabsf(plan.s[0].at[2] - (rock->part[0].pos[2] + 1.6f)) < 1e-4f && fabsf(plan.s[0].at[1] - (rock->part[0].pos[1] + 2.5f)) < 1e-4f
          && plan.s[1].op == KS_LAND && plan.s[2].op == KS_WALK && plan.s[2].to[0] == 1.1f && plan.s[3].op == KS_LIFT && plan.s[3].secs == 1.5f
          && plan.s[7].op == KS_FLY && fabsf(plan.s[7].at[2] - (rock->part[0].pos[2] - 1.6f)) < 1e-4f, "a rock: beside it on Link's side, walk, A held; then the other side");
    korok_plan(race, kfrom2, &plan);
    CHECK(plan.n == 9 && plan.s[1].op == KS_HOLD && plan.s[1].at[0] == 80 && plan.s[2].op == KS_FLY && plan.s[2].at[0] == 52 && plan.s[5].op == KS_LAND && plan.s[7].op == KS_HOLD,
          "a race: held on the stump, to the ring; then stood on the stump, to the ring");
    korok_plan(ceil, kfrom2, &plan);
    CHECK(plan.s[1].op == KS_HOLD && plan.s[2].op == KS_SNAP && plan.s[2].at[1] > plan.s[1].at[1] && plan.s[6].op == KS_LAND, "a ceiling: Lower, at once Upper; then stood and landed");
    KorokInfo fl = {0}; fl.hide = KOROK_HIDE_FLOWERS; fl.type = korok_type_of(fl.hide);
    const struct { float x; int role; } flw[] = {{0, KOROK_ROLE_FIRST}, {30, KOROK_ROLE_WAY}, {10, KOROK_ROLE_WAY}, {40, KOROK_ROLE_LAST}, {20, KOROK_ROLE_WAY}};
    for (int i = 0; i < 5; i++) { KorokPart *pt = &fl.part[fl.nparts++]; pt->kind = KOROK_PART_AREA; pt->role = (uint8_t)flw[i].role; pt->pos[0] = flw[i].x; }
    korok_plan(&fl, kfrom2, &plan);
    CHECK(korok_complete(&fl) && plan.s[1].at[0] == 0 && plan.s[2].at[0] == 10 && plan.s[3].at[0] == 20 && plan.s[4].at[0] == 30 && plan.s[5].at[0] == 40
          && fabsf(plan.s[1].at[1] - 0.8f) < 1e-5f && plan.s[6].op == KS_LAND && plan.s[7].at[0] == 0, "flowers: the first, the nearest one left each time, the last; twice");
    KorokInfo leaf = {0}; leaf.hide = KOROK_HIDE_LEAF; leaf.type = korok_type_of(leaf.hide); leaf.pos[1] = 20; leaf.nparts = 1;
    leaf.part[0].kind = KOROK_PART_AREA; leaf.part[0].pos[0] = 10; leaf.part[0].pos[1] = 20; leaf.part[0].scale[0] = 5;
    korok_plan(&leaf, kfrom2, &plan);
    CHECK(plan.s[2].op == KS_HOLD && fabsf(plan.s[2].at[0] - 8) < 1e-4f && fabsf(plan.s[2].at[1] - 19.1f) < 1e-4f && plan.s[3].op == KS_SWEEP,
          "a sparkle beside its Area: held 0.4 of the Area's size in from its centre");

    printf("{\"status\":\"%s\",\"probe\":\"probe-dish\",\"checked\":%d,\"failures\":%d}\n", failures ? "fail" : "pass", checks, failures);
    return failures != 0;
}
