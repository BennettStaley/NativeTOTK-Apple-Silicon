// TOTK: MetalFX frame interpolation modes, shared by totk_mfx.m (Objective-C) and the pacing in hle_nvn.c (C).
// The values are the settings menu's graphics.interpolation (runtime/settings_schema.h) and the "mfx_interp" flag, so the
// first four keep their numbers: off, 30+30, On (half the refresh rate, or game 40 shown at 80 under variable refresh with
// a limit of 80), every frame (uncapped). Upstream's settings panel (2026-10-25) added 40+40 and 50+50: the game held at
// that rate (hle_nvn.c game_hold_fps), MetalFX makes the frame between; they come after, as 4 and 5. 60+60 is 6.
#pragma once
enum { MFX_OFF, MFX_30, MFX_HALF, MFX_MAX, MFX_40, MFX_50, MFX_60, MFX_NMODES };   // MFX_60: 60+60 (user, 2026-10-26: game 60 shown at 120)
static inline int mfx_hold_fps(int m) { return m == MFX_30 ? 30 : m == MFX_40 ? 40 : m == MFX_50 ? 50 : m == MFX_60 ? 60 : 0; }   // the game's rate, 0 = not held
int mfx_enabled(void);   // the mode of this game frame (0 = off)
int mfx_requested(void);   // the mode the player picked (the settings panel), taken over at the next game frame
/* GPU memory (totk_mfx.m mfx_present_tick): above MFX_BP_HIGH percent of the budget frame interpolation and per-object
   motion are paused, resumed under MFX_BP_LOW percent; after MFX_BP_MAX pauses they stay off for the session. A mode
   chosen while the use plus its estimate (mfx_need_bytes) is above MFX_BP_HIGH percent is not turned on at all. */
#define MFX_BP_HIGH 95
#define MFX_BP_LOW 85
#define MFX_BP_MAX 3
/* For the settings menu, next to the chosen mode: running as chosen, off, or not running for lack of GPU memory. */
enum { MFX_STATE_OFF, MFX_STATE_ON, MFX_STATE_PAUSED_MEMORY /* resumes on its own under MFX_BP_LOW percent */,
       MFX_STATE_OFF_MEMORY /* paused MFX_BP_MAX times: off for this session unless chosen again */,
       MFX_STATE_REFUSED_MEMORY /* chosen but never turned on: the estimate did not fit */ };
int mfx_interp_state(void);
/* A rough estimate of the GPU memory interpolation takes at an output of w x h (totk_mfx.m MFX_BYTES_PER_PX), plus the
   per-object motion arenas when TOTK_OBJ_MOTION is on; 0 x 0: the last output size it ran at (0 if never). Not a
   measurement: MetalFX does not report the memory of its interpolator. */
unsigned long long mfx_need_bytes(unsigned w, unsigned h);
