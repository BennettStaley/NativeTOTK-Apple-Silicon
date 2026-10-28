/* Unit test for runtime/dish_give.h and the Dishes parts of runtime/spawn_list.h: the GameData name hash against values
   the game and the save editors use, request limits, the change-queue room check, and the actor list's dishes and
   @effect lines (a menu that cannot give dishes leaves them out). Also map_pins.h and landmarks.h (towers, shrines and
   the story's places: a small map file, place list, message file and save made here; the story flags and the lock), and
   shrine_auto.h (a shrine's outside and inside map files, its kind, the save's DungeonState, which shrines a run does and
   in which order, where Link stands). Prints one JSON line; exit 1 on failure. */
#include "dish_give.h"
#include "spawn_list.h"
#include "map_pins.h"
#include "landmarks.h"
#include "shrine_auto.h"
#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* A little-endian BYML / MSBT / SARC made here, as the game's files are laid out. */
static uint8_t buf[0x2000]; static size_t at;
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
enum { S_ENTRANCE, S_LIFT, S_ANCHOR, S_WARP, S_D7, S_GO, S_ENTER_POS, S_START, S_ON_ELEVATOR, S_GOAL, S_GOAL_EXIT, S_EXIT_POS, S_TBOX, S_RETURN, S_LEAVE_POS, S_ENEMY, S_D200, NSTRS };
static const char *const shrine_strs[NSTRS] = {"DgnObj_ZonauShrineEntrance_A_01", "FldObj_EnterAttetionActor", "DestinationAnchor", "DgnObj_WarpPoint_Zonau_A_01",
                                               "Dungeon007", "DmF_SY_SmallDungeon2Go", "EnterSmallDungeonPos", "StartPos", "OnElevator", "DgnObj_Hrl_GoalSystem_B_01",
                                               "DgnObj_GoalExit", "SmallDungeonExit", "TBox_Dungeon_Stone", "DmF_SY_SmallDungeon4Return", "LeaveSmallDungeonPos", "Enemy", "Dungeon200"};
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
    };
    size_t fn = shrine_map(field, 5);
    CHECK(shrine_scan_field(buf, fn, 1, &sc) == 4, "outside: entrance, lift, floor, travel point (Dungeon200 left out)");
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

    printf("{\"status\":\"%s\",\"probe\":\"probe-dish\",\"checked\":%d,\"failures\":%d}\n", failures ? "fail" : "pass", checks, failures);
    return failures != 0;
}
