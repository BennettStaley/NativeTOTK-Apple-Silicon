/* Actor spawner (actor_spawn.c): queue an actor by name; it is created a few metres in front of the player at the next
   frame end. Return values and limits: actor_spawn_queue.h. */
#pragma once
#include "runtime.h"
#include <stdatomic.h>
#include <stdint.h>

/* 0 queued, or a negative SPAWN_ERR_* (runtime_spawn_error names it). Any thread. */
int runtime_spawn_actor(const char *name, int count);   /* at runtime_spawn_placement() */
int runtime_spawn_actor_at(const char *name, int count, int place);   /* place: SPAWN_PLACE_* (actor_spawn_queue.h) */
/* The Spawn tab's Place choice (SPAWN_PLACE_*), used by runtime_spawn_actor; the tab keeps it in spawn-picks.txt. */
void runtime_spawn_set_placement(int place);
int runtime_spawn_placement(void);
const char *runtime_spawn_error(int code);
/* "spawn <name> [count [front|above|far|feet]]" from the test input file: 1 when the line was a spawn command (handled or refused). Only test
   profiles with TOTK_SURVEY=1 (runtime_spawn_allow_commands) run it; in normal play the menu's Spawn tab is the way in. */
int runtime_spawn_command(const char *line);
void runtime_spawn_allow_commands(void);
/* 1.0.0 and 1.4.3 at startup (world_survey.c runtime_survey_init, every session): checks the call's entry and turns the spawner on.
   Returns 1 when it is on. */
int runtime_spawn_init(Module *main_module);
/* SPAWN_OK when the spawner is on, SPAWN_ERR_UNSUPPORTED on a game version without it, SPAWN_ERR_DISABLED when the
   entry check failed. */
int runtime_spawn_available(void);
/* Called on the frame-end thread after a request ran: how many of the count the game accepted, and why it stopped
   (NULL when all were). The menu shows it as a toast (settings_panel_mac.h). */
typedef void (*RuntimeSpawnObserver)(const char *name, int requested, int created, const char *problem);
void runtime_spawn_observe(RuntimeSpawnObserver fn);
/* Give cooked dishes (the Spawn tab's Dishes; actor_spawn.c, dish_give.h): count dishes named name (Item_Cook_*), each
   restoring life quarter hearts, with effect (DISH_EFFECTS name, "None" or NULL for none) at level for seconds, sold for
   price. 0 queued, or a negative SPAWN_ERR_* / DISH_ERR_* (dish_error_text). 1.4.3 on the Mac only. Any thread. The
   observer (runtime_spawn_observe) reports the result like a spawn. */
int runtime_dish_give(const char *name, int life, const char *effect, int level, int seconds, int price, int count);
int runtime_dish_available(void);   /* SPAWN_OK, SPAWN_ERR_UNSUPPORTED or SPAWN_ERR_DISABLED */
/* Nonzero while requests wait for the frame-end thread. */
extern _Atomic int runtime_spawn_waiting;
void runtime_spawn_tick(uintptr_t player_actor, uint64_t frame);
