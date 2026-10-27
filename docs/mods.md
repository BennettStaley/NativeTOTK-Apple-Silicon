# Mods

The port loads RomFS mods: mods that replace or add game data files (models, textures, layouts, text, parameters,
packs). It reads them from a mods folder at every start; changes to the folder or to the order apply at the next start.

## Where

| | Folder |
|---|---|
| Mac | `~/Library/Application Support/MacNativeGames/TOTK/mods` |
| iPhone / iPad | the app's `mods` folder in the Files app (On My iPhone → Tears of the Kingdom → mods) |
| Tests | `TOTK_MODS_DIR=<folder>` (`0` = no mods) |

## Installing a mod by hand

Mods made for Yuzu, Ryujinx or a modded Switch ship a `romfs` folder. Unzip the mod into the mods folder so that each
mod has its own folder with `romfs` inside:

```
mods/
  mods.txt
  Base Defogger/
    romfs/Pack/EnvPalette.pack.zs
    romfs/Pack/CavePalette.pack.zs
  Some Other Mod/
    Some Other Mod v2/romfs/...        an extra folder from the archive is fine (up to three levels)
  Texture Pack/
    Model/...                          a folder whose top level already is RomFS folders counts as romfs/
```

- A mod with several `romfs` folders side by side (options, "pick one") is not loaded: keep the option you want and
  delete the others.
- Paths are case-sensitive in the game. A file whose path differs from the game's only in case (`pack/` for `Pack/`) is
  loaded under the game's spelling, and the log says so.
- A file the game does not have is added.
- `mod.json` (optional; the mod browser writes it): `name`, `author`, `version`, `game_versions`, `gamebanana_id`.

## mods.txt

One mod folder per line, in load order. The first line wins a file that two mods both change. `-` in front of a name
keeps the mod installed but off. `#` starts a comment line. Folders not listed are loaded after the listed ones, in
name order, and turned on. A line naming a folder that is gone is dropped the next time the list is saved.

```
# Mods load order: the first line wins a file two mods both change. "-" in front = installed but off.
Texture Pack
-Base Defogger
Some Other Mod
```

## What happens at start

The log (`~/Library/Logs/MacNativeGames/totk.log`, iOS `Documents/log.txt`) lists, under `[totk] mods:`
(the log is off by default: turn it on with `defaults write org.macnativegames.totk TOTK_DEBUG_LOG -bool YES`,
`org.macnativegames.botw` for Breath of the Wild, and start the game again; the `grep` checks on this page need it too):

- each mod, whether it is on, its file count and notes (code patches not applied, its own ResourceSizeTable left out,
  made for an older game version, files a mod higher in the list already changes);
- the files it replaces or adds (the first 16), and what happened to files the port's own data patches also change (see
  below);
- the ResourceSizeTable: the game's memory table is adjusted for the mods' files.

### Files the port also patches

The port ships a few fixed game files of its own (`games/totk/scripts/make_romfs_patches.py` and the heap sizes for
1440p / 4K output). When a mod changes one of them:

| File | What is used | Log line |
|---|---|---|
| `UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs` (the HUD and menu layouts: button glyph mods such as Xbox UI or PlayStation UI replace it) | the mod's copy, with the port's quick-menu background fix applied to it again | `... the mod's copy is used, with the port's quick-menu background fix applied to it again`, then `... fix is applied to the mod's copy (4 visibility keys moved ...)` |
| `Event/EventFlow/DmF_SY_GameOver.bfevfl.zs` (a game over fix only 1.0.0 runs) | the mod's copy | `... the mod's copy is used and the port's patch is left out` |
| `System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs` (only with 1440p / 4K output) | the port's copy: the game needs the bigger heaps for that output | `... which needs its own copy (the heap sizes for the output resolution); the mod's copy is left out` |

The quick-menu background fix: the weapon / shield / material quick menus (hold a d-pad direction) and the Autobuild
screen show a capture of the game picture behind them. The game takes it when the capture panes become visible at
animation frame 1.0; at 60 fps that comes after the world has stopped drawing, so the background is black. The fix moves
those four visibility keys to frame 0.0 (in `anim/ShortCutBG_00_In.bflan` and `anim/AutoBuilder_00_In.bflan`). For a
mod's archive the runtime decompresses it with the game's zstd dictionaries, moves the keys only where they are exactly
the game's (hidden at 0.0, shown at 1.0) and serves the result uncompressed (zstd raw blocks), cached in
`mods/.cache/capture-fix-*.blarc.zs` until the mod's file changes. If the mod changed those animations, its archive is
used as it is and the log says `the port's quick-menu background fix was not applied (...)`: the quick menus can then
show a black background at 60 fps.

Mod files are not hashed at start, so a start with mods is about as fast as one without. If a mod file changes or is
deleted while the game runs, the game stops when it next opens that file instead of reading something unchecked.

### The ResourceSizeTable

The game reserves memory for each resource from `System/Resource/ResourceSizeTable.Product.143.Nin_NX_NVN.rsizetable.zs`.
Mods that bring their own copy made it for one game version and one set of mods, so the port never uses it: it takes
the game's table and raises the entry of every file the mods change or add (and of each changed file inside a replaced
`.pack`) to an estimate from its size and type, using the calculator of [TkSharp](https://github.com/TKMM-Team/TkSharp)
(TKMM's library, MIT). The result is cached in `mods/.cache` until the mods' files change.

## What works, what doesn't yet

| | |
|---|---|
| RomFS files (replace and add), any number of mods, load order, turning mods off | works |
| Archives with an extra top folder, folders without `romfs/` whose top level is RomFS folders | works |
| A mod's own ResourceSizeTable | left out; the game's table is adjusted instead |
| Files the port also patches (UI layouts, the game over flow) | the mod's copy; the port's quick-menu fix is applied to the UI layouts again |
| Code patches for main (`exefs/`: `.pchtxt`, `.ips`, `.ips32`) | not loaded from the mods folder: merged when the app is built with `--mod <mod folder>` ([Code patches](#code)); the Mods tab says so for such a mod |
| Extra code modules (`subsdk9` and the like) | not loaded: the app runs main, sdk and subsdk0 only |
| TKMM packages (`.tkcl`) | listed but not loaded; coming later |
| Two mods changing the same file (merging packs, text, parameters) | the first in the order wins; merging is coming later |
| Mods made for 1.0-1.2.x | load, flagged: files whose format changed in 1.4.3 can crash or look wrong |
| Cheats (`<mod>/cheats/<build ID>.txt`, Atmosphère format) | Mac: listed in Settings › Cheats while the mod is on, in the load order ([settings](settings.md#cheats)); iPhone / iPad: not yet |

<a id="code"></a>
## Code patches (build time)

`--mod <mod folder>` merges a mod's main-module code patches into the app while it is built. The app has no module
loader: at start it maps the three code modules it was built from (main, sdk and subsdk0), rewritten for Apple
silicon and bound by SHA-256. The patches are applied to the verified retail main of the version being built before
that rewrite, so the app still matches the hash it is bound to; RomFS (data) mods are unaffected and are still
loaded at start.

```bash
python3.14 build_app.py "/path/to/Tears of the Kingdom.xci" -k "/path/to/prod.keys" --mod "/path/to/Mod"
```

Several mods with code patches are combined in a mod manager first: TKMM's merged output is one folder with one
`exefs/` and one `romfs/` (a manager may also pack several mods' records into one `.ips`), and `--mod` takes that
folder as it is. Two patch files that change the same bytes are refused with both names, so a merge that overwrote
one mod's change with another's stops the build instead of shipping silently.

The folder holds the mod's files in the usual layout - `exefs/` for code, `romfs/` for data - and either half is
enough: without `exefs/` there is no code to merge, and without `romfs/` the version folder is never touched. An
`exefs/` that holds nothing to merge into main (`subsdk9`, a `main.npdm`) is a note, not a failure: the build
carries on, its code half is in the app, and its data half takes effect once the folder is installed
(`--install-mod-romfs`, or by hand). With `--update` the patch is merged into the updated module, which is the one
the app runs.

## Keyboard key icons

When no controller is connected at start and `mods/<version>-keyboard/manifest.tsv` exists, the launcher points
`TOTK_ROMFS_MODS` at that folder instead, so the prompts show the Mac keyboard's keys while you play on the keyboard:

```sh
python3.14 scripts/make_button_icons.py "$HOME/Library/Application Support/MacNativeGames/TOTK/mods/v0-keyboard" \
  --app "Tears of the Kingdom.app" --version v0 --style azerty   # or --style keyboard for a US layout
```

The choice is made once per start: connecting a controller later keeps the keyboard icons until the next start.

## Code patches at launch (`TOTK_PCHTXT`, Mac)

A main `.pchtxt` can also be applied when the game starts, without a build:

```bash
open --env TOTK_PCHTXT="/path/to/mods.pchtxt" "/path/to/Tears of the Kingdom.app"
```

It is read the way `--mod` reads it, and its `@nsobid` must match main's build ID, which a built app keeps in
`extracted/<version>/exefs/main.build-id`. The rewriter doesn't see these bytes, so a patch that makes system calls,
uses the thread-pointer or counter registers, or uses x18 still needs a build with `--mod`. A line on a site the app
patches itself stops the start.

## The older layout

Before the mods folder, the port loaded one folder, `mods/<v0|v112|v121|v143>`, with a `manifest.tsv` written by
`games/totk/scripts/make_mod_manifest.py` (each file's size and SHA-256 and its original's, checked at start). It still
loads, before the mods folder's mods; it is not listed as a mod.

Each `manifest.tsv` line has four tab-separated fields: `<RomFS path>  <size>  <SHA-256 of the mod file>  <SHA-256 of the
original, or ->` (`-` adds a file the game does not have). Only the listed files are served: an unlisted new path is
absent for the game, an unlisted replacement is read from the game, and a listed file whose size or hash changed stops the
start (`RomFS overlay file missing or wrong size` / `RomFS overlay integrity mismatch`). To make one (from the repository
root, after copying the mod's `romfs/` contents into the folder):

```bash
MODDIR="$HOME/Library/Application Support/MacNativeGames/TOTK/mods/v143"
python3 games/totk/scripts/make_mod_manifest.py "$MODDIR" --app "/path/to/Tears of the Kingdom.app" --version v143
```

The script hashes each file and its original in the app's RomFS, leaves out files identical to the original, and prints
`...: N files (R replaced, A added)`; the log then says `.../mods/v143 checked, N files`. Regenerate it after every
change and never edit it by hand. A build with `--mod <mod folder> --install-mod-romfs` fills this folder itself:
it replaces it with the mod's `romfs/` and writes the manifest (what it held is moved next to it as
`<version>.backup-<date>-<time>`). A file there that the build's own patches also replace (a translation's UI archive,
`Common.Product.<n>`) is merged with the build's edit by `make_mod_manifest.py`, and its manifest line names the build's
file as the original, so the game lets it take the place of the build's overlay (upstream #134, issue #73). (Upstream's guide to this layout, also in Chinese:
[ly05010419/MacNativeGames docs/mods.md](https://github.com/ly05010419/MacNativeGames/blob/main/docs/mods.md).)

## Button-icon mods

A mod that changes the game's button-icon archive (`UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs`) and whose
name (or `mod.json` `"prompts": "xbox"` / `"playstation"`) says Xbox or PlayStation is a *button prompts* mod. It is not
switched in the Mods tab: **Settings › Controls › Button prompts** picks the style at each start (Automatic follows the
last controller connected) and turns on the first installed mod of that style and every other button-icon mod off.
Switch (the game's own icons) turns them all off. A mod that names both (a "Controller UI (Xbox & PS)" pack with options
inside) is an ordinary mod.

## Breath of the Wild

The Breath of the Wild app reads its mods from the folder for its version, `botw160` for 1.6.0 or `botw190` for 1.9.0:

```text
~/Library/Application Support/MacNativeGames/BotW/mods/botw160
~/Library/Application Support/MacNativeGames/BotW/mods/botw190
```

The older layout above applies: copy the mod's RomFS contents into that folder and generate the manifest with the app,
naming the folder `botw160` or `botw190`, or passing `--version botw160` or `--version botw190` (1.6.0 shown):

```bash
MODDIR="$HOME/Library/Application Support/MacNativeGames/BotW/mods/botw160"
python3 games/totk/scripts/make_mod_manifest.py "$MODDIR" --app "/path/to/Breath of the Wild.app"
```

Check `grep "mods/botw160 checked" ~/Library/Logs/MacNativeGames/botw.log | tail -1` after the start
(`mods/botw190 checked` on 1.9.0).

- A mod that makes a resource larger than the original also has to ship the matching resource size table,
  `System/Resource/ResourceSizeTable.product.srsizetable`, built for the same version. Without it the game reserves the
  original size and stops at start (for example a translation whose `Pack/Bootup_<lang>.pack` grew).
- The game's language follows the app's preferred language, which also picks the language folder a mod must use (`USen` for
  `en-US`). To give only this game another language:
  `defaults write org.macnativegames.botw AppleLanguages -array en-US`.

## Three packaging traps that look like a crash

These come from how a pack was built, not from its content, and none of them prints one of the lines above.

- **A `.pchtxt` without `@flag offset_shift 0x100`.** Its addresses are `main` addresses, while an IPS file's offsets
  count the 0x100-byte NSO header. Both `TOTK_PCHTXT` and `--mod` stop the start when a patch line comes before that
  flag, or when it names another shift: the alternative is every byte landing 0x100 bytes off without an error.
  Converting an IPS by hand: `main address = IPS32 offset - 0x100`.
- **A `.zs` whose zstd frame does not declare its content size.** The game's decompressor has no size to read for those
  and decompresses to a null destination: the window dies right after `ENTER nnMain` and the log holds nothing of ours.
  `zstd` writes such frames when compressing from a pipe (`zstd -c`); the descriptor byte of the frame header (`raw[4]`)
  must have bits 7-6 set. Compress from the bytes with a library, assert `(raw[4] >> 6) & 3`, and keep the window size,
  checksum and dictionary of the file being replaced (`zstd -l -v`).
- **A merged `ResourceSizeTable` that kept the records but lost the slots.** A manager that merges `.rsizetable` files
  can write the record array only; actors whose slot is gone then load with size 0. Slot names are logical resource
  paths (`Actor/...`), not RomFS paths, so not finding them in the index is normal. Rebuild the table as a union of the
  tables being merged - each value copied byte for byte, the record array kept sorted by key, since the game
  binary-searches it - and check the frame header of the compressed result before installing it. `grow_restbl` in
  `games/totk/scripts/make_button_icons.py` is one writer of that format.
