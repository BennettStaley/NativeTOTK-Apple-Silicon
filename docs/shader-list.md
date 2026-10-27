# The shared shader list

`games/totk/assets/shader_list.tsv` is the one list of the game shaders and pipelines this project prepares ahead of
play. The Mac, iPhone and iPad builds all use it, and every player's discoveries go into it. Please send new entries
in a pull request.

## Why it exists

The game builds a shader *variant* from each shader for the bind state of a draw: which kind of texture sits in each
slot, attribute types, output formats. A variant can only be translated once it is known.

- On the Mac, a missing variant is translated while you play. That costs a short stall the first time.
- An iPhone or iPad cannot translate. It skips any draw whose variant the app does not already carry. A new install
  with only the default variants shows little or nothing of the picture.

The list names the variants players have met, so each build carries them from the start, and a running Mac app picks up
what was added after its build by itself (below).

## What is in it

The list holds identifiers only, never game data. Each build makes the shaders from the player's own game files.

| Row | Fields | Meaning |
|---|---|---|
| `V` | shader key, deps | A shader and the bind state it was translated for. |
| `L` | shader key, source, control offset, code offset, length | Where a shader that is not a BNSH file sits in the player's game files. |
| `S` | `<variant>_b`, stage, flags | A shader library the warm-up loads. |
| `C` | `<variant>_b`, flags | A compute pipeline the warm-up builds. |
| `R` | pipeline key, flags | A render pipeline the warm-up builds. |
| `B` | pipeline key, vertex border, fragment border, flags | That render pipeline with the sampler border colours Metal lacks, emulated in its shaders (`-` for a stage without). The warm-up builds the patched shaders and the pipeline. |
| `T` | pipeline key, tile reads, flags | That render pipeline with the fragment variant that reads the draw's own colour attachments from the tile, `<argument id>:<attachment>;` per read (`nvn_mtl.m warm_tileread`). Before 2026-10-26 only `pso_user.tsv` had these, so every new install built them in play (all 58 a 5-minute route built in the world with the list). |

- **Shader key:** the SHA-256 of the shader's NVN control section and code, computed the same way as the runtime.
- **Deps:** the translator's `deps` as compact, sorted JSON. They record the texture, image, attribute and fragment
  output types the shader depends on.
- **Flags:** `t` marks the title screen, its menu and the save list. The first frame waits only for these. `m` marks
  the rest of the warm-up.

**Locator rows:** the build finds the shaders of BNSH files, of agl's `.sharcb` packages and of the game module by
itself. A locator is for a shader kept anywhere else. It names the RomFS path
(each `/` after a package enters it, `.zs` decompressed) or `module:<name>`, plus hex offsets of the NVN control section
and the code and the code length. The build reads the bytes there from the player's own files and keeps them only when
they hash to the key, so another game version's bytes are never used.

An entry is a shader plus its deps, never a variant name. The runtime picks a variant by matching deps, and the same
shader and deps can carry different names on different machines.

`V` rows are sorted. Warm rows keep the order they were recorded in. Keys of every game version may be present; a
build uses only those found in the selected game files.

The build exports the warm rows to `build/pso_title.tsv` (rows marked `t`) and `build/pso_manifest.tsv` (rows marked
`m`), which the runtime reads. Edit the list, never those files.

## The Mac app keeps itself current

A Mac app does not wait for its next build: the running game downloads the list once a day and prepares the variants
its translation cache lacks from the player's own game files (`third_party/d2r_gfx/gfx/nvn/totk_shader_list.m`).
It only downloads; the game never uploads anything.

- **Fetch.** At the start, on a background queue, the game asks for
  `https://raw.githubusercontent.com/ly05010419/MacNativeGames/main/games/totk/assets/shader_list.tsv` at most once a
  day, with `If-None-Match` / `If-Modified-Since`: an unchanged list costs one request and answers 304. Timeout 20 s;
  offline, the previous list is used and the next start tries again. Only https (redirects too).
- **Check.** The download is untrusted, so `totk_shader_list.h` takes only the documented rows with the same patterns as
  `shader_list.py`, caps the file (48 MB), the lines (8 KB), the rows (1,000,000) and the deps (4 KB, only the deps kinds
  and their words, written exactly as `canonical` writes them), and rejects the whole file on anything else: the
  previous list stays. Of a valid file only the `V` rows are used.
- **Difference.** The `V` rows whose deps the cache's `<key>.variants` do not have. Keys of other game versions are
  skipped: the first time, the game indexes every shader of its own files (BNSH, agl `.sharcb`, SARC packages, zstd
  with the game's dictionaries, the game module, through the RomFS the game reads, mods included; 2–8 s for
  1.4.3's 28,769 shaders, the same count as the build) and keeps the index, so later days read only the files a new
  variant needs. The bytes are checked against the key before use.
- **Prepare.** After the warm-up (the title screen is up), one variant at a time at utility priority: translated by a
  translator process of its own, so the game's own translations never wait for it, then compiled with Metal and handed
  to the game's shader table like the warm-up's shaders: the first use in play is neither a translation nor a compile.
  It pauses while the game is busy (one of the game's own translations in the last 3 s, or under 20 frames a second).
  The shader counter counts the variants still being prepared as compiling. The log says
  `shader list: N new variants from the shared list, preparing…` and then
  `shader list: ready: …`.
- **Setting.** Settings › Interface › *Download new shaders* (on by default; live). Off: nothing is fetched and a
  preparation in progress stops.

Files, in `~/Library/Application Support/MacNativeGames/TOTK/shader-list/`: `shader_list.tsv` (the last good download),
`state.json` (address, ETag, Last-Modified, when checked), `index-<RomFS identity>.tsv` (where this game's shaders are; a
change of mods makes a new one), `failed.tsv` (variants the translator could not make, not tried again with the same
translation chain). The cache folder gets `shader-list-done.tsv` (list rows already prepared). For tests,
`TOTK_SHADER_LIST_DIR` names the folder (the launcher sets it; without it nothing happens), `TOTK_SHADER_LIST_URL` another
list (https, `file://`, or http on this Mac) and `TOTK_SHADER_LIST_FORCE=1` checks even when the last check is less than
a day old. `tests/test_shader_list_fetch.py` covers the parser on malformed input, the scanner, the difference, the
preparation and the fetch against a local server.

An iPhone or iPad cannot translate while playing, so its app has none of this; it carries the list from its build.

## Warm-up scope

The prepared seed also carries `shader-keys.json`: every shader key found in the selected game files, including shaders
that were not translated. The background warm-up warms only cache entries of those shaders, plus shaders the game was
seen to use on this Mac (`observed/<RomFS SHA>.keys` in the translation cache). Entries of other game versions in the
shared cache are skipped during warm-up only, never deleted or refused for drawing. Without a valid scope (an older
app) the whole cache is warmed as before; `TOTK_WARM_ALL=1` does that for comparison. Not used on iPhone / iPad.

## Sending what you played (players)

You do not need to do anything while playing. When the Mac has to translate a shader variant, or an iPhone or iPad
misses one, that variant is not in the list yet, so the game writes it to `shader-list-contribution.txt`: one `V` row
per shader and bind state, nothing else (`third_party/d2r_gfx/gfx/nvn/totk_shader_contrib.h`). Since every build carries
each shader's default variant ([shader-variants.md](shader-variants.md); `all_default` in the seed's manifest), only
states those variants do not serve are written, never a default variant (`totk_shader_rt.m contrib_wanted`): the list
keeps non-default variants only (2026-10-22; Breath of the Wild too since 2026-10-23).
Render pipelines are collected with them (2026-10-22): one the game builds in play that the app's warm-up lists lack
becomes an `R` row (and a `B` row with its border recipe, a `T` row with its tile reads), written off the submit thread
(`nvn_mtl.m pso_contribute`). A `B` or `T` row the lists lack goes in also when they have its plain `R` row (until
2026-10-26 it went only with a new `R` row).
Its shaders are named as a build names them: a default or depth-bound variant keeps its name, any other is named after
its deps as `prepare_shader_seed.py` names a list row (`totk_contrib_listed_name`), so another player's build has it.

- Mac: `~/Library/Application Support/MacNativeGames/TOTK/shader-list-contribution.txt`
- iPhone / iPad: Files app › On My iPhone (or iPad) › Tears of the Kingdom › `shader-list-contribution.txt`

On a Mac, every 100 new rows of either kind the launcher asks once, before the game starts, whether to send the file. An iPhone or
iPad never asks during play: while it is connected to the Mac that built its app (cable or the same Wi-Fi),
`games/totk/ios/tools/shader_contrib_watch.py` (a LaunchAgent the iOS build installs) fetches the device's rows every
10 minutes at most and adds the ones not there yet to the same file the Mac writes:
`shader-list-contribution/shader-list-contribution.txt` in your clone (the build makes the folder; Breath of the Wild's
is `shader-list-contribution-botw.txt`). One file for the Mac and every device, since the list does not tell platforms
apart; the folder opens in Finder with each reminder, which comes every 100 new rows in that file. "Remind me next time" waits for the next 100;
"Don't remind me again" stops the reminders for good (the file is still kept). "Send" opens the [shader list contribution](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml)
issue form; drag the file into it. Later reminders come at 200, 300 and so on, so sending the whole file each time
is fine: rows the list already has are skipped. The game never uploads anything by itself.

## Breath of the Wild

Breath of the Wild has its own list, `games/botw/assets/shader_list.tsv`, in the same format and kept with the same
tools (`shader_list.py merge ... games/botw/assets/shader_list.tsv <folder or file>`). Its build reads the game's
Yaz0 packages (`Pack/`, `Effect/`, `Event/`, `System/`: the `uking_*.sbfsha` material archives and the engine's agl
packages) and translates the boot and title packages' shaders for the default bind state plus every state the list
names (`games/botw/scripts/prepare_shader_seed.py`). Its players' contribution file is
`~/Library/Application Support/MacNativeGames/BotW/shader-list-contribution.txt`, sent with the
[Breath of the Wild issue form](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list-botw.yml)
(label `shader-list-botw`; `collect_shader_issues.py` merges both games' issues). The lists never mix: a key names a
shader of one game only.

Its shader key hashes the control section's own length (the word at +0x20: 0x7d1 for the game's archives, 0x848 for
agl's; Tears of the Kingdom's is 0x880, so its keys are unchanged) and, for the agl programs the game loads from
embedded BNSH files, the bytes as the game rewrites them before binding (`prepare_shader_seed.py bound_control`); the
agl packages' own records are bound unchanged.

Title rows (`t`) of either game must be recorded in a cache that has the prepared seed imported (the warm-up loads a
row's shader by its file name, and the seed names its variants by their bind-state answers, not as a cache translated
on the spot would): run the game with `DM_PSO_MANIFEST=off`, an empty `DM_NVN_SHMAP` folder and
`TOTK_SHADER_SEED_DIR=<prepared seed>`, then `shader_list.py merge --title ... <profile>/pso_user.tsv`.

## Merging what players sent (maintainers)

```sh
# what each open shader-list issue of either game would add (changes nothing). Each --seed gives a game version's
# shader keys (its shader-keys.json) and drops the rows its built-in variants serve; --keys <game>=<file> adds a key list
S=<work>/totk/build/shader-seed
python3 games/totk/scripts/collect_shader_issues.py --seed $S/v143 --seed <botw work>/totk/build/shader-seed/botw160
# write the lists; --close also thanks each merged issue and closes it
python3 games/totk/scripts/collect_shader_issues.py --seed $S/v143 --seed <botw work>/totk/build/shader-seed/botw160 --apply --close
# drop rows already in the list that the seed's default (or depth-bound) variants serve
python3 games/totk/scripts/shader_list.py prune games/totk/assets/shader_list.tsv <work>/totk/build/shader-seed/v143
```

It takes only `V` rows and render pipeline `R` / `B` / `T` rows from an attachment (or from a comment's, when the upload
failed) and drops everything else. One contribution file serves both games, so every row goes to the game its shader
keys belong to, whatever the issue's title says. Rows of shaders neither game's keys hold are not merged and keep their
issue open: a key list may lack a game version (give a seed of every version collected; a seed made before 2026-10-22
has no shader-keys.json and is refused), or the file is another game's (#525, Super Mario Odyssey). Once checked,
`--drop-unknown` leaves them out and closes the issue. Then check with `shader_list.py stats`, commit
the list and push; the next builds carry the new variants.

## Adding what you played (with git)

Use `games/totk/scripts/shader_list.py`. It needs only Python 3.

```sh
# Mac: add the shaders your own translation cache holds
python3 games/totk/scripts/shader_list.py merge games/totk/assets/shader_list.tsv \
  "$HOME/Library/Application Support/MacNativeGames/TOTK/shader-cache/rt-totk-native-v8-unverified"

# check the result, then commit the list and open a pull request
python3 games/totk/scripts/shader_list.py stats games/totk/assets/shader_list.tsv
```

- **iPhone / iPad:** `games/totk/ios/tools/fill_shaders.py` translates what a device missed. Like the Mac, it puts the
  new variants in the contribution file, not in the shipped list: add `--update-list` when you want the next build from
  this tree to carry them too. For Breath of the Wild it checks Breath of the Wild's own list (`--game botw`, the default
  for a `.botw` bundle identifier), and `--from-cache <rt-folder> --seed <seed>` packs the shaders the Mac's own Breath
  of the Wild cache has and the app lacks, already translated, for the device.
- **Pipelines:** merging a cache folder adds only its `V` rows. A cache's `pso_collected.tsv` holds every pipeline ever
  built, tens of thousands of them. To add pipelines, name a `pso_user.tsv` or `pso_collected.tsv` file explicitly.
- **Title rows:** record them with the warm-up off, then add them with `merge --title`.
- **Locator rows:** after merging a cache, run
  `python3 games/totk/scripts/locate_shaders.py --romfs <work>/totk/extracted/<version>/romfs --modules <work>/totk/rewritten/<version> <cache folder>`.
  It looks up every listed shader the build would not find by itself, using the code the cache recorded, and adds its
  locator after checking the key. It needs a built work folder and takes about 3 minutes.

The tool drops rows that are not shader identifiers. Merging the same source twice adds nothing.

## Reviewing a contribution

- `shader_list.py stats` must pass, and `python -m unittest tests.test_shader_list` must pass.
- The diff may change only `shader_list.tsv`. Do not add translated sources (`.metal`), compiled libraries
  (`.metallib`) or caches: they are made from the game.
