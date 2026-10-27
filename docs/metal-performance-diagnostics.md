# Metal performance diagnostics on macOS 27

macOS 27 records frame timing and related statistics of every Metal layer all the time (`man metalperftrace`). With
`TOTK_STATE_REPORTING=1`, Tears of the Kingdom and Breath of the Wild also tell the system what they are doing, through
Apple's StateReporting framework, so Instruments and `metalperftrace` can split that recording by game state.
`games/totk/scripts/metal_perf_report.py` turns a trace into a report. Both are diagnostics: off by default; nothing is
recorded for the game, collected or sent anywhere unless you ask; and the picture, the game and its pacing are the same
with them on.

## Quick start

```sh
# 1. Play with state reports (and, optionally, per-frame data for percentiles)
open --env TOTK_STATE_REPORTING=1 --env METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS=1 "Tears of the Kingdom.app"
# 2. Quit the game, then: the last 15 minutes into a report folder
python3 games/totk/scripts/metal_perf_report.py collect --last 15m --out ~/Desktop/totk-perf \
    --app "Tears of the Kingdom.app" --note route=lookout-landing
# or a trace you collected before
python3 games/totk/scripts/metal_perf_report.py report MetalPerfTrace_....atrc --out ~/Desktop/totk-perf --app "Tears of the Kingdom.app"
```

`flag state_reporting 1` / `0` in the input file (`TOTK_INPUT_FILE`) switches the reports on and off while playing.
The folder then holds:

| File | What it is |
|---|---|
| `trace.atrc` | Apple's raw trace. Instruments opens it (`xcrun xctrace import --input trace.atrc` makes a `.trace`; the states are in the State Reporting lane). It holds the Metal statistics, names and process IDs of **every** process that drew in the time range, not only the game: look before you share it. |
| `performance.json` | The game's numbers only: the whole run, visible play, every state change and the numbers per state, a one-second timeline. |
| `performance.md` | The same, to read. |
| `metadata.json` | Per run: game and version, source commit, runtime UUID, runtime and launcher SHA-256 (when `--app` is the build that ran), the Mac and macOS the game reported, the measurement switches it ran with (values without paths); your `--note`s, the trace's SHA-256, the Mac that made the report. No user paths, keys, saves or device identifiers. |

Exit status: 0 report written; 2 usage; 3 `metalperftrace` missing (macOS 26) or failed; 4 no `totk-native` process in
the trace. A failed step is written into the report rather than skipped.

Collect a range that starts before the game does: a trace holds the state changes inside its range, plus the current
states, which the system repeats about every 90 seconds (the report marks those "began before the trace"). Quit the game
before collecting, so the last state has an end.

## The states

Two domains. Only what the port knows for certain is reported: there is no title, menu, world or loading state, because
nothing in the port can tell them apart reliably.

**`org.macnativegames.Lifecycle`**

| Label | When |
|---|---|
| `starting` | the window opened |
| `shaderWarmup (archive reused)` / `(archive rebuilt)` | the start-up shader and pipeline warm-up began and the start-up screen still shows; the pipeline archive had this Mac's pipelines, or not (a first start, a new macOS or GPU driver) |
| `focused` | the game picture shows and the window has the keyboard |
| `unfocused` | the picture shows, another app has the keyboard. The Mac never pauses the game, so it keeps running; `TOTK_ASSUME_FOCUS=1` test runs are `unfocused` throughout |
| `settings` | the settings panel (key 1) is open: the game runs, but gets no input |
| `occluded` | the window is covered or on another Space: macOS stops presenting it, so these seconds are no measure of the game |
| `closing` | the window is closing |
| `reportingOff` | `flag state_reporting 0` switched the reports off (both domains) |

Stable metadata: `game` (`totk`, `botw`), `gameVersion`, `sourceCommit` (when the app records it), `runtimeUUID` (the game
binary's LC_UUID, matched against `--app`), `shaderChain`, `hostModel`, `hostChip`, `hostOS`, and `settings` (the
switches the run was started with that change what is measured: frame rate, MetalFX, TAA, resolution, aspect ratio,
pacing and present test switches, the Metal HUD and signposts; values without paths, `defaults` when none is set).

**`org.macnativegames.Graphics`**: the configuration the renderer runs with, in one label, for example
`1920x1080 | cap 120/game interval | window@120Hz` or
`2560x1440 | MetalFX+output | TAA | FI 50+50 | held 50 | fullscreen@120Hz`:

- the game picture's size (the crop of its last frame), and the aspect ratio when it is not 16:9;
- `MetalFX` (MetalFX upscaling on), `MetalFX+output` (also the window output stage), `TAA` (edge smoothing),
  `FI 30+30` / `40+40` / `50+50` / `max` (frame interpolation, as the renderer took it over);
- the frame-rate cap: `cap N` (uncapped clock), `cap N/game interval` (the uncapped clock, divided by the game's own
  present interval: Tears of the Kingdom's frame-rate controller picks 1, 2 or 3 refreshes per frame), `lock N` (the
  settings' frame lock), `held N` (interpolation holds the game), `movie 30` (Breath of the Wild's pre-rendered movies),
  `game interval` (`TOTK_UNCAPPED=0`), `display interval` and `no cap` (`DM_PRESENT_*` test switches); `own clock` when a
  held rate does not divide the display's and the game's own clock paces it;
- `present thread` when the window is not driven by the display link, `(display link stalled)` after macOS stopped
  calling it back (a covered window) until the port tries it again;
- `window` or `fullscreen`, the refresh rate of the screen the window is on, and `(paced N Hz)` when the pacing still uses
  another display's rate (it reads the rate once, at the first frame).

The stable metadata repeats these as fields, plus the window's drawable size (`view`): a window resize shows in
Instruments as a new interval of the same state. Every state has its own label because `metalperftrace` tells states
apart by label only. A change is reported at once; further changes within half a second are merged until the
configuration has held for half a second (at most 2 s), so dragging a window larger is two reports, not hundreds.

Whether a stage actually ran depends on what is on screen (a menu has no depth to smooth, a scene as large as the canvas
needs no upscale), so it is not a state. Every state change carries the game's counters (volatile metadata):
`gameFrames` (the game's presents, as the FPS box counts them), `generatedFrames` (interpolated frames handed to the
screen), `mergedFrames` (game frames the display link merged away), `upscaledFrames` and `antiAliasedFrames` (game frames
a MetalFX upscale or edge smoothing ran on), `paceTicks1` / `2` / `3` (game frames paced at 1, 2, 3 or more display
refreshes), `shaderTranslations`, `librariesCreated`, `pipelinesCreated`, `creationMs` (shaders and Metal objects made
while playing), `footprintMiB`, `metalAllocatedMiB`, `scene` (the internal resolution at that moment), and `sampledAt`.

## Reading the report

- Three frame rates, never added or subtracted. **Output FPS**: every drawable the window showed (the trace); with frame
  interpolation each generated frame is a drawable of its own. **Game FPS**: the game's own presents between two counter
  readings. **Generated / merged**: interpolated frames handed to the screen, game frames merged away. With interpolation
  on, the report never calls the output rate a game frame rate.
- **Visible play** and the per-state **visible** columns take only whole one-second buckets while the game picture
  showed (focused, unfocused, settings): no start-up, warm-up, covered window or closing, and no second that straddles a
  change. metalperftrace's own per-state aggregate is shown next to it and covers every second of a state, whatever the
  other domain said.
- metalperftrace keeps one-second summaries (count, minimum, average, maximum, standard deviation) of on-glass frame
  time, CPU begin-to-present, GPU time, next-drawable wait and present lateness, and the process's memory and CPU time.
  **p50 / p90 / p95 / p99 need per-frame data**: start the game with `METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS=1` (or run
  `metalperftrace setup --enable per-frame-metrics --pid <pid>`). Then the report gives nearest-rank percentiles over all
  drawables, per run and per state. They describe the window's drawables (interpolated ones included), not the game's own
  frames. Without per-frame data the report says they are not available; it never estimates them.
- metalperftrace's per-drawable data has a MetalFX frame interpolator field of its own. It reads false for this port's
  interpolated frames (measured on macOS 27.2), so the report never uses it: interpolated frames are counted from the
  game's `generatedFrames`.
- Metal's shader compiler statistics need `METAL_PERFORMANCE_SHADER_COMPILER_STATS_SIGNPOSTS=1`; the game's own counters
  above are always there.
- metalperftrace aggregates whole seconds, so states shorter than a second are listed as not aggregated; it also skips
  states in which the game window showed no drawable (the start-up screen during the warm-up). It gives no reason, so
  the report says only what the trace shows.
- On the Mac the GPU keep-alive (`TOTK_GPU_KEEPALIVE`) runs short kernels on its own queue between the game's command
  buffers; the trace's GPU time is the game layer's, the system's GPU utilisation includes the keep-alive.
- The source commit is what the app recorded at build time (uncommitted changes do not show); the runtime UUID and
  SHA-256 identify the build.

## Platforms

- macOS 27: StateReporting is opened at run time (`dlopen`, no `-framework`), so the app builds with any SDK.
- macOS 26: an app built there has no StateReporting to open; with `TOTK_STATE_REPORTING=1` the log says so once and the
  game runs as without it. `metalperftrace` does not exist there either: the report command stops with status 3 and says
  why. (An app built with the macOS 27 SDK needs macOS 27 anyway.)
- iPhone, iPad, Vision Pro: not built in (`TARGET_OS_OSX` only); nothing changes. `flag state_reporting 1` logs that it is
  Mac only.

## Overhead

Off (the default), each hook reads one atomic integer: no queue, no framework, no Objective-C. On, the game thread
compares a snapshot of 17 integers once per frame; every StateReporting call runs on a queue of its own; no render pass,
GPU wait or lock on the frame path is added (the snapshot is published under a mutex only when it changed). Timed on its
own on an M5 Pro, the game thread's hook costs about 2 ns a frame with the reports on, also with a change reported every
10,000 frames. In the games, with the repository's method (one binary, the switch toggled in alternating
30-second windows), the difference stayed within the window-to-window spread; the numbers are in the pull request.
