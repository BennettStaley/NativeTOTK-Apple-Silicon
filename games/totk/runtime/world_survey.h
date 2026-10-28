#pragma once
#include "runtime.h"
#include <stdatomic.h>

/* Test-profile-only control, executed on the game's frame-end thread after its GPU fence. */
extern _Atomic int runtime_survey_pending;
extern const int32_t runtime_survey_sync[2];
void runtime_survey_init(Module *main_module);
int runtime_survey_command(const char *line);
void runtime_survey_tick(uint64_t frame);
void runtime_survey_cancel_user(void);
/* Noclip (the Cheats tab, Mac; 1.0.0 and 1.4.3): while on, the player is held each frame at a point the left stick moves
   relative to the camera and the triggers (ZR / RT up, ZL / LT down) raise and lower, speed in metres a second; walls and
   the ground do not stop him. L / LB and R / RB step the speed down and up. On a controller, L3 + d-pad down held for half
   a second turns it on or off (gamepad.m). */
int runtime_noclip_available(void);
void runtime_noclip_set(int on);
int runtime_noclip_on(void);
void runtime_noclip_set_speed(float metres_per_second);
float runtime_noclip_speed(void);
enum { NOCLIP_NSPEEDS = 5 };
extern const int runtime_noclip_speeds[NOCLIP_NSPEEDS];   /* the speeds the Cheats tab and L / R step through, m/s */
void runtime_noclip_step_speed(int dir);                  /* to the next speed up (1) or down (-1) */
/* The npad buttons noclip keeps from the game while on (input.c): the triggers and the shoulder buttons, so ZR does not
   draw the bow (and slow time in the air) and ZL does not raise the shield while flying. 0 while off. */
uint64_t runtime_noclip_taken(void);
/* Teleport: noclip on, the player held at xyz (a map pin's position) from the next frame. 0 when refused. */
int runtime_noclip_goto(const float xyz[3]);
/* Set by the 1.4.3 patches in survey sessions: the benchmark camera block (world_survey.c). */
extern uint8_t *runtime_bench_cam;
/* Auto shrine (Tools › Shrines, TOTK; world_survey.c, shrine_auto.h): completes shrines one after another, the nearest
   first, by flying Link with noclip and pressing A where a player would; the game does the rest. */
int runtime_autoshrine_available(void);
int runtime_autoshrine_start(int one);              /* one: only the nearest shrine; 0 when refused (no noclip, running) */
int runtime_autoshrine_finish(int shrine);         /* only that shrine (DungeonNNN's number), from wherever Link is; 0 when refused */
void runtime_autoshrine_stop(void);
void runtime_autoshrine_set_chests(int on);         /* open each shrine's chests on the way (off at every start) */
int runtime_autoshrine_chests(void);
int runtime_autoshrine_status(char *text, size_t size);   /* the status line; 1 while a run is on */
int runtime_autoshrine_command(const char *line);   /* the input file's "autoshrine start | one | finish N | stop | status | chests on|off" */
void runtime_autoshrine_romfs(const char *path);    /* romfs.c: each file the game opens, relative to the RomFS */
