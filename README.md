# NativeTOTK-Apple-Silicon

<p align="center"><b>A technical exploration: can a Nintendo Switch game's NVN graphics be ported to Metal?</b><br>
Tested with your own copies of Tears of the Kingdom and Breath of the Wild on Mac, iPhone and iPad. Not an emulator: the game's own ARM64 code runs directly on Apple chips.</p>

A fork of [ly05010419/MacNativeGames](https://github.com/ly05010419/MacNativeGames) with an in-game settings menu,
mods, cheats and save transfer added (see [This fork](#this-fork-settings-mods-and-save-transfer-status-2026-10-20)).
This README is in English; the original project's README also has a Chinese section
([Chinese instructions](https://github.com/ly05010419/MacNativeGames#chinese)).

## Legal

**You must own the game.** Use this project only with games that you bought, and with keys and system files dumped from your own Nintendo Switch. This is an independent, non-commercial project. It is not affiliated with, endorsed by, or sponsored by Nintendo. The Legend of Zelda and Nintendo Switch are trademarks of Nintendo.

The repository contains no game content and distributes none: no game program, art, text, audio, shaders or keys. Everything the game needs is made on your own computer from your own files when you build the app. The app you build contains the game, so it is for your own use only; do not share it. What the repository does hold is the information needed to make the port work: the patch code, the addresses and hashes used to check your files, and the [shared shader list](docs/shader-list.md), which names shaders and bind states but contains no shader code.

Apple, Mac, iPhone, iPad, Apple Vision Pro and Metal are trademarks of Apple Inc.; this project is not affiliated with Apple. Other game titles and trademarks named here belong to their respective owners.

Whether dumping games, keys and system files is legal depends on where you live; check and follow your local laws.

Issues and pull requests that contain or ask for game files, keys or firmware will be deleted.

## About

**This project is a technical exploration, mainly feasibility experiments and analysis.** The Nintendo Switch is an ARM system (an ARM64 CPU in NVIDIA's Tegra X1), and Apple chips are ARM64 as well, so we want to test whether its games can, technically, be ported to Apple chips. It started from one question: can the NVN graphics a Switch game was written for be rebuilt faithfully on Apple's Metal, and can the game's own ARM64 code run directly on Apple chips without an emulator? Tears of the Kingdom and Breath of the Wild are the test cases, because they push the graphics API hard. The repository records the experiments, measurements and findings along the way; it is a study of the technique, not a game distribution. **It does not encourage piracy:** use only your own game data, dumped from a game you bought and from your own Switch, never game files, keys or firmware downloaded from the internet or obtained from anyone else.

How it works: bring your game file (XCI or NSP) and your `prod.keys`; one script builds a Mac app or an unsigned iPhone/iPad IPA. The game's own ARM64 code runs directly on Apple chips (rewritten where the operating system requires it), the Switch system services are answered by a small host, and the game's NVN graphics are rebuilt on Metal. The iOS build requires iOS/iPadOS 26 and iPhone 14 or newer; version 1.4.3 has been played on an iPhone 14 and an iPad, see [iPhone and iPad](docs/install.md#iphone-and-ipad). Details are in [How it works](docs/how-it-works.md).

| Game | Version | Platform | Status | Folder |
|---|---|---|---|---|
| The Legend of Zelda: Tears of the Kingdom | 1.0.0, 1.2.1, 1.4.3 (1.1.2 builds too) | Mac, iPhone, iPad; Apple Vision Pro (windowed, early) | Supported | [`games/totk`](docs/install.md) ([Vision Pro](docs/install.md#apple-vision-pro)) |
| The Legend of Zelda: Breath of the Wild | 1.6.0, 1.9.0 | Mac; 1.6.0 also on the iPhone | Supported | [`games/totk`](docs/install.md#breath-of-the-wild) (shared runtime), list in `games/botw` |
| Super Smash Bros. Ultimate | 13.0.2 | Mac | Community port | [`games/ssbu`](games/ssbu/README.md) |
| Super Mario Odyssey | 1.3.0 | Mac | Community port, early | [`games/odyssey`](games/odyssey/README.md) |
| Bayonetta 3 | 1.2.0 | Mac | Community port, work in progress | [`games/by3`](games/by3/README.md) |
| Super Mario Bros. Wonder | 1.0.0, 1.2.1 | Mac | Community port, early | [`games/wonder`](games/wonder/README.md) |
| Ori and the Blind Forest: Definitive Edition | 1.0.1 | Mac | Community port | [`games/ori`](games/ori/README.md) |
| Super Mario Party Jamboree | 2.3.0 | Mac | Community port, experimental | [`games/jamboree`](games/jamboree/README.md) |
| Mario vs. Donkey Kong | 1.0.1 | Mac | Community port, early | [`games/mvdk`](games/mvdk/README.md) |

Community ports keep their own runtime, graphics layer and scripts in their folder and are built with the steps there.

> **Recommended: let an AI assistant install and test it for you.** Open this repository in Claude Code, Codex, or any other AI assistant you like, tell it where your own game file and `prod.keys` are, and ask it to build and install the app and to sort out whatever comes up on your Mac. It follows the [install skill](.claude/skills/install-zelda/SKILL.md), runs every step and checks the result; you just wait with a coffee. Installing and testing by hand is not recommended: there are many steps, and every Mac, game version and dump is a little different. That's why I haven't provided a one-click build option: an AI assistant is my one-click build option.
>
> **The install skill** ([`.claude/skills/install-zelda/SKILL.md`](.claude/skills/install-zelda/SKILL.md)) is a short guide written for the assistant: which of your files goes where, the command for your case, how long each step takes (a build is 20–40 minutes; it must not be stopped), and the fix for each common error. Claude Code finds it by itself (or type `/install-zelda`). With any other assistant, start with: *"Read .claude/skills/install-zelda/SKILL.md and follow it to install Tears of the Kingdom for me."*

## This fork: settings, mods and save transfer (status, 2026-10-20)

[BennettStaley/NativeTOTK-Apple-Silicon](https://github.com/BennettStaley/NativeTOTK-Apple-Silicon), branch `feat/settings-menu`, adds a
recomp-style in-game layer on top of the 1.4.3 port. Tested on an M-series MacBook Pro (macOS, background test runs on
a prologue save and an outdoor Sky Temple save) and installed on an iPad Pro 13" M4 (iPadOS 26); the iPad build has not
been played through yet. Upstream is merged through `831bcd1` (2026-10-26).

| Area | What's there | Status |
|---|---|---|
| **Settings menu** ([docs](docs/settings.md)) | A sidebar inside the game window on the Mac (works in full screen; mouse, keyboard, controller; ⌘, / F1 / Home), a menu on iPad / iPhone; four tabs (Game, Graphics, Content, Tools) with sections, per-tab reset; 36 settings, almost all live | Mac tested; iPad installed |
| Display | Display resolution up to display native or 4K, render resolution 25-200 % (the 3D world up to 1440p), aspect ratio (upstream's key-4 engine: fill the screen, 16:9, 16:10, 21:9, 32:9) with Vert+ on taller screens (HUD keeps its shape), maximum output | Tested live |
| Graphics | FOV, AA (game / off / FXAA / SMAA), CAS sharpening, anisotropic filtering, LOD bias, frame interpolation, bloom, depth of field, lens flare, cave fog, shadow resolution (live), render distance | Tested; fog only affects caves, render distance only cuts below 100 % |
| World / interface / controls | Time of day speed, frame rate limit (also on display-paced 120 Hz), Hide HUD (F2), readout detail (avg / 1 % low), shader counter (compiling / new / total), camera speed, face buttons, Button prompts (Switch / Xbox / PlayStation via icon mods) | Tested |
| **Mods** ([docs](docs/mods.md), [browser](docs/mod-browser.md)) | One folder per mod (Yuzu / Ryujinx layout), load order, a GameBanana browser in the Mods tab, the 1.4.3 size table rebuilt for mods, old-version and code-patch mods flagged | Tested with Defogger and Xbox UI for v1.4.3 |
| **Cheats** ([docs](docs/settings.md#cheats)) | A Cheats tab in the settings menu with the community cheat list for the running version, downloaded by the game from sthetix/nx-cheats-db (no codes in the repository) and shown through our own catalog: clean names in groups, variants as one choice, junk and graphics cheats left out, the ones the port cannot run folded away; plus your own Atmosphère files from a cheats folder, mods' `cheats/` folders or `--cheats`. Memory cheats live, code patches at the next start (checked against the Apple-silicon rewrite, refused with a reason when unsafe) | Mac only; tested in game on 1.4.3 (Infinite health and stamina code patches, Damage ×4 choice applied, Moon jump live from a list downloaded mid-session) |
| **Spawn** ([docs](docs/settings.md#spawn)) | Tools › Spawn: create items, weapons, creatures, NPCs and Zonai devices next to you from the game's own actor list (categories, search, favorites, recents, amount 1-20, a toast with the result) through the game's `requestCreateActorAsync`, no code patched | 1.4.3 only; Mac and iOS; not yet tried in game from the menu |
| **Save transfer** ([docs](docs/save-sync.md)) | Files app / Finder Files tab; `.totksave` over AirDrop (Send saves…, double-click / Open in to import, with backups and version checks); `save_sync.py` over USB / Wi-Fi | AirDrop not yet tried on devices |
| Builds | Game data outside the Mac app: a code-only rebuild takes ~40 s instead of ~12 min; the iPad app carries the game's icon | Done |

Next: AirDrop and iPad on-device testing, merging mods that change the same file, TKMM `.tkcl` packages, code patches
(pchtxt) from the Mods tab without a rebuild (today: `build_app.py --mod`), 7z / RAR on iPad, free camera / photo mode, cheats on iPad / iPhone, faster iPad re-installs (keep the game
data on the device between installs), outdoor haze, the AutoBuilder quick-menu check. Several settings follow ideas from
MaxLastBreath's UltraCam and NX Optimizer (no code of theirs is included; see Acknowledgements).

## Let's maintain the shader list together: no more stutter

> [!TIP]
> **Every shader in the shared list is ready before you play.** On a Mac the game does not stop to compile it, and on an iPhone or iPad nothing is missing from the picture. The more places players cover, the smoother the game gets for everyone.

The [shared shader list](docs/shader-list.md) now holds **13,422 shader variants** (11,996 shaders) from the Mac, iPhone and iPad: only states a build does not make by itself, since every build makes each shader's default variant. Every build makes them from the player's own game files, so the list holds identifiers only, never game data. Helping takes no setup:

1. **Play.** Every build carries each shader of the game for its default bind state. When your game meets a shader in a state that variant does not serve, or builds a render pipeline the app's list lacks, it notes it in `shader-list-contribution.txt`; default variants are never noted.
2. **On a Mac, every 100 new rows (shaders and pipelines together) it asks you once, before the game starts.** Click "Send": the GitHub form opens and the file is shown, ready to drag in. If the GitHub CLI (`gh`) is installed and signed in, the dialog offers "Submit automatically" instead, which sends this batch for you (a private gist and an issue) without opening the browser. An iPhone or iPad never asks during play: while it is connected to the Mac that built its app (cable or the same Wi-Fi), the Mac asks instead. "Don't remind me again" stops the reminders.
3. **Everyone's next build carries them.** Nobody stutters there again.

**[Send your shaders →](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml)** · The game never uploads anything by itself.

**Stuttering when you first start playing or enter a new scene is expected while new shaders compile—don't be surprised.** Performance typically improves as the cache warms up, and every shader the [shared list](#lets-maintain-the-shader-list-together-no-more-stutter) gains is one nobody stutters on again.

## Install and play

Everything from building the app to playing is in **[docs/install.md](docs/install.md)**. The recommended way is to hand it to an AI assistant with the [install skill](.claude/skills/install-zelda/SKILL.md) (see the top of this page) rather than run the steps yourself:

- [What you need](docs/install.md#what-you-need) · [Build the app](docs/install.md#build-the-app) · [Breath of the Wild](docs/install.md#breath-of-the-wild) · [iPhone and iPad](docs/install.md#iphone-and-ipad) · [Apple Vision Pro](docs/install.md#apple-vision-pro)
- [Controls](docs/controls.md) · [Where things are](docs/install.md#where-things-are) · [Known issues](docs/install.md#known-issues)
- [Test records](docs/test-records.md)

### This fork's builds

**Where the game data goes, and fast rebuilds.** The Mac app holds only the code and small files (about 150 MB). The 17 GB RomFS image and the prepared shaders go once into `~/Library/Application Support/MacNativeGames/TOTK/game-data`, in folders named by their content (`v143-romfs-<id>`, `v143-shaders-<id>`); on the work folder's volume they are APFS clones and take no extra space. The app finds them through its `Contents/Resources/totk/game-data.json`; if they are missing it says so and shows the command that makes them again. A rebuild whose game data and shaders did not change (after a code change, for example) only rebuilds the game binary and assembles the small app: about 45 seconds instead of about 12 minutes on an M4, and the macOS check of the new app takes seconds. The first game start after the data is written verifies it once (about 20 seconds); later rebuilds keep that verification. To copy the app to another Mac, build a self-contained app with `--embed-data` (the data inside it, as before). `python3.14 build_app.py --prune-data` deletes the game-data folders no app points to (shader folders of older shader lists, for example); see [where the game data lives](docs/how-it-works.md#where-the-game-data-lives).

#### Cheats (optional, Mac)

The game reads Atmosphère cheat files (the format Atmosphère, Yuzu and Ryujinx use) named after the game version's build
ID: 1.0.0 `082CE09B06E33A12`, 1.4.3 `277178B7DBA1B6D4` (both read from the game's own files), 1.2.1 `9B4E43650501A4D4` and 1.1.2 `9A10ED9435C06733` (from the cheat databases, not checked). Put the file in
`~/Library/Application Support/MacNativeGames/TOTK/cheats/`, or in a mod's `cheats/` folder, and start the game; no
rebuild is needed. `--cheats <file or folder>` still builds one into the app (only the file for the version you build is
used; one for another version stops the build):

```sh
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys --cheats ~/Games/cheats/
```

The repository ships no cheat codes. The game downloads the community cheat list for your version itself
([sthetix/nx-cheats-db](https://github.com/sthetix/nx-cheats-db), once a day; Settings › Interface › *Download cheat
list* turns it off), so the settings menu's **Cheats** tab (⌘, then the last tab, or the app menu's **Cheats…**) offers
those cheats without any files, by group, all off until you choose them; files of your own follow, grouped by file. Cheats
that only change values in memory switch on and off immediately; cheats that patch the game's code take effect when the
game restarts (**Restart now**): the code is never made writable while the game runs. A code patch is refused, with the
reason, when it would overwrite an instruction this project rewrites, jump out of the game's code or depend on a button.
The first time you enable a cheat your profile is copied to `profile-before-cheats`; cheats can corrupt saves on a real
console too. See [the settings menu](docs/settings.md#cheats).

## Settings menu

The game has an in-game settings menu on Mac, iPad and iPhone, built like a recomp's: 35 settings across Display, Graphics,
Frame rate, Interface, Controls, Audio and World, plus a Mods tab. Nearly everything applies while you play; *Maximum
output*, *Button prompts* and mod changes apply at the next start, and on the Mac **Restart now** does that in one click.

- **Display:** output resolution (match the window, display native, 4K … 720p, custom width), 3D render scale (25–200 %),
  aspect ratio (wider and taller screens see more, nothing is stretched), maximum output.
- **Graphics:** field of view, anti-aliasing (the game's / off / FXAA / SMAA), sharpening (AMD CAS), anisotropic filtering,
  texture detail, frame interpolation, bloom, depth of field, lens flare, cave fog, shadow resolution, render distance.
- **Frame rate, Interface, Controls, Audio, World:** frame rate limit, performance readout (with average and 1 % low),
  shader counter (compiling / new / total), Download new shaders (Mac: the shared list, once a day), Hide HUD, touch controls (iOS), camera speed, face buttons, Button prompts
  (Switch / Xbox / PlayStation icons from a mod), rumble, motion, volume, time of day speed, Send saves (AirDrop).

On the Mac open it with **⌘,** or **F1** (or a controller's Home button); it is a sidebar inside the game window, so it
works in full screen and the picture stays in view while you change things. On iPad and iPhone use the gear at the top, a
three-finger tap or a controller's Home button. See [docs/settings.md](docs/settings.md), [mods](docs/mods.md) and
[save transfer](docs/save-sync.md).

## How it works

See [docs/how-it-works.md](docs/how-it-works.md).

Recorded performance experiments, remaining work and comparison methods: [Mac performance follow-ups](docs/performance-followups.md).

## Acknowledgements

- **[UltraCam](https://gamebanana.com/mods/480138)** and **[NX Optimizer](https://github.com/MaxLastBreath/nx-optimizer)** by **MaxLastBreath**: the ideas behind several settings (time of day speed, fog, depth of field, bloom, shadow resolution, render distance, camera speed, hiding the HUD). No UltraCam or NX Optimizer code or binaries are included; each setting is this port's own 1.4.3 implementation.
- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** (MIT) developed this way of running Switch games natively (for Diablo II: Resurrected): the NSO rewriter, the Horizon host and the NVN-on-Metal layer here started as its generic parts.
- **[nxnative](https://github.com/seedds/nxnative)**: the one-script packaging and NSP common-ticket import follow its approach.
- **Ryujinx** (MIT): its shader translator (`vendor/ryujinx`) turns the game's Maxwell shaders into SPIR-V, and its system-service code is a reference. With this project's patches and service it is compiled ahead of time (.NET NativeAOT) into `games/totk/prebuilt/macos-arm64/totk_xlate`; `games/totk/tools/totk_xlate/publish_native.sh` rebuilds it (needs the .NET 10 SDK, only for developers).
- **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)** (Apache-2.0) turns SPIR-V into Metal Shading Language; it is fetched at a pinned commit and compiled with clang during the build.
- **[TkSharp](https://github.com/TKMM-Team/TkSharp)** (MIT, TKMM Team): the ResourceSizeTable size estimate the mod loader uses. **[zstd](https://github.com/facebook/zstd)** (BSD): the decoder for the game's compressed files. **[SMAA](https://github.com/iryoku/smaa)** (MIT) and **[AMD FidelityFX CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS)** (MIT), ported to Metal: the SMAA anti-aliasing and sharpening settings (the FXAA option is this port's own) (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). Mods are browsed and downloaded from **[GameBanana](https://gamebanana.com)**; each mod keeps its author's license and is never re-hosted.
- **deko3d** (zlib): texture layout sizes. **bcdec**: BC texture decoding. **libopus** and **libvpx** (BSD) from Homebrew: audio and the pre-rendered movies.

## License

MIT (see [LICENSE](LICENSE)). Third-party code keeps its own license: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
