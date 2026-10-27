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
   the ground do not stop him. */
int runtime_noclip_available(void);
void runtime_noclip_set(int on);
int runtime_noclip_on(void);
void runtime_noclip_set_speed(float metres_per_second);
float runtime_noclip_speed(void);
/* Teleport: noclip on, the player held at xyz (a map pin's position) from the next frame. 0 when refused. */
int runtime_noclip_goto(const float xyz[3]);
/* Set by the 1.4.3 patches in survey sessions: the benchmark camera block (world_survey.c). */
extern uint8_t *runtime_bench_cam;
