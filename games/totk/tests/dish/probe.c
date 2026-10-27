/* Unit test for runtime/dish_give.h and the Dishes parts of runtime/spawn_list.h: the GameData name hash against values
   the game and the save editors use, request limits, the change-queue room check, and the actor list's dishes and
   @effect lines (a menu that cannot give dishes leaves them out). Prints one JSON line; exit 1 on failure. */
#include "dish_give.h"
#include "spawn_list.h"
#include "map_pins.h"
#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

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

    printf("{\"status\":\"%s\",\"probe\":\"probe-dish\",\"checked\":%d,\"failures\":%d}\n", failures ? "fail" : "pass", checks, failures);
    return failures != 0;
}
