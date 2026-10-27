# The settings menu

The game has its own settings menu, the same settings on Mac, iPad and iPhone. Changes apply while you play: the picture
changes within a frame or two, except the few settings marked *after restart*. Settings are saved to `settings.txt`.

## Opening it

| | |
|---|---|
| Mac | **⌘,**, **F1**, the app menu's **Settings…**, **L3 + R3** (both stick clicks) or **Options and Menu** (View and Menu on Xbox) held together for a moment, or a controller's **Home** button (on macOS 26 the Xbox button opens the system's Games overlay instead) |
| iPad / iPhone | the **gear** at the top of the screen, a **three-finger tap**, a controller's **Home** button, **− and +** held together, or **Esc** / **⌘,** on a keyboard |

On the Mac the menu is a sidebar inside the game window, so it works in full screen and the picture stays in view beside
it while you change things. While the menu is open the game receives no input.

The menu has four tabs, each a bar of sections:

| Tab | Sections |
|---|---|
| **Game** | Display · Frame rate (with frame interpolation) · Controls · Audio · Interface |
| **Graphics** | Image (preset, anti-aliasing, sharpening, anisotropic filtering, texture detail) · Effects (bloom, depth of field, lens flare, cave fog) · World detail (field of view, render distance, shadow resolution, dynamic resolution) |
| **Content** | Mods · Saves (Send saves) · Downloads (shader list, cheat list) |
| **Tools** | Cheats · Spawn · World (time of day) · Camera (hide the HUD) |

Navigation has two levels. On the **section bar**, ← → (D-pad) choose a section and ↓, Return or A opens it; in a
section ↑ ↓ choose (the list stops at its ends), ← → change, Page Up / Down or LT / RT page. Esc or B goes back one level
and closes the menu only from the section bar. Q / E, Tab or LB / RB switch tabs. **Reset this tab** (R, or X on the
section bar) and **Reset everything…** (Shift-R, or Y on the section bar, then Return / A to confirm) set settings back
to their defaults; cheats, mods, saves and spawn favorites are never reset. On iPhone and iPad the same sections are one
list ("Game · Display" …). Setting keys did not change, so `settings.txt` files keep working.

**Search** (Mac): / or ⌘F, or the field at the top, lists every setting whose name, description or section has all the
words typed, with where it lives; ↓ or Return goes into the results, Esc clears them. Each row shows one line of its
description; the focused row shows all of it. Settings that take effect at the next start say **↻ restart** before
you change them.

**Graphics preset** (Graphics › Image): *Battery saver* (67 % render scale, 30 fps), *Balanced* (the game's own look,
the default) and *Quality* (100 % render scale, SMAA, 20 % sharpening, 16x filtering, 2x shadows on the Mac, 150 %
render distance) set those settings together. Changing one of them by hand shows *Custom*.

A setting marked *(after restart)*, a change in the Mods tab, or a code cheat switched in the Cheats tab shows **Restart now** at the bottom of the menu (and on
the Mods tab's banner): the game closes the usual way (a save in progress finishes first) and the app opens again. Mac only;
on iPhone and iPad close the app from the app switcher and open it again.

## Display

| Setting | What it does |
|---|---|
| **Display resolution** | The finished picture, its text and icons included: *Match the window*, *Display native* (your screen's panel pixels, 3456 wide on a 16" MacBook Pro, whatever scaling macOS uses), 4K, 1440p, 1080p (the game's own), 900p, 720p or a *Custom width* (height at 16:9). Live, up to the maximum output. Mac only: iPhone and iPad show 720p. |
| **Render resolution** | The size the 3D world is drawn at, as a share of the display resolution (each choice shows the size it gives): *The game's own* (about 83 %), or 25–200 %. 50 % of a 3456-wide output renders 1728×972; above 100 % renders more pixels than the output (supersampling). The scene is capped at 2560×1440 (see below). |
| **Aspect ratio** | *16:9* (the game's own) or *Fill the screen*, 16:10, 21:9, 32:9. A wider shape shows more at the sides; a taller one (16:10, a MacBook in full screen) keeps the game's view across and shows more above and below. Nothing is stretched: the HUD keeps its shape in the middle 16:9 of the screen. Mac only. |
| **Maximum output** | The biggest output you can switch to while playing; a bigger one sets more memory aside. *Auto* is 4K on a Mac with 16 GB or more, else 1440p. Applies after a restart. |

How the two fit together: the **display resolution** is the finished picture, and the game's text, icons and menus are drawn at it,
so they are always sharp at that size. The **3D world** is drawn separately at the **render resolution**, a share of it, and then
stretched to fill the picture. A 4K display resolution with a 50 % render resolution draws the world at 1920×1080 behind 4K text: faster, a
softer world, crisp HUD. The line at the top of the menu shows both sizes as they are now
(`Display 3840×2160 · Render 2560×1432 (1440p, the most it can be)`). The 3D world stops at 1440p (see below), so at a
4K display resolution anything from 67 % up draws it at 1440p.

**Maximum render resolution** (Display, after a restart): 1440p, 1800p or 4K, the biggest the 3D world can be drawn
at; it needs the 4K maximum output. Above 1440p the game's texture heap passes 1 GiB, so memory pools may reach 2 GiB
and the GPU memory budget becomes 40 % of the Mac's memory. Measured on an 18 GB M3 Pro (2026-10-21): 1800p at a 4K
display resolution held 60 fps and used 5.4–6.1 GB of GPU memory against a 7.2 GB budget; 4K needs about 1.5 GB more.
The menu's second line shows **GPU memory** against that budget, orange above 80 % and red above 90 %: past it the game
stops on purpose instead of risking a system crash.

**Options that would exceed GPU memory** (Mac). The menu does not show a choice as on when it is not running:

- *Frame interpolation* pauses itself above 95 % of the budget (per-object motion with it, when `TOTK_OBJ_MOTION=1`)
  and resumes once the use plus what the pause freed is below 85 %; after 3 pauses it stays off for the session. A mode
  chosen while the use plus its estimate is above 95 % is not turned on at all (it turns on by itself once that is below
  85 %). The row keeps the chosen mode and says why it is not running, orange or red like the meter:
  `Half the refresh rate — paused: not enough GPU memory (6.9 of 7.2 GB)`, `— not applied: not enough GPU memory`,
  `— off for this session: not enough GPU memory`. Choosing Off and then a mode tries again.
- In the selector, a choice that would clearly not fit is marked *(not enough GPU memory)* and stays selectable. For
  frame interpolation (while it is not running): the use now plus about 56 bytes per display-resolution pixel (two scene
  copies, the UI layer, three outputs, the motion texture and an assumed two RGBA16F textures of MetalFX history, which
  Metal does not report; about 0.45 GB at 4K, 0.11 GB at 1080p), plus 192 MB of motion arenas with per-object motion,
  above 95 % of the budget. For the maximum render resolution: the use measured above (1800p 6.1 GB, 4K 7.6 GB) above
  the budget; a chosen one that does not fit says *may not fit* under the row. These are rough estimates from the
  textures made and one Mac's measurements, not a promise either way; the meter and the pause are what count.
- Other overruns of the budget still stop the game.

Limits and why:

- The 3D scene stops at 2560×1440. The game keeps its render targets in one fixed texture heap sized at start; a 4K
  scene would need a 1.5 GB heap, above the 1 GB a single zero-copy GPU buffer may take here.
- An output change rebuilds the game's canvas, and for a moment both sizes are held. The menu therefore applies a new
  output once the choice has stayed the same for 0.4 s, so clicking through several sizes rebuilds once.
- The runtime stops the game if GPU memory passes its budget (5000 MB, 6500 MB with the 4K maximum output; never above
  40 % of the Mac's memory): earlier kernel panics on this hardware happened near 15 GB of resident memory.

## Graphics

| Setting | What it does |
|---|---|
| **Frame interpolation** | MetalFX makes a frame between two game frames: off, 30 + 30, half the refresh rate, every frame (Mac and iPad). It needs GPU memory, and on the Mac it gives way before the game would stop (see *Options that would exceed GPU memory* below): the row then says *paused*, *not applied* or *off for this session* next to the chosen mode. |
| **Motion / depth debug view** | The motion and depth taken from the game's render, drawn over the picture: *Motion* (the motion vectors: direction as colour, brighter is faster, full brightness at 32 pixels per frame; with `TOTK_OBJ_MOTION=1` the pixels that took per-object motion are tinted lighter and outlined in white), *Depth* (the G-buffer depth, linearised, near is white, 4000 units and the sky black) or *Split* (picture, motion and depth in thirds). Works with frame interpolation on or off. With it on, this is exactly what MetalFX is given, drawn over real and interpolated frames alike (motion in pixels of the display resolution). With it off, the G-buffer depth and camera are still captured and the motion kernel runs, but no interpolator or output textures are made: about 8 bytes per render pixel (12 MB at 1600x900), more with `TOTK_OBJ_MOTION=1` (its target and position arenas), all released when the view is turned off. The view is drawn over the game's frame there, with motion in pixels of the render resolution, and is not drawn above 95% of the GPU memory limit (back under 85%). Off by default; it costs nothing while off. Frames without motion (first frame, camera cuts) show the plain picture. Settings key `graphics.mfx_debug_view` (unchanged). `TOTK_MFX_DEBUG_VIEW=motion\|depth\|split` for scripts; `flag mfx_debug_view 0..3` in an input file. Mac and iPad. |
| **Field of view** | 70–140 % of the game's camera angle. Aiming, first person and cutscenes keep their own changes, scaled. |
| **Cave fog** | 0–100 % of the game's fog in caves and other enclosed places (the fog ramp the Env update writes from the area's palette): 50 % reaches it at twice the distance, 0 % has none. Outdoors on Great Sky Island it made no visible difference: the outdoor haze comes from the sky / atmosphere and is not adjustable yet. Version 1.4.3. |
| **Shadow resolution** | The sun's cascaded shadow map: *the game's own* (the game picks the side every frame: 400 to 800 per cascade docked, 800 on Great Sky Island), *2x* or *4x* that for sharper shadow edges, capped at 2048 (so 4x of 800 is 2048); the game's other views grow too. 4x adds about 190 MB of depth maps in all. *0.5x* and *0.25x* are below the game's own quality: half or a quarter of the side (0.25x of 800 is 200), blurrier shadows that fade out on the ground. Measured 2026-10-23 (M3 Pro, 1440p, Great Sky Island): the four shadow passes cost 0.51 ms a frame at 800 and at 200 alike (about 1250 draws, bound by geometry, not by the map size), so they save no GPU time. Mac only; changes while you play (the next frame). Version 1.4.3. |
| **Anti-aliasing** | The game's own FXAA, off, FXAA or SMAA, on the 3D scene before the game draws its text and icons. |
| **Sharpening** | AMD FidelityFX CAS on the finished picture, under the text and icons. |
| **Anisotropic filtering** | The game's, or 2–16×: sharper ground and walls seen at an angle. |
| **Texture detail bias** | Below 0 picks sharper texture levels; too low shimmers. |
| **Depth of field** | On (the game's) or off. The game blurs the distance with one full-screen pass over the scene (it reads the depth and blends a blurred half-size copy of the picture over everything far away); off leaves that pass out, so far away stays as sharp as it was drawn. Version 1.4.3. |
| **Bloom** | 0–100 % of the game's glow around bright light. The tone map adds the bloom texture to the scene; below 100 % that texture is scaled first, so 0 % is no bloom at all. Version 1.4.3. |
| **Lens flare** | On (the game's) or off. With the sun in view the game draws about a dozen rings, ghosts and a glint over the scene before its tone map; off leaves those draws out (the sun, its glow and bloom stay). Live. Version 1.4.3. |
| **Render distance** | 10–100 % in 10 % steps of the camera's far clip distance (the game's own is 100 %, the most: above it, offered before, the distant land and sky islands turned black along the horizon seen from high in the sky, and a saved value above 100 now loads as 100). Below 50 % is below the game's own quality (land cut off within a few km; 10 % is 2.5 km), there to measure what drawing less of the far distance saves: at 10 % on Great Sky Island (M3 Pro, 1440p, one run of three profiled frames each) the frame's GPU time went from about 9.8 to 8.5 ms, mostly in the lighting passes after the G-buffer; the shadow passes did not change. The game's own is 25 km, more than the map is across, so below 100 % land in the far distance is cut off sooner; more would not load more of the world or raise the detail of distant land, which the game decides separately. |
| **Dynamic resolution** | *On* (the game's own) or *Off*. The game measures its frame times and, when frames run long, draws the 3D world at 90 % or 80 % of the render resolution; *Off* holds every step at 100 % (`TOTK_FIXED_RES=1`: one instruction of the game's resolution controller at `main+0x5848a8`), sharper in busy places at the cost of frame rate there. The render resolution you chose stays. Applies after a restart. Mac only (iPhone and iPad run code fixed at build). Version 1.4.3. |

## Frame rate, interface, controls, audio

- **Frame rate limit**: *Auto (steady)*, 30, 40, 60, 120 fps or the display's refresh rate. When the display paces the
  game (a 120 Hz display) the limit holds each frame for whole refreshes (30 fps = 4 refreshes at 120 Hz); 40 needs a
  120 Hz display.
  *Auto (steady)* (`framerate.cap = auto`, `TOTK_UNCAPPED_HZ=auto`; the Mac default) picks the highest rate that divides
  the display's and that the game holds: 120 / 60 / 40 / 30 at 120 Hz, 60 / 30 at 60 Hz. A frame rate between those
  steps judders: at about 98 fps on a 120 Hz display one frame in five stays up for two refreshes, so motion alternates
  between 8.3 and 16.7 ms. Auto measures the game's own frame time without the limit's wait (the newest 2.5 s, leaving
  out shader builds, loading and light title / loading frames, and single frames over 100 ms). It goes down as soon as
  more than 1 frame in 10 misses the current rate, to the rate the 95th percentile fits with 8 % to spare. It goes up
  after 10 s in which the 95th percentile stayed under 85 % of the faster rate's frame time, or tries the faster rate
  after 20 s at a steady rate (a frame time measured at a lower rate misleads both ways: the GPU overlaps our wait,
  and it clocks down when idle); a try that fails within 5 s is remembered, and that rate is not tried again for
  5 minutes unless the scene gets lighter. Frames after a stall of 100 ms or more and after a size change are left
  out. Each change is logged (`[auto-cap]`). With frame interpolation at *30 + 30* or *Half the refresh rate* the interpolation paces the game and
  Auto steps aside (it resumes at its last choice when interpolation is turned off); with *Every frame* it limits the
  game frames as a fixed limit would. On a display the game does not wait for (the uncapped mode below 120 Hz) Auto
  moves the uncapped clock's ceiling instead (60 Hz: 60 or 30). The iPhone / iPad default stays the display rate.
  In full screen on a display with variable refresh (below) Auto is not held to those steps: it aims at a continuous
  rate, the one the window's 95th percentile frame time fits with 5 % to spare, in 5 fps steps between the display's
  slowest rate (at least 30) and its fastest, and moves the uncapped clock there (frame times of 12.4-14.8 ms on the
  M3 Pro give about 65-75 fps instead of 60). Down at once when more than 1 frame in 10 misses it, up after 3 s in which
  the 95th percentile fits a rate at least one step faster; a failed rise backs off as above. Only a display that
  reports an interval granularity (NSScreen `displayUpdateGranularity` > 0) has the rate rounded to its steps. Logged
  as `[auto-cap] variable refresh: ...`. Fixed limits (60, 120, ...) are presented the same way at that rate.
- **Variable refresh (full screen)** (`framerate.vrr`, `TOTK_VRR=auto|0|1`; Mac only, default *Auto*). On Apple silicon
  with a variable-refresh display (the built-in ProMotion display of a 14" / 16" MacBook Pro, an Adaptive-Sync monitor)
  and the game in full screen, the display refreshes when each frame is ready instead of on its fixed 120 Hz grid, so
  a frame rate like 77 fps is shown at an even 13 ms instead of alternating 8.33 / 16.67 ms. The game presents
  without the display link (whose frame rate range snaps to divisors of the maximum refresh) and holds each frame for
  at least 1 / target fps (`presentAfterMinimumDuration`), as Apple describes in WWDC21 session 10147
  ([Optimize for variable refresh rate displays](https://developer.apple.com/videos/play/wwdc2021/10147/)) and as
  OpenEmu does (GameWindowController.swift / OpenEmuHelperApp.swift). A display supports it when NSScreen's minimum and
  maximum refresh intervals differ. *Auto* enters full screen when it is turned on and at launch on such a display;
  leaving full screen (green button, Ctrl-Cmd-F) is respected and the game simply stays on the fixed grid. The row
  says whether it is active now or why not: windowed, a fixed-refresh display, or frame interpolation on (MetalFX
  interpolation keeps the fixed-refresh timing it was built for). `TOTK_VRR=1` also tries it in a window (macOS only
  honours it in full screen; for tests). The log says `[vrr] active: display min 8.33 ms max 41.67 ms granularity
  0.00 ms; target 75.0 fps` at each change, and every 600 pictures `on-screen pacing, variable refresh: ...` counts
  the intervals within 1 ms of the target against those on the 120 Hz grid.
- **Performance readout**, and its **Readout detail**: *Simple* (`FPS 60  2560x1440`) or *Detailed*, which adds the
  average and the 1 % low over the last 5 seconds (`FPS 60  avg 59  1% low 48  2560x1440`). The 1 % low is the frame
  rate of the slowest 1 % of the game's frames in that window, so a single long hitch shows there while the average
  barely moves.
- **Hide the HUD**: hides everything the game draws over the picture (hearts, map, prompts, quest text) for clean
  screenshots; **F2** on a Mac keyboard toggles it and the setting follows. It hides the game's menus too (the pause
  menu and the inventory are drawn the same way as the HUD: with it on, the pause menu is a black screen), so turn it
  off to use them. The performance readout is the port's own and stays.
- **Shader counter** (after restart), and on iPhone / iPad the **touch controls** (when they show, opacity, size) and
  the **settings button**.
- **Button prompts**: the button icons the game shows: *Automatic* (the last controller you connected: Xbox icons for an
  Xbox controller, PlayStation icons for a DualSense or DualShock, the game's own otherwise), *Switch*, *Xbox* or
  *PlayStation*. Xbox and PlayStation use a button-icon mod installed from the Mods tab (for Xbox: "Xbox UI for
  v1.4.3" on GameBanana); the setting turns exactly one of them on at start and the others off, so going back to the
  game's own icons is one choice, not uninstalling. Applies after a restart (the game loads its icons once at boot).
- **Camera speed, horizontal / vertical**: 25–300 % of the right stick's left-right and up-down push (controller and
  arrow keys alike), each axis on its own. The push is scaled before the game reads it and stays within the stick's
  range, so above 100 % a partial push turns the camera like a fuller one and the game's top speed (stick all the way,
  or an arrow key) stays the game's; below 100 % every push, the arrow keys included, turns it slower.
- **Face buttons** (PlayStation and Xbox by position or by label), **rumble** and **motion controls** (Mac).
- **Volume**.

## Credits

Several settings follow features first offered on Switch and emulators by MaxLastBreath's UltraCam
(https://gamebanana.com/mods/480138) and NX Optimizer (https://github.com/MaxLastBreath/nx-optimizer). This port
reimplements them independently for 1.4.3 and contains none of their code.

Not here yet: NX Optimizer's switches for ambient occlusion, volumetric fog, screen-space reflections, global
illumination, the grass layers, far lights, forced level of detail, *Improve LOD* and *Remove QuaReduction*. In NX
Optimizer they are settings of UltraCam (its `Shaders` and `Graphics` sections) or older code patches it downloads,
so its files hold no game addresses or shader identities; each needs its 1.4.3 code site or render pass found first,
the way depth of field and lens flare were (by their shaders in the draw log).
## World

| Setting | What it does |
|---|---|
| **Time of day speed** | *Paused*, 0.25×, 0.5×, *Normal* (the game's own), 2×, 4× or 8×: how fast the day passes, the clock, the sun, the sky and the shadows with it. *Paused* stops the time of day where it is. Events that set the time (sleeping, cutscenes) still do, and where the game holds the clock itself (the opening beneath Hyrule Castle stays at 7:00) the setting has nothing to speed up. |

## Spawn

Tools › Spawn creates actors next to you, by calling the game's own `ActorMgr::requestCreateActorAsync` on its
frame-end thread (no game code is patched). **Versions 1.0.0 and 1.4.3**; elsewhere the tab says "Spawning needs game
version 1.0.0 or 1.4.3 for now" (1.2.1 needs its main checked first). Mac, iPad and iPhone.

- **Categories**: Favorites, Recents, Creatures, NPCs, Items, Weapons, Armor, Zonai, Objects, from the actor list the build
  makes from your own game files (`build/actor_list.tsv`, `scripts/prepare_actor_list.py`). System actors are left out;
  actors without a found name are hidden unless **Show unnamed** (U, X) is on. Each row shows the name and, dimmed, the
  internal name.
- **Search** (/ or ⌘F) looks through every category while a query is typed.
- **Amount** 1–20 (← →, − +). A, Return or a click spawns; Y or F marks a favorite. Every spawn goes to Recents.
  Favorites and recents are kept in `spawn-picks.txt` beside the profile (next to `cheats-enabled.txt`).
- **Place**, the popup next to Amount: **In front** (3 m ahead, the default), **Above** (8 m up, so things drop in),
  **Far** (15 m ahead) or **At Link's feet**. P, or ← on the category column with a controller, picks the next one; on
  iPhone and iPad it is a menu in the bar. It is kept in `spawn-picks.txt` (`place above`).
- There is no Undo yet: the game's delete call has not been matched to 1.4.3.
- A toast in the window's corner says "Spawned Apple ×5" or why it failed. A spawn asked for during loading waits for the
  world. Test runs: the input file's `spawn <name> [count [front|above|far|feet]]` works only with `TOTK_SURVEY=1` on a test profile.

## Cheats

Mac only for now (iPhone and iPad cannot change the game's code while it runs; their menu has no Cheats section). The
last tab starts with the **cheat list**: the community cheats for the running game version, ready to switch on without
any files. Below it come the Atmosphère cheat files of your own (the format Atmosphère, Yuzu and Ryujinx use).

### The cheat list

No cheat codes are in this repository or in the app: the codes are their authors', collected without a license, so the
game downloads them. It fetches [sthetix/nx-cheats-db](https://github.com/sthetix/nx-cheats-db)'s file for the game
(`cheats/0100F2C0115B6000.json`, every version's cheats) and keeps the running version's part as
`~/Library/Application Support/MacNativeGames/TOTK/cheats-db/<build ID>.txt` (`TOTK_CHEATS_DB_DIR` overrides, `0` = none),
which is read first at every start (`runtime/cheat_db_fetch.h`, `runtime/cheat_db.h`). Thanks to sthetix for the
database and to the cheat authors it credits.

- **When.** 20 s after the game opens, in the background, at most once a day (an unchanged database answers HTTP 304),
  and when the Cheats tab is opened after the day is over. **Update** in the tab's **Cheat list** row checks now; the row
  says how many cheats there are, when the list was checked and changed, and why the last check failed. Offline, the
  cached list keeps working.
- **Setting.** Settings › Content › Downloads › *Download cheat list* (also in the Cheats tab; on by default; live). Off: nothing
  is fetched, the cached list stays. Download only: the game never uploads anything.
- **Check.** The download is untrusted: strict JSON, 16 MB at most, https only (redirects too), 20 s timeout; anything
  else keeps the previous list. The file is rewritten only when this version's cheats changed.
- **New list.** The first list that arrives while the game runs is read at once: its live cheats work right away, its
  code patches from the next start. A newer list later replaces the file and is read at the next start (**Restart to
  apply** says so).

The list is shown through our own catalog of the 1.4.3 cheats (`runtime/cheat_catalog.h`: names, groups and a line
about each, never codes): Player, Combat, Movement, Inventory, World, Map, and Other for entries the catalog does not
know yet (their numbering, `№13.` or `#01.`, taken off). Left out of the list:

- titles, notes, group markers and master codes (masters still run together with the cheats you choose);
- helpers that only undo other cheats (*Restore Code*, *ReEnable Fog*, *Time Resume*): switching a code patch off and
  restarting already restores the game's code;
- resolution, frame rate and picture cheats (FPS, resolution, RRS, contrast, saturation): the Display settings do that,
  and the two would fight;
- repeats: the same words, the same name or the same catalog entry again (1.4.3's *Show all Miskos treasure* has the
  words of *Show all Great Fairy*, so only the second is listed).

Variants of one cheat are one row with a menu instead of switches that conflict: *Damage multiplier* (Off, ×2, ×4, ×10,
×30), *Infinite stamina* (Off, On, the Master Sword quest version), *Default pace* (Off, Running, Walking). Choosing one
switches the others off. Cheats the port cannot run are folded into one row at the end, **N not available here**;
**Show** lists them with the reason (on 1.4.3: the paraglider and movement speed variants, the time of day cheats and
*60 more seconds*). Your choices are saved under the cheat's name without its numbering (`db<TAB>Inf. Health`), so they
survive the database renumbering its entries.

### Your own cheat files

A file is read only when its name is the running game's
build ID, in any case: 1.0.0 `082CE09B06E33A12`, 1.4.3 `277178B7DBA1B6D4` (both read from the game's own files), 1.2.1 `9B4E43650501A4D4` and 1.1.2 `9A10ED9435C06733` (from the cheat databases, not checked). The places, each file a group of its own:

1. the file built into the app with `build_app.py --cheats`;
2. the cheats folder, `~/Library/Application Support/MacNativeGames/TOTK/cheats/<build ID>.txt` (**Open** shows it in
   Finder; `TOTK_CHEATS_DIR` overrides, `0` = none);
3. each mod that is on, in the load order: `<mod>/cheats/<build ID>.txt` (the Yuzu / Ryujinx layout; Atmosphère's
   `atmosphere/contents/<title ID>/cheats/` inside a mod is found too). See [mods](mods.md).

Files are read at the start: a file added or changed while the game runs shows **Restart to apply**.

Each cheat (of the list or a file) has a switch (mouse, Return / A, or ← off and → on; a choice steps through its menu)
and a line that says how it applies:

| | |
|---|---|
| *Live* | changes values in memory about 30 times a second; on and off at once |
| *Code patch* | writes the game's code, which is only possible before the game starts: *applies after restart* / *turns off after restart* until then |
| *Can't be used here* | refused, with the reason (it would overwrite code this port rewrites, jump out of the game's code, or patch code only while a button is held) |

How a cheat is split: the words it writes into the game's code (and into its read-only data, which is sealed with the
code) are written once, at the start, before the game runs; everything else in the same cheat (pointer chains into the
game's memory, button conditions, values in its data) runs live on the cheat thread. A cheat with both parts shows
*Code patch* and needs a restart to switch; once applied, the live part only compares the code words, it never writes
them again. Code or read-only data can therefore not be written under a condition, in a loop, or only while a button is
held: those cheats are refused. A cheat that writes the game's ordinary data or heap is *Live*.

Reasons you may see under *Can't be used here*:

| | |
|---|---|
| *overlaps the game runtime's own patch at main+…* | the port already patches that instruction (frame rate, time of day, aspect ratio and so on) |
| *code (or read-only data) patch … is under a condition or in a loop* | a button-held or conditional code patch, see above |
| *patches main+…, an instruction the Apple-silicon rewrite changed* | the port runs a different instruction there, so the cheat's word would not do what it says |
| *branches out of the game's code*, *into an instruction window the Apple-silicon rewrite moved*, *into unused padding* | a code cave or jump the port cannot follow |
| *writes part of a word …*, *stores a register into the game's code …*, *refers to main+…, outside the game's module* | instructions the code-patch path does not support |
| *this build has no map of the instructions the Apple-silicon rewrite changed* | the app was built without the map the checks need (`prepare_runtime.py` makes it); every code patch is refused rather than applied blind |
| *conflicts with "…"; turn one off* | two cheats that are on write different values to the same code word (shown on the second; the first one wins) |

A section that does not parse (a malformed line, a bad instruction) is refused on its own with the reason; the other
cheats of the file still load. A file that cannot be read at all (larger than 1 MB, not a cheat file) shows its reason
under the file's heading. At most 1024 cheats are read from all files together; the rest of a file is left out with a
note. Two cheats with the same name in one file are kept apart as `name (#2)`, `name (#3)`, which is also how they are
saved in `cheats-enabled.txt`.

Master codes (`{...}` sections) are always on with their file and shown without a switch. The cheat databases' group
sections (one `20000000` or `20000001` word, opening and closing a group) are shown as headings in your own files. **Turn off all cheats** switches every cheat off (code patches at
the next start). The choices are saved in `cheats-enabled.txt` next to the profile, one line per cheat: the source and
the name (`db<TAB>[name]` for the cheat list, `folder<TAB>[name]`, `mod:<folder><TAB>[name]`; the app's own file just the name, as before), so equal names
in two files are separate cheats. **Reset this tab** and **Reset everything…** leave cheats alone. The first cheat turned on copies the profile to
`profile-before-cheats` first.

## Known issues

- With *Aspect ratio* filling a screen that is not 16:9, the game's pause menu, inventory and map (drawn at 16:9, outside
  the extended view) sometimes flash for a frame while opening or closing. Seen on a Mac (2026-10-21), frame
  interpolation off. Ruled out from that session's log: the canvas the UI is squeezed in changes only when the output
  resolution changes, not with menus, and the aspect ratio and its field-of-view factor are set once, not per frame.
  Suspects, not yet confirmed in game: (1) a UI quad drawn before the frame's first main UI shader is left unsqueezed
  (`nvn_mtl.m` `uw_ui_squeeze`; the log now says `UI quad ... before the UI began`, and `TOTK_UI_SQUEEZE_EARLY=1`
  squeezes those too); (2) a full-screen fade or dimming quad of the menu is squeezed into the middle 16:9 like the
  rest of the UI, so the bands outside it miss the fade for those frames.

## The file

`settings.txt` — Mac: `~/Library/Application Support/MacNativeGames/TOTK/settings.txt`; iPhone / iPad: the app's
Documents folder (Files › Tears of the Kingdom). One `key = value` line per setting, `#` starts a comment; unknown keys
and bad values are ignored, so a file from a newer build still starts an older one. An environment variable that is
already set wins over the file (`open --env TOTK_FOV=120 "Tears of the Kingdom.app"`), so a test never has to touch the
player's settings. The table of settings is `games/totk/runtime/settings_schema.h`, shared by the Mac launcher, the
runtime, the iOS app and `tests/test_settings.c`.

## For testing

Test runs drive the menu's own code path through `TOTK_INPUT_FILE`: `set <key> <value>` changes a setting as the menu
does (`set display.scale 50`, `set graphics.aa smaa`), `menu` opens or closes the Mac menu, `menukey <Mac key code>`
presses a key in it (`menukey 12`, Q, from the first tab opens the last one, Tools), and `settings-shot <png>` saves the Mac menu as a picture.

### Frame rate tab (2026-10-23 layout)
In order: **Frame rate limit** (Auto fastest steady, 30, 40, 60, 80 with variable refresh, 120, Unlimited), **Variable refresh (full screen)** (Auto / Off; the row says whether it is active and at what rate), **Frame interpolation** (Off, On, 30 + 30, Every frame), **Per-object motion** (with interpolation; restart, recompiles shaders once) and **Motion / depth debug view**. With variable refresh and interpolation On, the game runs at half the shown rate on the display's steps: on the built-in XDR (4.17 ms steps) game 60 / 40 / 30 is shown at 120 / 80 / 60; Auto picks the highest the game holds, a fixed limit is the shown rate.
