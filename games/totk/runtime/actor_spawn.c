/* Actor spawner: creates any actor (items, weapons, enemies, Zonai devices, materials) next to the player by calling the
   game's own engine::actor::ActorMgr::requestCreateActorAsync, the call the game uses to place actors itself. No code is
   patched (so it works on iOS too): requests wait in a queue and run on the game's frame-end thread, the point where
   world_survey.c already calls game code (runtime_survey_tick, after the game's frame-end GPU wait). That tick is set up
   in every 1.4.3 session (loader.c runtime_frame_controller_return_pc); the survey's own commands stay test-only, and in
   normal play only the menu's Spawn tab queues requests (the input file's "spawn" needs TOTK_SURVEY=1).

   1.0.0 uses the public symbols directly (same entry check, 105 callers). 1.2.1 (lotuskit: 0xab92cc, sInstance 0x4722920)
   waits for a 1.2.1 main to check against. 1.4.3 addresses (game_version.h, GAME_SPAWN_*), found from 1.0.0, whose symbols are public (totk-lotuskit
   syms/exking/engine/actor/ActorMgr.py, from dt-12345/totk_syms):
     requestCreateActorAsync 1.0.0 0xa5fdf8 -> 1.4.3 0x599670: the only target that the mapped callers agree on
       (scripts/map_version_sites.py, by callers), 108 direct callers in 1.4.3 against 105 in 1.0.0, the same 12 calls
       inside, identical first seven instructions, and the same CreateArg fields read (+0x48..+0x53 bytes, +0x68..+0x88).
     ActorMgr::sInstance 1.0.0 0x46479e0 -> 1.4.3 0x3ad7450: 87 of the callers load x0 from one GOT slot (1.0.0 0x4557b48,
       1.4.3 0x395e008) whose relocation names exactly these addresses; map_data finds the same through code references.
   Call shape and CreateArg layout (facts, not code, from totk-lotuskit and dt-12345/actor_spawn, both GPL-2.0):
     bool requestCreateActorAsync(ActorMgr *, const SafeString &name, const CreateArg &, CreateWatcherRef *,
                                  CreatePriority (High = 1), PreActor *, ActorFile *, Function *, bool,
                                  Result *, PreActor **out);
   SafeString here is one pointer (the function reads the name through [x1]). CreateArg is 0x98 bytes, below. */
#include "actor_spawn.h"
#include "actor_spawn_queue.h"
#include "dish_give.h"
#include "game_version.h"
#include "world_survey.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(GAME_SPAWN_REQUEST_CREATE) && !defined(GAME_BOTW)
#define SPAWN_SUPPORTED 1
#endif
#if defined(SPAWN_SUPPORTED) && defined(GAME_DISH_COOK) && defined(TOTK_GAME_V143)
#define DISH_SUPPORTED 1   /* give a cooked dish: 1.4.3 only (the end of this file) */
#endif
static int dish_init(Module *m);
static void dish_tick(uint64_t frame);
static int dish_waiting(void);

_Atomic int runtime_spawn_waiting;
static _Atomic int spawn_commands;   /* the input file's "spawn" lines: test profiles only (world_survey.c) */
static _Atomic(RuntimeSpawnObserver) spawn_observer;
static _Atomic int spawn_place;   /* SPAWN_PLACE_*: the Spawn tab's Place popup */
void runtime_spawn_set_placement(int place) { if (place >= 0 && place < SPAWN_PLACE_COUNT) atomic_store(&spawn_place, place); }
int runtime_spawn_placement(void) { return atomic_load(&spawn_place); }
void runtime_spawn_allow_commands(void) { atomic_store(&spawn_commands, 1); }
void runtime_spawn_observe(RuntimeSpawnObserver fn) { atomic_store(&spawn_observer, fn); }
#ifdef SPAWN_SUPPORTED
static pthread_mutex_t spawn_lock = PTHREAD_MUTEX_INITIALIZER;
static SpawnQueue spawn_queue;
static _Atomic int spawn_enabled;

int runtime_romfs_has_file(const char *path);   /* romfs.c */
static int actor_exists(const char *name) {
    char path[SPAWN_NAME_MAX + 32];
    snprintf(path, sizeof path, "Pack/Actor/%s.pack.zs", name);
    return runtime_romfs_has_file(path);
}
#endif

int runtime_spawn_actor(const char *name, int count) { return runtime_spawn_actor_at(name, count, runtime_spawn_placement()); }
int runtime_spawn_actor_at(const char *name, int count, int place) {
#ifndef SPAWN_SUPPORTED
    (void)name; (void)count; (void)place;
    return SPAWN_ERR_UNSUPPORTED;
#else
    if (!atomic_load(&spawn_enabled)) return SPAWN_ERR_DISABLED;
    pthread_mutex_lock(&spawn_lock);
    int r = spawn_queue_push_at(&spawn_queue, name, count, place, actor_exists);
    pthread_mutex_unlock(&spawn_lock);
    if (r == SPAWN_OK) {   /* waiting first, then the frame-end wake-up (world_survey.c clears them in the other order) */
        atomic_store(&runtime_spawn_waiting, 1);
        atomic_store(&runtime_survey_pending, 1);
    }
    return r;
#endif
}

const char *runtime_spawn_error(int code) { return spawn_error_text(code); }

int runtime_spawn_available(void) {
#ifndef SPAWN_SUPPORTED
    return SPAWN_ERR_UNSUPPORTED;
#else
    return atomic_load(&spawn_enabled) ? SPAWN_OK : SPAWN_ERR_DISABLED;
#endif
}

/* "spawn <name> [count]" from the test input file (input.c), next to world_survey's "survey" commands. */
int runtime_spawn_command(const char *line) {
    if (strncmp(line, "spawn ", 6)) return 0;
    if (!atomic_load(&spawn_commands)) {
        fprintf(stderr, "[spawn] rejected: the input file's spawn needs TOTK_SURVEY=1 on a test profile (players use the Spawn tab)\n");
        return 1;
    }
    char name[SPAWN_NAME_MAX + 1] = {0}, word[16] = {0}; int count = 1, used = 0, place = runtime_spawn_placement();
    if (sscanf(line, "spawn %96s %d %15s %n", name, &count, word, &used) == 3 && used && !line[used]) {
        if ((place = spawn_place_parse(word)) < 0) used = 0;
    } else {
        used = 0;
        if (!(sscanf(line, "spawn %96s %d %n", name, &count, &used) == 2 && used && !line[used])) {
            count = 1; used = 0;
            if (!(sscanf(line, "spawn %96s %n", name, &used) == 1 && used && !line[used])) used = 0;
        }
    }
    if (!used) {
        fprintf(stderr, "[spawn] rejected: expected spawn <actor name> [count 1..%d [front|above|far|feet]]\n", SPAWN_COUNT_MAX);
        return 1;
    }
    int r = runtime_spawn_actor_at(name, count, place);
    if (r == SPAWN_OK) fprintf(stderr, "[spawn] queued name=%s count=%d place=%s\n", name, count, SPAWN_PLACE_WORDS[place]);
    else fprintf(stderr, "[spawn] refused name=%.96s count=%d: %s\n", spawn_name_valid(name) ? name : "(invalid)", count, spawn_error_text(r));
    return 1;
}

#ifndef SPAWN_SUPPORTED
int runtime_spawn_init(Module *m) { (void)m; return 0; }
void runtime_spawn_tick(uintptr_t player, uint64_t frame) { (void)player; (void)frame; }
#else
typedef struct {
    float position[3];
    float rotation[9];          /* row-major 3x3 */
    float scale[3];
    uint32_t pad_3c;
    const char *name_40;        /* a SafeString, "" */
    uint8_t b48[8];             /* read as bytes by the call: +0x48, +0x4b, +0x4d */
    uint8_t b50[4];             /* +0x50, +0x51 (halfword), +0x53 */
    uint32_t pad_54;
    void *blackboard_info;      /* bb::InitInfo<32> * */
    void *blackboard_60;
    void *parent, *dependent, *creator, *creator_rtti, *instance_heap;
    uint8_t transform_flags;    /* 1 position, 2 rotation, 4 scale */
    uint8_t pad_91[7];
} CreateArg;
_Static_assert(sizeof(CreateArg) == 0x98, "CreateArg is 0x98 bytes");
_Static_assert(__builtin_offsetof(CreateArg, blackboard_info) == 0x58, "blackboard at +0x58");
_Static_assert(__builtin_offsetof(CreateArg, instance_heap) == 0x88, "heap at +0x88");
_Static_assert(__builtin_offsetof(CreateArg, transform_flags) == 0x90, "flags at +0x90");

/* bb::InitInfo<32>: sead::Buffer<Param> {s32 size; Param *ptr}, s32 count, then 32 Params of 0xc8 bytes; empty here. */
typedef struct { int32_t size; uint32_t pad; void *params; int32_t count; uint32_t pad2; uint8_t buffer[32][0xc8]; } BlackboardInit;
_Static_assert(sizeof(BlackboardInit) == 0x1918, "bb::InitInfo<32> is 0x1918 bytes");

typedef _Bool (*RequestCreate)(void *mgr, const char *const *name, const CreateArg *arg, void *watcher, uint32_t priority,
                               void *pre_actor, void *actor_file, void *function, _Bool flag, uint32_t *result, void **out);
static RequestCreate request_create;
static void **actor_mgr_slot;

static const char *result_text(uint32_t r) {
    static const char *const names[] = {"FailedToAcquireWatcher", "Ok", "EmptyActorName", "FailedToCreatePreActor",
        "FailedToCreateBlackboard", "EmptyNameRef", "NoMatchingFactory", "FailedToAllocateRequest", "InvalidWatcher"};
    return r < sizeof names / sizeof *names ? names[r] : "?";
}

/* Every 1.0.0 and 1.4.3 session (the same entry check for both): the call is only made for requests the player (the Spawn tab) or a test queued. */
int runtime_spawn_init(Module *m) {
    static const uint32_t entry[] = {0xa9ba7bfd, 0xa9016ffc, 0x910003fd, 0xa90267fa, 0xa9035ff8, 0xa90457f6, 0xa9054ff4};
    int dishes = dish_init(m);   /* its own checks; it needs the same frame-end tick, so either one keeps the tick */
    if (strcmp(m->name, "main") || GAME_SPAWN_REQUEST_CREATE + sizeof entry > m->text_end || GAME_SPAWN_ACTOR_MGR + 8u > m->size ||
        memcmp(m->base + GAME_SPAWN_REQUEST_CREATE, entry, sizeof entry)) {
        fprintf(stderr, "[spawn] off: requestCreateActorAsync entry does not match " TOTK_GAME_VERSION "\n");
        return dishes;
    }
    request_create = (RequestCreate)(void *)(m->base + GAME_SPAWN_REQUEST_CREATE);
    actor_mgr_slot = (void **)(void *)(m->base + GAME_SPAWN_ACTOR_MGR);
    atomic_store(&spawn_enabled, 1);
    fprintf(stderr, "[spawn] enabled: the Spawn tab's requests run on the game's frame-end thread\n");
    return 1;
}

static void spawn_one(const SpawnRequest *r, const float base[3], float fx, float fz, uint64_t frame) {
    static BlackboardInit blackboard;   /* frame-end thread only; the call copies what it needs */
    void *mgr = *actor_mgr_slot;
    RuntimeSpawnObserver observer = atomic_load(&spawn_observer);
    if (!mgr) {
        fprintf(stderr, "[spawn] name=%s status=no_actor_mgr frame=%llu\n", r->name, (unsigned long long)frame);
        if (observer) observer(r->name, r->count, 0, "the game's actor manager is not up yet");
        return;
    }
    unsigned created = 0; uint32_t last = 0;
    for (int k = 0; k < r->count; ++k) {
        float off[3];
        spawn_place_offset(k, r->count, fx, fz, r->place, off);
        CreateArg arg;
        memset(&arg, 0, sizeof arg);
        for (int c = 0; c < 3; c++) arg.position[c] = base[c] + off[c];
        arg.rotation[0] = arg.rotation[4] = arg.rotation[8] = 1.0f;
        arg.scale[0] = arg.scale[1] = arg.scale[2] = 1.0f;
        arg.name_40 = "";
        memset(&blackboard, 0, sizeof blackboard);
        blackboard.size = 32; blackboard.params = blackboard.buffer;
        arg.blackboard_info = &blackboard;
        arg.transform_flags = 1 | 2 | 4;
        const char *name = r->name;
        uint32_t result = 0; void *pre_actor = NULL;
        _Bool ok = request_create(mgr, &name, &arg, NULL, 1, NULL, NULL, NULL, 0, &result, &pre_actor);
        created += ok; last = result;
        if (!ok) break;   /* the same name fails the same way again */
    }
    fprintf(stderr, "[spawn] name=%s requested=%d created=%u result=%u(%s) place=%s at=%.2f,%.2f,%.2f frame=%llu\n", r->name,
            r->count, created, last, result_text(last), SPAWN_PLACE_WORDS[r->place], base[0], base[1], base[2], (unsigned long long)frame);
    if (observer) {
        char why[96];
        snprintf(why, sizeof why, "the game refused it (%s)", result_text(last));
        observer(r->name, r->count, (int)created, (int)created == r->count ? NULL : why);
    }
}

/* Frame-end thread (world_survey.c runtime_survey_tick). player: the Player actor, or 0 when there is none yet. */
void runtime_spawn_tick(uintptr_t player, uint64_t frame) {
    if (!atomic_load_explicit(&runtime_spawn_waiting, memory_order_acquire)) return;
    if (player) dish_tick(frame);   /* at most one dish a frame, from its own queue */
    pthread_mutex_lock(&spawn_lock);
    SpawnRequest batch[SPAWN_QUEUE_MAX]; unsigned n = 0;
    if (player) while (n < SPAWN_QUEUE_MAX && spawn_queue_pop(&spawn_queue, &batch[n])) n++;
    atomic_store_explicit(&runtime_spawn_waiting, spawn_queue.len != 0 || dish_waiting(), memory_order_release);
    pthread_mutex_unlock(&spawn_lock);
    if (!player) return;   /* loading or a menu: keep the requests for a frame with a player */
    float pos[3], rot[9];
    memcpy(pos, (const void *)(player + GAME_ACTOR_POS), sizeof pos);
    memcpy(rot, (const void *)(player + GAME_ACTOR_ROT), sizeof rot);
    /* the actor's facing is its local +Z: the third column of the row-major rotation, flattened to the ground */
    float fx = rot[2], fz = rot[8], len = sqrtf(fx * fx + fz * fz);
    if (!isfinite(len) || len < 1e-3f) { fx = 0; fz = 1; } else { fx /= len; fz /= len; }
    for (unsigned i = 0; i < n; ++i) spawn_one(&batch[i], pos, fx, fz, frame);
}
#endif

/* ---- give a cooked dish (the Spawn tab's Dishes; dish_give.h) ----

   A cooked dish (Item_Cook_*) has no actor pack, so requestCreateActorAsync cannot make one; it exists only as a slot of
   the inventory's Food pouch in GameData. The game puts a dish there with one routine, 1.4.3 main+0x20a0144 (1.0.0
   0x1a14800; called from PouchMgr::addToPouch in both versions, 1.4.3 +0x2098dc8, 1.0.0 +0xc9446c, and from two other
   places): it takes the first Food Content slot whose Name is empty and writes Effect.{Type, Level, Time}, LifeRecover,
   MaterialName[], Name, Price and StockNum = 1 through the GameData setters, then sets the Food pouch's "new" flag
   (bool 0xe1754905) in both pouches. It never merges a dish into another slot. We make the same calls in the same
   order with the player's values instead of a cooking result. Left out: the ingredient names (MaterialName) and the
   routine's last call (+0x20a1210, the cooking records, which reads the whole cooking result), so a given dish lists no
   ingredients and does not count as a discovered recipe. PouchMgr::addToPouch (1.4.3 0x20986b0, afiaka87/totk-mods
   arrowbound GameProfiles.hpp, MIT; 1.0.0 0xc93dc0 per dt-12345/add-material-example, GPL-2.0, facts only) is not used:
   for Food it fills the slot from the actor's own parameters, which these pack-less dishes do not have, and it may
   stack onto a slot of the same name.

   How the 1.4.3 addresses (game_version.h, GAME_DISH_*) were found. The routine's call sequence is the same in 1.0.0
   and 1.4.3 (same 0xa0-byte frame, the same calls in the same order), and 1.0.0's calls are known by name:
   setStructInt 0x8202b0, setStructEnum 0x820bfc, getStructStruct 0x81fed4 (aquacluck/totk-lotuskit
   syms/exking/gmd/GameDataMgr.py, GPL-2.0, facts only), getStructStructByIndex 0x937298, getStructString64 0x9d722c and
   the Food count 0xb91738 (arrowbound GameProfiles.hpp). scripts/map_version_sites.py's caller mapping (map_by_callers,
   from 1.0.0) agrees on every one: the 1.0.0 callers that map land on 0x16fdcc (setStructInt), 0x170210 (setStructEnum,
   40 callers agree), 0x16fac0 (getStructStruct), 0x282ed8 (getStructStructByIndex), 0x6e76d0 (getStructString64),
   0xd61d6c (Food count), 0x282280 (setStructString64, 1.0.0 0xb0e210), 0x1b02f8 / 0x923d20 (get/setStructBool, 1.0.0
   0xaf69b0 / 0x755f20), and none disagrees; the caller counts match too (setStructInt 313 / 317, setStructEnum 278 /
   291, getStructStruct 439 / 430, getStructStructByIndex 447 / 452). arrowbound names 0x2ab6b34 as 1.4.3's
   getStructStructByIndex: a near twin with 9 callers that the cooking routine does not use; we use the routine's own
   0x282ed8. GameDataMgr: the routine loads it through GOT 0x395dff0, whose relocation is 0x3ad66b0 (arrowbound's slot;
   1.0.0 GOT 0x4557b20 -> 0x4646c60 = lotuskit's GameDataMgr::sInstance). PouchMgr: read directly from 0x3a81678
   (arrowbound's GOT 0x39601e8 relocates to it; 1.0.0 0x45f28a8). The pouch layout (PouchMgr +0x8: two pouches of 0xc0
   bytes, a 16-byte struct handle per category, Food = 6 at +0x60, the pouch in use at +0x188, used only when below 2)
   is read from the routine itself (+0x40..+0x70) and from 0xd61d6c.
   Signatures (the routine's arguments; bivouac GameDataService.cpp in afiaka87/totk-mods, MIT, uses the same shapes for
   1.2.1): bool getStructStructByIndex(mgr, Handle *out, const Handle *, u32 array hash, u32 index);
   bool getStructString64(mgr, const char **out, const Handle *, u32 hash); bool getStructStruct(mgr, Handle *out,
   const Handle *, u32 hash); setStructEnum(mgr, u32 value, const Handle *, u32 hash); setStructInt(mgr, s32, ...);
   setStructString64(mgr, const char *const *value, ...) (the text is copied into the change queue);
   bool getStructBool(mgr, u8 *out, ...); setStructBool(mgr, u32, ...); s32 count(PouchMgr *, u32 category).

   At start-up every target's first words are compared with 1.4.3's, the routine's nine calls must still branch to
   these targets and its two loads must still address these globals, and the setters' change-queue loads must still be
   where they were; any difference turns giving off with a [red] line instead of calling a wrong address. The setters
   push into fixed-size change queues and a full queue ends the game, so a dish is written only when every queue it
   touches has room (else it waits a frame, as afiaka87's GameDataQueue.hpp does). The game applies the queues later, so
   a slot just written still reads empty: one dish a frame, and a slot written in the last second is skipped. */
#ifndef DISH_SUPPORTED
__attribute__((unused)) static int dish_init(Module *m) { (void)m; return 0; }
__attribute__((unused)) static void dish_tick(uint64_t frame) { (void)frame; }
__attribute__((unused)) static int dish_waiting(void) { return 0; }
int runtime_dish_available(void) { return SPAWN_ERR_UNSUPPORTED; }
int runtime_dish_give(const char *name, int life, const char *effect, int level, int seconds, int price, int count) {
    (void)name; (void)life; (void)effect; (void)level; (void)seconds; (void)price; (void)count;
    return SPAWN_ERR_UNSUPPORTED;
}
#else
typedef struct { uintptr_t ptr; uint32_t index; uint8_t flag, pad[3]; } GmdHandle;   /* the game zeroes all three first */
_Static_assert(sizeof(GmdHandle) == 16, "GameData struct handle is 16 bytes");
typedef int32_t (*PouchCountFn)(void *pouch, uint32_t category);
typedef uint32_t (*ByIndexFn)(void *mgr, GmdHandle *out, const GmdHandle *handle, uint32_t hash, uint32_t index);
typedef uint32_t (*GetString64Fn)(void *mgr, const char **out, const GmdHandle *handle, uint32_t hash);
typedef uint32_t (*GetStructFn)(void *mgr, GmdHandle *out, const GmdHandle *handle, uint32_t hash);
typedef void (*SetEnumFn)(void *mgr, uint32_t value, const GmdHandle *handle, uint32_t hash);
typedef void (*SetIntFn)(void *mgr, int32_t value, const GmdHandle *handle, uint32_t hash);
typedef void (*SetString64Fn)(void *mgr, const char *const *value, const GmdHandle *handle, uint32_t hash);
typedef uint32_t (*GetBoolFn)(void *mgr, uint8_t *out, const GmdHandle *handle, uint32_t hash);
typedef void (*SetBoolFn)(void *mgr, uint32_t value, const GmdHandle *handle, uint32_t hash);

enum {
    POUCH_FOOD = 6,   /* PouchCategory (dt-12345/add-material-example: Weapon, Bow, Arrow, Shield, Armor, Material, Food, ...) */
    POUCH_INDEX = 0x188, POUCH_BASE = 0x8, POUCH_SIZE = 0xc0, POUCH_HANDLE = 0x10,
    DISH_RECENT = 16, DISH_RECENT_FRAMES = 60, DISH_WAIT_FRAMES = 600,
};
/* GameData names (MurmurHash3 of Content, Name, StockNum, LifeRecover, Price, Effect, Type, Level, Time) */
#define H_CONTENT 0x4290322eu
#define H_NAME 0x25efa387u
#define H_STOCK 0x22344481u
#define H_LIFE 0x515632a9u
#define H_PRICE 0x439cc54eu
#define H_EFFECT 0xecff4f2au
#define H_TYPE 0x746585e5u
#define H_LEVEL 0x195a67f5u
#define H_TIME 0xaa1f82b8u
#define H_NEW 0xe1754905u   /* the Food pouch's flag the routine sets (its name is not known) */
/* The GameData change queues the writes may go through (dish_give.h DishStoreView), as offsets into GameDataMgr, and
   how many entries one dish pushes into each: int and int array element (LifeRecover, Level, Time, Price, StockNum),
   enum (Effect.Type), string64 (Name), bool (the flag, both pouches). */
static const struct { uint32_t offset; int pushes; } dish_stores[] = {
    {0xc8, 5}, {0x128, 5}, {0x248, 1}, {0x2a8, 1}, {0x608, 1}, {0x668, 1}, {0x68, 2},
};

static DishQueue dish_queue;   /* spawn_lock */
static _Atomic int dish_enabled;
static PouchCountFn pouch_count; static ByIndexFn by_index; static GetString64Fn get_string64; static GetStructFn get_struct;
static SetEnumFn set_enum; static SetIntFn set_int; static SetString64Fn set_string64; static GetBoolFn get_bool; static SetBoolFn set_bool;
static void **pouch_slot, **gamedata_slot;
static struct { int32_t index; uint64_t frame; } dish_recent[DISH_RECENT];   /* frame-end thread only, like the rest below */
static unsigned dish_recent_next, dish_given, dish_tries;

static uint32_t word_at(const Module *m, uint64_t at) { uint32_t w; memcpy(&w, m->base + at, 4); return w; }
/* Where a BL at `site` branches to, or ~0 when the word there is not a BL. */
static uint64_t bl_target(const Module *m, uint64_t site) {
    uint32_t w = word_at(m, site);
    if ((w & 0xfc000000u) != 0x94000000u) return ~(uint64_t)0;
    int64_t imm = (int64_t)(w & 0x3ffffff); if (imm & 0x2000000) imm -= 0x4000000;
    return site + (uint64_t)(imm * 4);
}
/* What an ADRP at `site` plus the 64-bit LDR (unsigned offset) at `ldr` on its register loads from, or ~0. */
static uint64_t adrp_ldr_target(const Module *m, uint64_t site, uint64_t ldr) {
    uint32_t a = word_at(m, site), l = word_at(m, ldr);
    if ((a & 0x9f000000u) != 0x90000000u || (l & 0xffc00000u) != 0xf9400000u || ((l >> 5) & 31) != (a & 31)) return ~(uint64_t)0;
    int64_t imm = (int64_t)(((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)); if (imm & 0x100000) imm -= 0x200000;
    return (site & ~(uint64_t)0xfff) + (uint64_t)(imm * 4096) + ((l >> 10) & 0xfff) * 8;
}

static int dish_init(Module *m) {
    /* 1.4.3: the first words of each target (several share a prologue, so the call sites below are what pins them) */
    static const struct { uint64_t at; uint32_t w[4]; } entries[] = {
        {GAME_DISH_COOK, {0xd10283ff, 0xa9047bfd, 0xa9056ffc, 0xa90667fa}},
        {GAME_DISH_POUCH_COUNT, {0xd10103ff, 0xa9017bfd, 0xa90257f6, 0xa9034ff4}},
        {GAME_DISH_STRUCT_BY_INDEX, {0x34000563, 0xf9400049, 0xb4000529, 0xf9400528}},
        {GAME_DISH_GET_STRING64, {0xd10143ff, 0xa9017bfd, 0xf90013f7, 0xa90357f6}},
        {GAME_DISH_GET_STRUCT, {0x340007e3, 0xf9400049, 0xb40007a9, 0xf9400528}},
        {GAME_DISH_SET_ENUM, {0xa9bd7bfd, 0xa90157f6, 0xa9024ff4, 0x910003fd}},
        {GAME_DISH_SET_INT, {0xd10103ff, 0xa9037bfd, 0x9100c3fd, 0x39403048}},
        {GAME_DISH_SET_STRING64, {0xa9bd7bfd, 0xa90157f6, 0xa9024ff4, 0x910003fd}},
        {GAME_DISH_GET_BOOL, {0xd10103ff, 0xa9037bfd, 0x9100c3fd, 0x39403048}},
        {GAME_DISH_SET_BOOL, {0xd10103ff, 0xa9037bfd, 0x9100c3fd, 0x39403048}},
    };
    /* the cooking routine's calls, in its order (offsets into 1.4.3's routine) */
    static const struct { uint32_t site; uint64_t target; } calls[] = {
        {0x74, GAME_DISH_POUCH_COUNT}, {0xc8, GAME_DISH_STRUCT_BY_INDEX}, {0xe4, GAME_DISH_GET_STRING64},
        {0x15c, GAME_DISH_GET_STRUCT}, {0x1a0, GAME_DISH_SET_ENUM}, {0x1f0, GAME_DISH_SET_INT},
        {0x2ec, GAME_DISH_SET_STRING64}, {0x348, GAME_DISH_GET_BOOL}, {0x368, GAME_DISH_SET_BOOL},
    };
    /* the setters' change-queue loads (dish_stores): int ldr [x0,#0xf0] / array [x0,#0x150], enum [x0,#0x270] / array
       store add x22,x0,#0x2a8, string64 [x0,#0x630] / array store add x22,x0,#0x668, bool [x0,#0x90] */
    static const struct { uint64_t at; uint32_t w; } queue_loads[] = {
        {GAME_DISH_SET_INT + 0xc8, 0xb940f009}, {GAME_DISH_SET_INT + 0x2b4, 0xb9415008},
        {GAME_DISH_SET_ENUM + 0xf8, 0xb9427009}, {GAME_DISH_SET_ENUM + 0x184, 0x910aa016},
        {GAME_DISH_SET_STRING64 + 0xc0, 0xb946300a}, {GAME_DISH_SET_STRING64 + 0x1bc, 0x9119a016},
        {GAME_DISH_SET_BOOL + 0x17c, 0xb940900d},
    };
    const char *why = NULL;
    if (strcmp(m->name, "main")) return 0;
    if (GAME_DISH_GAMEDATA_MGR + 8u > m->size || GAME_DISH_POUCH_MGR + 8u > m->size || GAME_DISH_COOK + 0x400u > m->text_end ||
        GAME_DISH_SET_INT + 0x2b8u > m->text_end || GAME_DISH_SET_STRING64 + 0x1c0u > m->text_end || GAME_DISH_SET_BOOL + 0x180u > m->text_end)
        why = "addresses outside main";
    for (size_t i = 0; !why && i < sizeof entries / sizeof *entries; i++)
        if (entries[i].at + 16 > m->text_end || memcmp(m->base + entries[i].at, entries[i].w, 16)) why = "a target's entry differs";
    for (size_t i = 0; !why && i < sizeof calls / sizeof *calls; i++)
        if (bl_target(m, GAME_DISH_COOK + calls[i].site) != calls[i].target) why = "the cooking routine's calls differ";
    for (size_t i = 0; !why && i < sizeof queue_loads / sizeof *queue_loads; i++)
        if (word_at(m, queue_loads[i].at) != queue_loads[i].w) why = "a setter's change-queue load differs";
    if (!why && adrp_ldr_target(m, GAME_DISH_COOK + 0x58, GAME_DISH_COOK + 0x5c) != GAME_DISH_POUCH_MGR) why = "the routine's PouchMgr load differs";
    if (!why) {
        uint64_t got = adrp_ldr_target(m, GAME_DISH_COOK + 0x40, GAME_DISH_COOK + 0x48);
        uintptr_t slot = 0;
        if (got + 8 <= m->size) memcpy(&slot, m->base + got, sizeof slot);
        if (got + 8 > m->size || slot != (uintptr_t)(m->base + GAME_DISH_GAMEDATA_MGR)) why = "the routine's GameDataMgr load differs";
    }
    if (why) {
        fprintf(stderr, "[dish] [red] off: %s from " TOTK_GAME_VERSION " (giving dishes refused)\n", why);
        return 0;
    }
    pouch_count = (PouchCountFn)(void *)(m->base + GAME_DISH_POUCH_COUNT);
    by_index = (ByIndexFn)(void *)(m->base + GAME_DISH_STRUCT_BY_INDEX);
    get_string64 = (GetString64Fn)(void *)(m->base + GAME_DISH_GET_STRING64);
    get_struct = (GetStructFn)(void *)(m->base + GAME_DISH_GET_STRUCT);
    set_enum = (SetEnumFn)(void *)(m->base + GAME_DISH_SET_ENUM);
    set_int = (SetIntFn)(void *)(m->base + GAME_DISH_SET_INT);
    set_string64 = (SetString64Fn)(void *)(m->base + GAME_DISH_SET_STRING64);
    get_bool = (GetBoolFn)(void *)(m->base + GAME_DISH_GET_BOOL);
    set_bool = (SetBoolFn)(void *)(m->base + GAME_DISH_SET_BOOL);
    pouch_slot = (void **)(void *)(m->base + GAME_DISH_POUCH_MGR);
    gamedata_slot = (void **)(void *)(m->base + GAME_DISH_GAMEDATA_MGR);
    atomic_store(&dish_enabled, 1);
    fprintf(stderr, "[dish] enabled: the Spawn tab's Dishes go into the Food pouch on the game's frame-end thread\n");
    return 1;
}

int runtime_dish_available(void) { return atomic_load(&dish_enabled) ? SPAWN_OK : SPAWN_ERR_DISABLED; }
int runtime_dish_give(const char *name, int life, const char *effect, int level, int seconds, int price, int count) {
    if (!atomic_load(&dish_enabled)) return SPAWN_ERR_DISABLED;
    DishRequest r;
    int e = dish_request_make(&r, name, life, effect, level, seconds, price, count);
    if (e != SPAWN_OK) return e;
    pthread_mutex_lock(&spawn_lock);
    e = dish_queue_push(&dish_queue, &r);
    pthread_mutex_unlock(&spawn_lock);
    if (e == SPAWN_OK) {   /* waiting first, then the frame-end wake-up, as for spawns */
        atomic_store(&runtime_spawn_waiting, 1);
        atomic_store(&runtime_survey_pending, 1);
        fprintf(stderr, "[dish] queued name=%s life=%d effect=%s level=%d time=%d price=%d count=%d\n", r.name, r.life,
                DISH_EFFECTS[r.effect].name, r.level, r.time, r.price, r.count);
    }
    return e;
}
static int dish_waiting(void) { return dish_queue.len != 0; }   /* spawn_lock held */

static int dish_slot_recent(int32_t index, uint64_t frame) {
    for (unsigned i = 0; i < DISH_RECENT; i++)
        if (dish_recent[i].frame && dish_recent[i].index == index && frame - dish_recent[i].frame < DISH_RECENT_FRAMES) return 1;
    return 0;
}
static DishStoreView dish_store(void *mgr, uint32_t offset) {
    const uint8_t *s = (const uint8_t *)mgr + offset;
    DishStoreView v;
    memcpy(&v.capacity, s + 40, 4); memcpy(&v.buffer, s + 48, 8); memcpy(&v.control, s + 56, 4);
    return v;
}
/* Ends the front request: tells the menu how many of its count went in, and why it stopped (NULL: all did). */
static void dish_finish(const DishRequest *r, const char *problem, uint64_t frame) {
    if (problem) fprintf(stderr, "[dish] [red] name=%s given=%u of %d: %s frame=%llu\n", r->name, dish_given, r->count, problem, (unsigned long long)frame);
    else fprintf(stderr, "[dish] name=%s given=%u frame=%llu\n", r->name, dish_given, (unsigned long long)frame);
    RuntimeSpawnObserver observer = atomic_load(&spawn_observer);
    if (observer) observer(r->name, r->count, (int)dish_given, problem);
    pthread_mutex_lock(&spawn_lock);
    dish_queue_drop(&dish_queue);
    pthread_mutex_unlock(&spawn_lock);
    dish_given = dish_tries = 0;
}

/* One dish of the front request, the way the game's cooking routine stores one (above). Frame-end thread. */
static void dish_tick(uint64_t frame) {
    DishRequest r;
    pthread_mutex_lock(&spawn_lock);
    const DishRequest *front = dish_queue_front(&dish_queue);
    if (front) r = *front;
    pthread_mutex_unlock(&spawn_lock);
    if (!front) return;
    void *mgr = *gamedata_slot, *pouch = *pouch_slot;
    const char *wait = !mgr || !pouch ? "the inventory is not loaded" : NULL;
    for (size_t i = 0; !wait && i < sizeof dish_stores / sizeof *dish_stores; i++)
        if (!dish_store_has_room(dish_store(mgr, dish_stores[i].offset), dish_stores[i].pushes)) wait = "the game's GameData change queue stayed full";
    if (wait) {   /* try again next frame, for a while */
        if (++dish_tries >= DISH_WAIT_FRAMES) dish_finish(&r, wait, frame);
        return;
    }
    uint32_t in_use; memcpy(&in_use, (const uint8_t *)pouch + POUCH_INDEX, 4);
    const GmdHandle *food = (const GmdHandle *)(const void *)((const uint8_t *)pouch + POUCH_BASE +
                                                             (in_use < 2 ? in_use * POUCH_SIZE : 0) + POUCH_FOOD * POUCH_HANDLE);
    int32_t slots = pouch_count(pouch, POUCH_FOOD), slot = -1;
    GmdHandle content;
    for (int32_t i = 0; i < slots && slot < 0; i++) {
        memset(&content, 0, sizeof content);
        if (!(by_index(mgr, &content, food, H_CONTENT, (uint32_t)i) & 1)) continue;
        const char *current = "";
        if (!(get_string64(mgr, &current, &content, H_NAME) & 1)) continue;
        if ((!current || !*current) && !dish_slot_recent(i, frame)) slot = i;
    }
    if (slot < 0) {
        dish_finish(&r, slots > 0 ? "the Food pouch is full" : "the Food pouch was not found", frame);
        return;
    }
    const DishEffect *effect = &DISH_EFFECTS[r.effect];
    GmdHandle fx; memset(&fx, 0, sizeof fx);
    if (get_struct(mgr, &fx, &content, H_EFFECT) & 1) {
        set_enum(mgr, dish_murmur3(effect->name), &fx, H_TYPE);
        if (r.effect) { set_int(mgr, r.level, &fx, H_LEVEL); set_int(mgr, r.time, &fx, H_TIME); }
    }
    set_int(mgr, r.life, &content, H_LIFE);
    const char *name = r.name;   /* copied into the change queue */
    set_string64(mgr, &name, &content, H_NAME);
    set_int(mgr, r.price, &content, H_PRICE);
    set_int(mgr, 1, &content, H_STOCK);
    /* the Food pouch's flag, as the routine sets it: pouch 0's handle is asked, both pouches' are set */
    const GmdHandle *flags[2] = {(const GmdHandle *)(const void *)((const uint8_t *)pouch + POUCH_BASE + POUCH_FOOD * POUCH_HANDLE),
                                 (const GmdHandle *)(const void *)((const uint8_t *)pouch + POUCH_BASE + POUCH_SIZE + POUCH_FOOD * POUCH_HANDLE)};
    uint8_t is_new = 1;
    if (!flags[0]->flag && (get_bool(mgr, &is_new, flags[0], H_NEW) & 1) && !is_new)
        for (int i = 0; i < 2; i++) if (!flags[i]->flag) set_bool(mgr, 1, flags[i], H_NEW);
    dish_recent[dish_recent_next].index = slot; dish_recent[dish_recent_next].frame = frame ? frame : 1;
    dish_recent_next = (dish_recent_next + 1) % DISH_RECENT;
    dish_given++; dish_tries = 0;
    fprintf(stderr, "[dish] wrote slot=%d of %d name=%s life=%d effect=%s level=%d time=%d price=%d (%u of %d) frame=%llu\n", slot, slots,
            r.name, r.life, effect->name, r.level, r.time, r.price, dish_given, r.count, (unsigned long long)frame);
    if ((int)dish_given >= r.count) dish_finish(&r, NULL, frame);
}
#endif
