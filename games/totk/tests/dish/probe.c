/* Unit test for runtime/dish_give.h and the Dishes parts of runtime/spawn_list.h: the GameData name hash against values
   the game and the save editors use, request limits, the change-queue room check, and the actor list's dishes and
   @effect lines (a menu that cannot give dishes leaves them out). Also map_pins.h and landmarks.h (towers and shrines:
   a small map file, message file and save made here). Prints one JSON line; exit 1 on failure. */
#include "dish_give.h"
#include "spawn_list.h"
#include "map_pins.h"
#include "landmarks.h"
#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* A little-endian BYML / MSBT / SARC made here, as the game's files are laid out. */
static uint8_t buf[0x400]; static size_t at;
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

    /* landmarks.h: a map file with one tower, its door 5 m out along x, and one shrine entrance with its travel point */
    static const char *const keys[] = {"Actors", "Dynamic", "DungeonIndexStr", "Gyaml", "TowerNo", "Translate"};
    static const char *const strs[] = {"DgnObj_WarpPoint_Zonau_A_01", "DgnObj_ZonauShrineEntrance_A_01", "Dungeon007", "FldObj_CannonDoor_A_01",
                                       "FldObj_Cannon_A_01", "Tower03"};
    at = 16; memcpy(buf, "YB\x07\0", 4);
    uint32_t kt = byml_table(keys, 6), st = byml_table(strs, 6), actor[4];
    const float pos[4][3] = {{100, 50, 200}, {105, 50.5f, 200}, {-10, 300, 40}, {-11.5f, 300.5f, 40}};
    const uint32_t gy[4] = {4, 3, 1, 0};
    for (int i = 0; i < 4; i++) {
        const uint8_t ft[3] = {0xD2, 0xD2, 0xD2}; const uint32_t fv[3] = {f32(pos[i][0]), f32(pos[i][1]), f32(pos[i][2])};
        uint32_t tr = byml_array(3, ft, fv), dyn = 0;
        if (i == 0) { const uint32_t k[] = {4}, v[] = {5}; const uint8_t t[] = {0xA0}; dyn = byml_dict(1, k, t, v); }
        if (i == 2) { const uint32_t k[] = {2}, v[] = {2}; const uint8_t t[] = {0xA0}; dyn = byml_dict(1, k, t, v); }
        const uint32_t k[] = {1, 3, 5}, v[] = {dyn, gy[i], tr}; const uint8_t t[] = {0xC1, 0xA0, 0xC0};
        actor[i] = dyn ? byml_dict(3, k, t, v) : byml_dict(2, k + 1, t + 1, v + 1);
    }
    const uint8_t at4[] = {0xC1, 0xC1, 0xC1, 0xC1};
    uint32_t list = byml_array(4, at4, actor);
    const uint32_t rk[] = {0}, rv[] = {list}; const uint8_t rt[] = {0xC0};
    uint32_t root = byml_dict(1, rk, rt, rv);
    memcpy(buf + 4, &kt, 4); memcpy(buf + 8, &st, 4); memcpy(buf + 12, &root, 4);
    static LandmarkScan scan; static Landmark lm[LANDMARKS];
    CHECK(landmarks_scan_byml(buf, at, &scan) == 4 && scan.has_centre[2] && scan.has_entrance[7] && scan.ndoors == 1 && scan.nwarps == 1, "map actors");
    CHECK(landmarks_scan_byml(buf, 8, &scan) == -1, "not a map file");
    CHECK(landmarks_place(&scan, lm) == 2 && lm[2].placed && !lm[0].placed && lm[LANDMARK_TOWERS + 7].placed, "two placed");
    CHECK(lm[2].at[0] == 103.5f && lm[2].at[1] == 51.5f && lm[2].at[2] == 200, "the tower: 1.5 m in from its door, 1 m up");
    CHECK(lm[LANDMARK_TOWERS + 7].at[0] == -11.5f && lm[LANDMARK_TOWERS + 7].at[1] == 302 && lm[LANDMARK_TOWERS + 7].at[2] == 40, "the shrine: its travel point, 1.5 m up");

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
    CHECK(landmarks_state_parse(save2, sizeof save2, lm) == 12, "twelve flags");
    CHECK(lm[0].state == LANDMARK_DONE && lm[1].state == LANDMARK_FOUND && lm[2].state == LANDMARK_NOT_FOUND && lm[3].state == LANDMARK_UNKNOWN, "tower states");
    CHECK(lm[15].state == LANDMARK_DONE && lm[16].state == LANDMARK_FOUND && lm[17].state == LANDMARK_NOT_FOUND && lm[18].state == LANDMARK_UNKNOWN, "shrine states");

    printf("{\"status\":\"%s\",\"probe\":\"probe-dish\",\"checked\":%d,\"failures\":%d}\n", failures ? "fail" : "pass", checks, failures);
    return failures != 0;
}
