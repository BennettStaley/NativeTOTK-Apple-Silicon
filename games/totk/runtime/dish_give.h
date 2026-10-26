/* Give a cooked dish (the Spawn tab's Dishes, runtime/actor_spawn.c): the request queue, its limits, the meal effects
   the tab offers and the hash the game stores for an effect. Plain C, header only, no game code: tests/test_dish_give.c
   runs it on any Mac. Callers hold their own lock; nothing here allocates.

   A cooked dish (Item_Cook_*) has no actor pack, so it cannot be spawned into the world; it only exists as a Food slot
   of the inventory's GameData: Pouch.Food.Content[i].{Name, StockNum, LifeRecover (quarter hearts), Price,
   Effect.{Type, Level, Time (seconds)}}. Effect.Type is an enum whose stored value is the 32-bit MurmurHash3 (seed 0) of
   the effect's name, as every GameData name is (Content 0x4290322e, None 0xb6eede09; the member list and the effect
   names: marcrobledo/savegame-editors zelda-totk, MIT; the game itself writes None as 0xb6eede09 in 1.4.3 main+0x20a02cc). */
#pragma once
#include "actor_spawn_queue.h"
#include <stdint.h>
#include <string.h>

enum {
    DISH_QUEUE_MAX = 8,
    DISH_LIFE_MAX = 400,       /* quarter hearts: 100 hearts, far above the game's own 160 (its cooking table's MaxLv) */
    DISH_LEVEL_MAX = 250,      /* the save editor's limit for Effect.Level; the tab keeps to the game's own range */
    DISH_TIME_MAX = 59999,     /* seconds, the save editor's limit for Effect.Time; the tab offers up to 30:00 */
    DISH_PRICE_MAX = 9999,
};

/* What runtime_dish_give returns besides SPAWN_OK and the SPAWN_ERR_* codes (actor_spawn_queue.h). */
enum {
    DISH_ERR_NOT_DISH = -20,   /* not an Item_Cook_* name */
    DISH_ERR_EFFECT = -21,     /* an effect the tab does not offer */
    DISH_ERR_VALUE = -22,      /* hearts, level, time or price out of range */
};
static inline const char *dish_error_text(int code) {
    switch (code) {
    case DISH_ERR_NOT_DISH: return "not a cooked dish";
    case DISH_ERR_EFFECT: return "unknown meal effect";
    case DISH_ERR_VALUE: return "hearts, level, duration or price out of range";
    default: return spawn_error_text(code);
    }
}

/* MurmurHash3 x86 32-bit, seed 0, of a NUL-terminated string: the game's GameData name hash. */
static inline uint32_t dish_murmur3(const char *text) {
    const uint8_t *p = (const uint8_t *)text;
    size_t len = strlen(text), blocks = len / 4;
    uint32_t h = 0, k;
    const uint32_t c1 = 0xcc9e2d51u, c2 = 0x1b873593u;
    for (size_t i = 0; i < blocks; i++, p += 4) {
        k = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        k *= c1; k = k << 15 | k >> 17; k *= c2;
        h ^= k; h = h << 13 | h >> 19; h = h * 5 + 0xe6546b64u;
    }
    k = 0;
    switch (len & 3) {
    case 3: k ^= (uint32_t)p[2] << 16; /* fall through */
    case 2: k ^= (uint32_t)p[1] << 8;  /* fall through */
    case 1: k ^= p[0]; k *= c1; k = k << 15 | k >> 17; k *= c2; h ^= k;
    }
    h ^= (uint32_t)len;
    h ^= h >> 16; h *= 0x85ebca6bu; h ^= h >> 13; h *= 0xc2b2ae35u; h ^= h >> 16;
    return h;
}

/* The meal effects the tab offers, with our English label. The game's cooking table (Cooking/CookingTable SystemData
   EffectList, read by scripts/prepare_actor_list.py into the actor list) also has LifeRecover (that is the hearts field,
   not an effect), TwiceJump and EmergencyAvoid; the last two are left out because the Effect.Type enum the save editor
   lists for Pouch.Food has no such value, so the game may refuse them. Level and duration limits come from the game's
   table at run time (SpawnEffect in spawn_list.h). */
typedef struct { const char *name, *label; } DishEffect;
static const DishEffect DISH_EFFECTS[] = {
    {"None", "No effect"},
    {"LifeMaxUp", "Extra hearts"},
    {"StaminaRecover", "Stamina recovery"},
    {"ExStaminaMaxUp", "Extra stamina"},
    {"LifeRepair", "Gloom recovery"},
    {"ResistHot", "Heat resistance"},
    {"ResistCold", "Cold resistance"},
    {"ResistElectric", "Shock resistance"},
    {"ResistBurn", "Flame guard"},
    {"AllSpeed", "Speed up"},
    {"AttackUp", "Attack up"},
    {"DefenseUp", "Defense up"},
    {"QuietnessUp", "Stealth up"},
    {"LightEmission", "Glow"},
    {"NotSlippy", "Slip resistance"},
    {"SwimSpeedUp", "Swim speed up"},
    {"AttackUpCold", "Cold weather attack"},
    {"AttackUpHot", "Hot weather attack"},
    {"AttackUpThunderstorm", "Stormy weather attack"},
    {"MiasmaGuard", "Gloom resistance"},
};
enum { DISH_EFFECT_COUNT = (int)(sizeof DISH_EFFECTS / sizeof *DISH_EFFECTS) };

/* The table index of an effect name, or -1. */
static inline int dish_effect_find(const char *name) {
    for (int i = 0; name && i < DISH_EFFECT_COUNT; i++) if (!strcmp(DISH_EFFECTS[i].name, name)) return i;
    return -1;
}
/* Effects whose level is an amount (quarter hearts for LifeMaxUp and LifeRepair) rather than a tier 1-3; the tab shows
   them as hearts. */
static inline int dish_effect_in_hearts(const char *name) {
    return name && (!strcmp(name, "LifeMaxUp") || !strcmp(name, "LifeRepair"));
}

/* Cooked dish actor names: Item_Cook_<letter>_<number>[...] (Item_Cook_A_01 Mushroom Skewer, Item_Cook_C_17 Elixir). */
static inline int dish_name_valid(const char *name) {
    return spawn_name_valid(name) && !strncmp(name, "Item_Cook_", 10) && name[10];
}

/* A sale price like the game's for an ordinary dish: a few rupees, more for more hearts and a stronger effect. */
static inline int dish_default_price(int life_quarters, int level, int has_effect) {
    int price = 2 + life_quarters / 2 + (has_effect ? 10 * (level > 3 ? 3 : level) : 0);
    return price < 1 ? 1 : price > DISH_PRICE_MAX ? DISH_PRICE_MAX : price;
}

typedef struct {
    char name[SPAWN_NAME_MAX];
    int life;          /* quarter hearts */
    int effect;        /* DISH_EFFECTS index; 0 None */
    int level, time;   /* 0 and 0 with None */
    int price, count;  /* count dishes, one inventory slot each (as cooking makes them) */
} DishRequest;
typedef struct { DishRequest item[DISH_QUEUE_MAX]; unsigned head, len; } DishQueue;

/* SPAWN_OK, or why the request is refused. With effect None the level and time are stored as 0. */
static inline int dish_request_make(DishRequest *r, const char *name, int life, const char *effect, int level, int time,
                                    int price, int count) {
    if (!dish_name_valid(name)) return spawn_name_valid(name) ? DISH_ERR_NOT_DISH : SPAWN_ERR_NAME;
    if (count < 1 || count > SPAWN_COUNT_MAX) return SPAWN_ERR_COUNT;
    int e = dish_effect_find(effect && *effect ? effect : "None");
    if (e < 0) return DISH_ERR_EFFECT;
    if (!e) level = time = 0;
    if (life < 0 || life > DISH_LIFE_MAX || level < 0 || level > DISH_LEVEL_MAX || time < 0 || time > DISH_TIME_MAX ||
        price < 0 || price > DISH_PRICE_MAX || (e && level < 1))
        return DISH_ERR_VALUE;
    memset(r, 0, sizeof *r);
    strcpy(r->name, name);   /* dish_name_valid bounded it */
    r->life = life; r->effect = e; r->level = level; r->time = time; r->price = price; r->count = count;
    return SPAWN_OK;
}
static inline int dish_queue_push(DishQueue *q, const DishRequest *r) {
    if (q->len >= DISH_QUEUE_MAX) return SPAWN_ERR_FULL;
    q->item[(q->head + q->len++) % DISH_QUEUE_MAX] = *r;
    return SPAWN_OK;
}
static inline DishRequest *dish_queue_front(DishQueue *q) { return q->len ? &q->item[q->head] : NULL; }
static inline void dish_queue_drop(DishQueue *q) {
    if (!q->len) return;
    q->head = (q->head + 1) % DISH_QUEUE_MAX;
    q->len--;
}

/* GameData change queues (1.4.3, gmd::GameDataMgr): every setter pushes into a fixed-size queue of its type, and a full
   queue ends the game (the setters branch to main+0x27ea690, which does not return). Each store keeps the capacity at
   +40, the entries at +48 and the write index in the low 20 bits of +56 (the same layout afiaka87/totk-mods
   GameDataQueue.hpp, MIT, checks for 1.2.1; for 1.4.3 read from the setters' own loads, actor_spawn.c). */
typedef struct { int32_t capacity; uint64_t buffer; uint32_t control; } DishStoreView;
enum { DISH_QUEUE_HEADROOM = 2 };
static inline int dish_store_has_room(DishStoreView s, int pushes) {
    int32_t used = (int32_t)(s.control & 0xfffff);
    return s.capacity > 0 && s.buffer && used <= s.capacity && pushes + DISH_QUEUE_HEADROOM <= s.capacity - used;
}
