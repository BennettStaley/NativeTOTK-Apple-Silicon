# First-launch preparation and warm-up of eight thousand shaders

User goal: a freshly built App should, on its very first launch, discover and warm up about eight thousand shaders as if a play cache already existed, without having to play through the game once to collect them. Mac only for testing.

The old packager copied only the PSO key list, not the MSL, reflection and dependency index those keys refer to. With an empty cache, warm-up measured 0 shaders; 2,171 shader keys in the list had no translated data. Warm-up results on a machine that already has a play cache say nothing about a first install.

## Build and launch behaviour

1. Before signing and packaging, `build_app.py` prepares shaders from the selected version's fully verified local RomFS; it does not read any existing user cache. `prepare_shader_seed.py` decompresses the dictionary archives, scans the whole RomFS (`Shader/` packs, SDK/Bootup packs, and, since 2026-10-19, shaders embedded in effect `Effect/*.esetb` files and UI layouts) and the game's program modules (main embeds BNSH), parses BNSH/NVN programs, and also parses the agl engine's own shader archives `.sharcb` (file magic `BAHS`: each record header holds the code's offset in the code pool, the control section size and a signed offset to the control section; several records may share one control section). The title screen's compositing, post-processing and so on use agl shaders (`Lib/agl`, `Shader/ApplicationPackage`, `Lib/Terrain`, `Lib/gsys`); scanning only BNSH missed all of them, which is why a fresh iPhone install rendered a fully magenta screen. Control sections come in two sizes, 0x880 and 0x88c; the runtime key covers only the first 0x880 bytes. No padding bytes are invented for old short control structures.
2. Shaders are deduplicated by the same control + code SHA-256 the runtime uses. The native translator and SPIRV-Cross generate MSL, reflection and dependency indexes for the default binding state of every shader in the startup pack; then, following the shared list `assets/shader_list.tsv` ([shader-list.md](shader-list.md)), each shader found in this game also gets the binding states the list records, keeping only one where the dependencies match an existing variant. iPhone / iPad cannot translate during play and rely entirely on this. The build stops if fewer than 8,000 are prepared successfully; it must not ship on the strength of the list alone, duplicate entries or inflated counts. Partial output from failed translations is not packaged. One translation service process is started per CPU core, work is distributed per shader (a shader's default and list binding states are translated in their original order in the same process), and results are merged in the original order, so the output is byte-for-byte identical to translating one at a time. On an Apple M4 (10 cores), the 24,032 translation jobs for 1.4.3 dropped from about 166 s to about 29 s.
3. The result is `build/shader-seed`. Its `shader-keys.json` lists every shader key found in this game version (including ones that were not translated or failed), with the translation chain, version and RomFS hash, and is included in the file hashes of `seed-manifest.json`; older seeds without it are re-prepared. Version, translation chain, tool identity and all file hashes are checked on reuse/packaging. This is data derived from the user's game; it stays in the work directory and the App and is never committed to the source repository. Metal libraries are generated on the target Mac; binary caches for other GPUs are not packaged.
4. When the Metal toolchain is installed, `step_shader_libs` precompiles every MSL in the seed into `build/shader-libs/<source SHA-256>.metallib` using the same safe-math options as the runtime, and puts them in the App; rebuilds compile only changed shaders. The runtime looks up by the SHA-256 of the local MSL and uses a library only on an exact match; otherwise it compiles as before. The system Metal cache does not travel with the App (it misses once the App moves to a directory with a different path length; see "The Metal cache" below), but these libraries do.
5. On first launch the seed is verified and imported into the external user cache: shader files are published first, then `.variants` are merged; the user's existing variants and other files are kept. The same seed is not imported twice; deleted files are restored. Wrong translation chains, tampered source files and out-of-bounds file names are rejected. Concurrent callers wait for the same import to finish.
6. Since 2026-10-23 (576fdfd) the title screen waits until all warm-up and the pipeline archive are done; `TOTK_WARM_BEFORE_TITLE=0` restores the previous behaviour, in which case the Mac loading screen into a game waits for warm-up to finish with no limit (`mtl_warm_wait(-1)` in `hle_nvn.c`), while iPhone / iPad still wait at most 15 seconds. The previous behaviour: before the first frame, only what the title screen, main menu and save list use is warmed (rows marked `t` in the shared shader list `assets/shader_list.tsv`, exported at build time as `build/pso_title.tsv`; see "The first frame waits only for the title list" below and [shader-list.md](shader-list.md)). This version's remaining shaders and lists keep warming behind the title screen (see "Warm-up scope" next), and the first load into a game waits at most another 15 seconds on the loading screen. (The 2026-10-18 daytime version warmed the whole batch of shaders before the first frame.) Only list rows that currently have data count toward progress, and the plan for the run is frozen; old pipeline list entries missing their source are counted separately. Newly translated MSL joins the supplementary scan only after its reflection is published, so a pair still being written is not misreported as failed. The shortcut help sits below the startup overlay so warm-up progress stays visible.

## Warm-up scope (PR #84 part A, merged 2026-10-20)

All versions share one translation cache directory, so on a Mac that has played other versions the cache mixes in other versions' shaders, and background warm-up used to compile all of them. Background warm-up now covers only the shaders in the seed's `shader-keys.json`, plus shaders this machine has actually seen the game use that are not in the list (recorded in the cache's `observed/<RomFS SHA>.keys`, with translation chain, version and RomFS identity; ignored if they don't match). Other versions' cache entries are simply not warmed this run; they are not deleted and are used as usual when actually drawn.

- `shader-keys.json` is read directly from the seed directory and checked against the hashes and game identity in `seed-manifest.json`; if the check fails, warm-up falls back to the whole cache. It is verified with the other files when the seed is imported, but not copied into the cache.
- `TOTK_WARM_ALL=1` goes back to warming the whole cache, for comparison.
- Not enabled on iOS (iOS warm-up and memory were not measured); behaviour unchanged there.
- Measured (2026-10-20, M4, 1.4.3, copy of a player cache mixed with shaders from 1.2.1 and other versions): same cache and seed, scoped warm-up did 20,993 shaders in 15.9 s of background time; `TOTK_WARM_ALL=1` did 31,770 in 28.4 s. A cache that has only played one version gains nothing.
- PR #84 part B (compiling Metal libraries locally) is on hold: there is only a manual tool, not yet wired into packaging or the launcher.

## Pipeline archive (2026-10-21, on by default, `TOTK_PIPELINE_ARCHIVE=0` turns it off)

When a pipeline is built, Metal compiles the shaders' intermediate form (metallib) into machine code for this GPU. To avoid hitches, iOS used to build every pipeline the App ships with at startup and keep them all (3.84 GB on an iPhone 14 once in game, limit 4 GB); the system's own shader cache is cleared on reboot and OS updates, so it can't be relied on.

Machine code now goes into a Metal binary archive inside the translation cache (`pipelines/` directory, `totk_pipeline_archive.h`):
- When building a pipeline, look only in the archive first (`MTLPipelineOptionFailOnBinaryArchiveMiss`); on a miss compile normally, then add the result to the archive.
- First run (iOS) does no full warm-up: it builds only the title screen's pipelines; the rest of the pipeline lists are built one by one in the background, added to the archive, and dropped immediately.
- With an archive present, only the pipeline lists are prebuilt; the ~20,000 shader libraries are no longer loaded separately, and the rest come from the archive when used.
- Adding to the archive means compiling again and competing with the game for the compiler service, so nothing is written during play: pipelines newly compiled on the game thread are recorded in `pso_user.tsv`, the next launch's warm-up compiles them and writes them into the archive on the spot (`par_add`), and the launch after that takes them from the archive. (Before 2026-10-22 they were queued until the game was "idle in the world", which an iPhone never reached, so every launch recompiled several hundred.)
- **Eight pipelines per file on the Mac, one per file on iOS** (before 2026-10-26 one per file everywhere; `pipelines/<name>.metalarchive`, with the pipeline keys a file holds listed in a `.keys` file of the same name; `TOTK_PIPELINE_ARCHIVE_BATCH=<count>` changes it); when building a pipeline only the file that holds it is handed to Metal, and files for unused pipelines are never opened. The key is used only to find the file; Metal checks functions and state, so the wrong file is just a miss, never a wrong pipeline. Why not more per file, measured on M4:
  - With many pipelines in one file, opening it makes Metal copy every pipeline descriptor into one flatbuffer; memory grows roughly with the square of entries per file and is not returned when the archive is released (simplest shader: 32 entries 1 MB, 1,024 entries 777 MB);
  - Metal checks every archive handed to a pipeline, so more files means slower (102 new pipelines with 142 files attached: 3.1 s);
  - Adding to an archive opened from a file and writing it again writes out only the newly added part;
  - Single-entry file: opens in 0.08 ms, about 35 KB, building a pipeline from it 0.04 ms. About 55 KB each on disk (including the shader intermediate form); 3,750 entries about 206 MB.
- When the OS version, GPU or translation chain changes, the whole `pipelines/` directory is deleted and rebuilt (`pipelines/identity.json`).
- When adding pipelines to a new archive, Metal creates a working directory in the user cache directory (`com.apple.gpuarchiver/gpuarchiver-*`, about 380 KB). After the write no process opens it again, but Metal never deletes it, not even on process exit. With one file per pipeline, every write left one behind: a one-minute first run left 432, and overnight it grew to 65,536 (25 GB), at which point no new directory could be created. Now, once the warm-up threads have written the files in hand and no archive is being written, the directories that appeared meanwhile are deleted (before 2026-10-26 after every single write, when archive writes happened one at a time; other programs' directories are not touched). Where the directory lives: programs with a bundle id (the iPhone / iPad App, the packaged Mac App) use `<cache dir>/<bundle id>/com.apple.gpuarchiver`, which belongs to this App alone, and leftovers are cleared in the background at startup; programs without a bundle id (such as `build/totk-native-v143`) use `<cache dir>/com.apple.gpuarchiver`, shared by the whole user. On iOS `confstr(_CS_DARWIN_USER_CACHE_DIR)` returns no path, so both the `confstr` directory and the system Caches directory are used, each checked with and without the bundle id. The first version only looked at the `confstr` one and deleted nothing on iPhone (2026-10-21: one first run left 239; after the fix, startup deleted 239, leaving 0). Mac first run measured: 3,000 entries written, 3,000 deleted, directory count no longer grows.
- **The warm-up builds pipelines on several threads (2026-10-26).** Shader libraries were already loaded on several threads, but pipelines were built one after another, each opening its own archive, writing its file and listing the working directories twice: Tears of the Kingdom 1.4.3 with an empty cache spent 235 s on 23,226 pipelines at its first start (about 10 ms each) and waited 248 s in all for the title. Now each block's pipeline rows (R / B / T / C) are shared among the warm-up threads (8 on the Mac, 2 once in the world, 1 on iOS); each thread adds to its own archive and writes it when a file is full (`warm_build_rows`, `par_write`, `par_flush_thread`). Writing the files is most of the time (315 s of the 8 threads' 400 s added together); Metal mostly compiles an archive's machine code at that step, so the gain comes from the threads writing at the same time. Waiting before the title on an Apple-silicon Mac:

  | Pipelines per file | First start (empty cache) | Second start | Footprint in the world after the second start |
  |---|---|---|---|
  | 1 (before, one thread) | 248 s | 28.3 s | 7,427 MB |
  | 1 (8 threads) | 109.5 s | 19.3 s | 7,371 MB |
  | **8 (Mac default)** | **62.0 / 83.0 s** | **11.4 / 11.5 s** | **7,249 / 7,414 MB** |
  | 32 | 75.2 / 71.4 s | 12.1 / 11.8 s | 7,903 / 8,201 MB |

  32 per file takes 500–800 MB more (Metal loads a file's whole set of pipeline descriptors when it opens it), so the Mac uses 8. iOS opens a file only when one of its pipelines is used, a large file would load unused descriptors with it, and the phone runs close to its memory limit, so it keeps one per file (not measured on a phone). The "before" row ran from another app folder with a warmer system Metal cache, which favours it. Pipelines built in play over the same route: 73 (75 before), no change. Breath of the Wild 1.6.0 (8 per file): first start 23.4 s (9,224 pipelines), second 3.4 s. Regression check: `make probe-gpu-pipeline-archive` (4 threads writing, one key written twice, read back, a file left half-written is cleaned up, every pipeline taken from its file without compiling, the working directories deleted).
- `TOTK_SHADER_DEVICE_LIBS=1` (Mac, for tools): load the seed's precompiled `.metallib` files as iOS does, e.g. an iOS seed. Pipeline descriptors built on the Mac then reference exactly the libraries the iPhone loads, so `metal-tt` can generate an iPhone GPU archive offline.

iPhone 14 measured (same archive, after entering the game):

| | Before (full warm-up) | Pipeline archive | After rebooting the phone |
|---|---|---|---|
| footprint | 3.84 GB | 3.65 GB | 3.64 GB |
| Warm-up | 95–122 s | 5.9 s | 16.2 s |
| Total pipeline build time in long frames | 1,452 ms | 102 ms | 241 ms |
| Archive hits | — | 4,000 / 4,000 | 4,000 / 4,000 |

First run (archiving in the background) was 3.54 GB and archived 4,607 entries in 138 s. Mac (M4, same archive, same location): memory flat (one file per entry 4,418 MB, no archive 4,403 MB; `MALLOC_LARGE` 74 MB in both). First run is the same as before (412 ms at world entry vs 417 ms before), because archiving means compiling again and competing with the game for the compiler service, so it waits for idle; the first run also does not attach archives to pipelines, since Metal checks every attached archive and the file count keeps growing during writes. From the second run on: first frame 59.6 s → 26.7 s, total pipeline build time in long frames 498 → 266 ms, longest single build 417 → 177 ms.

## Counts and scope

| Selected version | Unique shaders | Offline translation failures |
|---|---:|---:|
| 1.0.0 | 9,360 | 0 |
| 1.1.2 | 8,286 | 0 |
| 1.4.3 | 8,290 | 0 |

These are genuinely extracted and translated unique shaders, not eight thousand pipelines, and not every binding-state variant a save has ever encountered. When texture types, vertex attributes or output formats change, dependency matching still rejects unsuitable seed entries and generates new variants; required validation is not skipped to improve hit rate. Entering new areas can still compile pipelines and new variants; eliminating all hitches is not promised.

The 1.4.3 seed is about 334 MiB, 24,871 runtime files. CPU-only check with the real production importer: importing into an empty directory brought in 24,871 files, the second import 0, both recognising 8,290 shaders. The source user cache, the real save and its backup were not used to generate this data. For the other versions, extraction/translation is what has been verified; it is not presented as full play verification of a new App.

The 86 system shaders in the first trial were an interim step before the scope widened; once the user explicitly asked for eight thousand, they were replaced by the full set above. Early tests stopped automation and kept the window when real keyboard input arrived; the three interrupted script rounds are not counted as passing. One interim package was also deliberately stopped to fix the shortcut panel covering warm-up progress; it is not treated as the final version.

## Tests and local evidence

- The 37 root-level tests cover container/update/relocation, work directory isolation, packaging/desktop migration, actual BNSH stage offsets and runtime hashes, rejection of corrupt ranges, native import/reuse with user variants preserved, and rejection of wrong translation chains/file hashes/paths. It was also verified that while an old App is still running, the packager waits for it to close normally before replacing it.
- Full preparation, real import reuse, the final App and real launch evidence are archived outside the project at `/Volumes/mac-APFS/MacNativeGames-artifacts/2026-10-18/analysis/v143-port/shader-first-launch-8k`. The project keeps only this document and the test code, not the raw run records.
- The first-round interim build `bd005120...` with only 86 shaders showed that with an empty cache it imported 259 files and had the 86 shaders ready after about 4.8 seconds, before the first frame; this does not count as final verification of the eight thousand. Its original interrupted records are in `shader-first-launch-35/36`, left unchanged.

## Final App measurements (2026-10-18 daytime, version that warmed the whole seed before the first frame)

Full 1.4.3 CLI packaging passed; runtime SHA-256 `0f3798678a46c453a0d96dfae1f0401fb68849aa6fc84627df0f0f60d27a9062`. The real App was opened through LaunchServices with a fresh empty profile and empty translation cache; no prior play, no user cache copied in, no injected keys. Results:

- 8,290 discovered initially; 24,871 runtime files imported.
- The startup-critical phase waited 596,773 ms; the first frame was released only once all 8,290 shaders were available, and arrived 641.563 s after launch. The whole run was observed for about 642 seconds. The roughly ten-minute first-launch wait is this machine's measurement, not a promise for other machines.
- Full background warm-up took 597,024.3 ms, ending at 8,291: the 8,290 from the seed plus 1 actually added at startup; 0 supplementary warm-up failures. An empty profile has no collected render/compute PSOs, so both were 0 this run; shader warm-up must not be described as eight thousand pipelines being ready.
- A read-only check saw the warm-up phase text and progress bar; afterwards the game entered the opening underground scene normally, with Link, Zelda and the torch visible. The briefly shown FPS was not treated as a long-run performance test.
- Real user input recorded: none. Afterwards the standalone test session was closed normally via the window close button. The real profile and the pre-upgrade backup each had 54 files with identical hashes before and after. The bundle signature passed `codesign --verify --deep --strict` again.

Raw records are in the evidence directory: `runtime/report.json`, `runtime/game.log`, `runtime/game-after-visual-review.log`, `runtime/close-result.json`, `visual-review.json`, `post-launch-signature.json` and `formal-profile-after.json`. This full App run verified the first launch with an empty translation cache; a repeat import of 0 files was verified by the CPU check of the production importer. It is not claimed that the new eight-thousand set has completed three rounds of App positive/negative comparison or a second GPU launch timing.

## The first frame waits only for the title list (2026-10-18 evening, Claude)

**Why the change.** With all 8,290 at the front of warm-up, the first frame waited for every one to compile. Metal keeps a system cache of compiled libraries, but it is not always there: macOS purges it when disk space is tight, and it has also been invalidated wholesale for reasons not yet understood (see "The Metal cache" below). Measured:

- The same game binary copied to another directory waited 451 seconds for the first frame (precisely because the directory changed; see "The Metal cache" below);
- After the system disk filled and the cache was purged, the desktop App waited 679 seconds for the first frame;
- Even with a warm cache, every launch waited an extra 4–11 seconds before the title.

**What changed.**

- Title list (formerly `assets/pso_title.tsv`, merged since 2026-10-19 into the shared list `assets/shader_list.tsv` with the `t` marker): the shaders and pipelines created by the title screen, main menu and save list were recorded once each on 1.4.3, 1.1.2 and 1.0.0, merged and deduplicated to 1,445 rows (1.4.3 recorded 600 rows; the other two versions added 389 and 456 more). Rows that don't belong to the current version are skipped automatically during warm-up. Recording was done with warm-up off (`DM_PSO_MANIFEST=off`) and the directory holding the existing lists pointed at an empty directory (`DM_NVN_SHMAP`); otherwise rows already in a list would not be recorded.
- The list order in `nvn_mtl.m warm_start` is now: title list (the only thing the first frame waits for) → game list → save's list → locally collected list → seed. The seed has only shader libraries and goes last; the lists before it build pipelines as they go.

**Measured** (Mac mini M4, 1.4.3, same save copy, auto-load into the world). One run first to warm the cache, then two runs of each, alternating:

| | Old order (seed first) | New order (title list first) |
|---|---|---|
| First frame | 5.8 / 5.4 s | 2.0 / 2.0 s |
| Wait before first frame | 3.8 / 3.6 s | 0.1 / 0.1 s |
| All warm-up done | 5.0 / 4.7 s (mostly before first frame) | 8.8 / 10.1 s (behind the title screen) |
| Into the world | 27 / 29 s | 22 / 19 s |
| First load waits for warm-up | No | No |

**With a cold cache** (new Mac, or macOS purged the cache): the title list itself must compile, so the first frame takes a few seconds; then the whole seed compiles in the background. Single-threaded this took about 7.5 minutes; entering the world during it, the loading screen waits at most 15 seconds and the rest compiles during play, with hitches in new areas. This later became multithreaded; see "Parallel compilation in background warm-up".

**Launch proceeds when seed import fails.** Previously a failed seed import (corrupt seed file in the App, full disk, etc.) would `die` and exit with nothing on screen. Now it logs a line in red, the startup phase shows "Shader cache not ready", and the game starts as usual, translating shaders during play as before the seed existed. The import already writes shader files first and the index that references them last, so the cache stays usable if it fails midway.

Measured: in a copy of the App resources, append a newline to one seed file, then launch with an empty cache.

- Old binary: exits with status 2 before the first frame, log line `startup shader preparation: Startup shader seed integrity check failed`.
- New binary: title in 5 seconds, main menu normal, the cache holds the 240 shaders the title translated on the spot.

## The Metal cache (third correction, 2026-10-18 night)

Libraries Metal has compiled are stored in this user's system cache: one per App by bundle ID (`$(getconf DARWIN_USER_CACHE_DIR)<bundle id>/com.apple.metal`), and programs without a bundle share `…/C/com.apple.metal`. The game binary is swapped in by the launcher via `execv` and has no bundle, so it uses the shared one (this Mac has no separate one for `org.macnativegames.totk`). **Lookups in the shared one require, besides the same source and compile options, that the directory containing the program has a path of the same length**: only the length matters, not the path itself. Checked on the night of 2026-10-18 with 20 new sources (compiled at A, looked up from B; all English paths):

- Hit (20 / 20): a newly created directory at the same path (which is what repackaging in place does); directory renamed to a same-length name (`X`→`Y`, `pathtest`→`qathtest`); same length on another volume; same length, different depth; different file name or code signing identifier in the same directory; different current working directory; launched via a relative path (Metal uses the absolute path).
- Miss (0–2 / 20): directory name one character longer (`X`→`XX`), one level deeper, moved anywhere with a different length (same volume or another volume alike).

It looks as though Apple uses only the path length when computing the key (probably a bug); future macOS versions may change this. The explanations given earlier that day ("separate per directory", "shared by all programs", "separate per signing identifier") were each only half right: in those comparisons the directory lengths either happened to match or didn't.

So after moving the App (including into `/Applications`: the game's directory path goes from 102 characters to 68), the next launch is cold; repackaging in the original location keeps the cache.

**Only one program at a time can write to it.** Once a program connects to this cache (compiling one library keeps it connected until exit), libraries newly compiled by another program are visible while the first one stays open, and almost all vanish as soon as the first one exits (2 of 20 left). To see who is connected: `lsof $(getconf DARWIN_USER_CACHE_DIR)com.apple.metal/*/libraries.*`.

When this shared store fills up (data file fixed at about 1.08 GB) it evicts, aggressively and not least-recently-used: after another program compiled 3,000 new ones, only 6% of 500 compiled a minute earlier remained; compiling the same sources again from a directory with a different path length displaces the original copies (20 displaced 9–12). So any bundle-less program compiling thousands of new libraries at once can push the game's entries out. macOS also purges it entirely when the system disk fills.

To test a cold start, append a comment with a random string to the end of every source in a test copy of the translation cache; Metal has then compiled none of them, just like a new Mac. The thousands of new libraries compiled this way push out other entries (including those from the player's App directory), so afterwards use the precompile tool in the App's directory to recompile the player's translation cache. This method makes only "compiling shader libraries" cold; the system cache for pipeline building is not necessarily cold. Pipeline building always measured under 1 second; nearly all the time goes into compiling libraries. The program must run from its original location: a copy elsewhere (different path length) not only misses the original directory's cache but also cannot find the title list and seed list in `build/`.

## Parallel compilation in background warm-up (2026-10-18 evening, Claude)

**What changed.** The warm-up thread splits each list into 512-row chunks: first the shader libraries used by the chunk are compiled across several threads, then pipelines are built row by row in the original order. Pipeline building is still sequential and totals under 1 second; library compilation is what takes time. Supplementary cache warm-up is also parallel in chunks of 512.

- Thread count `TOTK_WARM_THREADS`: Mac default is cores minus 2, at most 8 (8 on M4); iOS default 1 (the phone loads precompiled metallibs and does not compile).
- At most 2 threads once in the world, so as not to compete with the game for cores.
- `shader_load` was already called concurrently by the warm-up and submit threads. Going multithreaded required three more changes: compile options are initialised only once (previously two threads calling for the first time at once could get options not yet set to precise math); two counters became atomic; logging takes a lock per line (lines interleaved with multiple threads).

**Measured** (Mac mini M4, 1.4.3, sitting at the title, no save loaded; random comment added to every source so Metal had compiled none):

| | 1 thread | 8 threads |
|---|---|---|
| Wait for title list before first frame (283 shaders) | 9.8 s | 2.4 / 2.3 s |
| Background warm-up done (9,268 shader libraries, 3,424 render pipelines) | 856 s (14.3 min) | 187.5 / 188.8 s (3.1 min) |
| Library compile time (summed over threads) | 849 s, 92 ms each on average | 1,453 / 1,470 s, 157–159 ms each on average |
| Pipeline build time | 0.74 s | 0.71 / 0.66 s |

8 threads is 4.6× faster than 1 (the two 8-thread runs differ by under 1%). Each library takes longer (92 → 157 ms) because 8 compile requests run at once and compete for cores (M4 has only 4 performance cores, and the game uses them too), but total time is shorter. With a warm cache (also sitting at the title): 8 threads finish in 1.6 s with a 47 ms wait before the first frame; 1 thread 12.2 s with a 265 ms wait. The first run after recompiling is slightly slower (8 threads 4.0 s, 1.5 s wait).

## Precompiling at package time (2026-10-18 night, Claude)

**What changed.** At the end of `build_app.py` (after macOS has checked the new App), a small tool built from `tools/warm_metal_cache.m` is copied into the directory of the game binary inside the App (`Contents/Resources/totk/build/`). There it compiles the seed's 8,290 sources with the same compile options as the game (precise math) on 8 threads, then in a new process samples 64 to check they hit, and finally deletes the tool (the App's signature covers this directory, so nothing may be left behind). The tool sits in the game binary's directory so its path has the same length as the game's (see "The Metal cache" above). Before compiling, `lsof` checks whether any other program is connected to this cache (a running game counts); if so the step is skipped and the program named. Any problem in this step never fails packaging; the game simply compiles in the background on first open as before. Packaging takes about 2 minutes longer (125 s on M4). When the Metal toolchain is installed and every seed shader has already been compiled into a library in the App (item 4 above, `step_shader_libs`), this step is skipped: the game loads those libraries directly and does not need the Metal cache.

**Measured** (Mac mini M4, 1.4.3, a new player's cache: seed only, random comment added to every source; game sitting at the title, no save loaded):

| Where the precompile tool runs | Tool compiles 8,290 | Game's full warm-up | Wait for title list before first frame |
|---|---|---|---|
| No precompile (same cold cache) | — | 175–188 s | 325–351 ms |
| Another directory (different path length) | 126 s | 181 s (no hits) | 325 ms |
| Another directory (different path length), signed with the game's signing identifier | 124 s | 175 s (no hits) | 330 ms |
| **The game binary's directory** | **125 s** | **4.4 s** (0.8 ms each) | **18 ms** |

**One failure, and the fix.** When the desktop App was repackaged at 21:33 on 2026-10-18, a small probe program of mine happened to start at the same moment as the precompile (it was queued waiting for macOS to scan the new App; when the scan ended both ran together and it connected to the cache first). The precompile compiled all 8,290, but the new process found only 1 / 64, and packaging correctly reported "Metal did not keep them". Afterwards, with no other program connected, the tool was copied into the game binary's directory inside the App and the player's whole translation cache (9,268) was compiled: 144 s, 64 / 64 hits in a new process. Starting from a save copy (running the game binary inside the App directly), background warm-up finished in 3.1–4.1 s, the wait before the first frame was 79–108 ms, and the world was reached in 43–60 s. Packaging now checks with `lsof` first (`metal_cache_users`).

**Limitations.**
- Only helps the Mac that did the packaging (the code is for this Mac's GPU); copied to another Mac, the App is cold as before;
- Moving the App after packaging breaks the match (to put it in `/Applications`, package straight there with `-o`);
- If another program is connected to the cache at package time, precompiling is skipped (packaging names the program);
- The system purging the cache, or another bundle-less program compiling a large batch of new libraries, also makes it cold;
- Only the seed is precompiled; new variants the player produces (in `pso_collected.tsv`) still compile on first encounter, and background warm-up compiles them on the next launch.

## A fresh iPhone install relies on the seed alone (2026-10-19 early morning, Claude)

**Problem.** After a fresh install the iPhone 14 showed a fully magenta screen: about 57,000 draws per 300 frames were skipped for missing shaders. iOS cannot translate during play and can only use the seed shipped in the App; and none of the 167 missing shaders were in BNSH files. They were in the agl engine's own shader archives `.sharcb` (102 in `Lib/agl/agl_resource…sarc.zs`, 39 in `Shader/ApplicationPackage…sarc.zs`, 6 each in `Lib/Terrain` and `Lib/gsys`), plus 14 in the BNSH embedded in the game's main module. The title screen's compositing and post-processing use these. The iPad had been filled in round by round with `fill_shaders.py`, which captured code from the device at runtime, so it never showed the problem.

**What changed.** `prepare_shader_seed.py` now parses `.sharcb` directly (see item 1 of "Build and launch behaviour" above) and also scans the game's program modules (`--modules rewritten/<version>`), accepting both 0x880 and 0x88c control sections. For 1.4.3 it parses 28,769 shaders in total, covering everything seen in the Mac player cache and the iPad fill-ins; agl and module shaders are also translated in their default binding state. The seed has 15,266 variants (12,472 default state + 2,794 from the shared list); 84 failed default translation (80 agl vertex shaders hit an array out-of-bounds in the translator, 2 an out-of-range parameter, 2 compute shaders a SPIRV-Cross null pointer). None of these shaders has appeared in any cache; the game has not used them so far.

**Import.** On first launch the seed is imported from the App into the local cache: each file's hash is checked, then it is copied. Copying now uses APFS clones (`clonefile`; the App and the cache are on the same volume): 44,744 files went from 59 s to 14 s (M4); checking still takes about 18 s. Both steps show a progress bar and count (`runtime_boot_count`).

**Verification.** The App can't be launched remotely while the device is locked, so this was simulated on the Mac: `TOTK_SHADER_NO_TRANSLATE=1` makes the Mac runtime use only the seed like the phone, skipping and recording requests for missing shaders, together with an empty `TOTK_SHADER_CACHE` and `TOTK_SHADER_SEED_DIR=build/shader-seed/v143`. An empty profile ran 36,000 frames from the new-game opening cutscene to the cave: not one draw was skipped for a missing shader, no translation requests were written, the picture was correct, and the bottom-left box read "New shaders 0 / total 15266". Retest on the real device pending its unlock.

## Retesting

Build the App normally with the existing command from the README; no new entry point or manual cache arguments are needed. The build log is `<work>/logs/prepare-shader-seed.log`. The first-launch log should show `[shader-seed]`, the actual count, the "startup-critical pipelines ready" line and the "pipeline warm-up complete" line. The "startup-critical pipelines ready" line covers only the title list: under 1 second of waiting with a warm cache, a few seconds with a cold one. The rest of warm-up finishes in the background; total time is on the "pipeline warm-up complete" line. When comparing launch times, first make sure both sides have the same Metal cache state (both warm or both cold); see "The Metal cache".

Full diagnostics use the production launcher, a separate empty profile/translation cache and the standard game slot, checking in turn the first launch, a reopen with the same cache, and a negative control with the seed disabled and an empty cache:

```sh
python3.14 games/totk/scripts/verify_first_launch_shaders.py \
  --app '/Volumes/mac-APFS/MacNativeGames-artifacts/apps/Tears of the Kingdom.app' \
  --output "$HOME/Library/Application Support/MacNativeGames/TOTK/tests/first-shader-check-new"
```

The output directory must be new. Real user input, or the user starting a new game, stops control immediately and keeps the test session. Completed launch phases are recorded separately; a later interrupted screenshot/wait is never rewritten as a full pass. The system Metal driver cache is not cleared, so this check verifies discovery/warm-up behaviour with an empty translation cache; it cannot serve as a cold-compile timing benchmark for a brand-new GPU.

CPU-only import diagnostics (no game or GPU launched) use `scripts/diag/import_shader_seed.m`, linked against Foundation and including `third_party/d2r_gfx/gfx/nvn`. Arguments are the seed directory in the bundle and a new private cache directory; two consecutive runs should import all files and then 0 files.

## Work directory size and what is kept

This full end-to-end verification put a separate build workspace at `analysis/v143-port/full-build-work`, bringing the analysis directory to about 36 GiB; that workspace was about 33 GiB, mainly two sets of unpacked data (base game and 1.4.3). `analysis/v112-port` is about 70 MiB, almost all `runtime-layout.json`. The rest of the 1.4.3 usage also includes profile copies from several rounds of scene tests, logs and binary diagnostic samples; this is not the size of the version port code or the shader seed itself. `du` may count blocks shared by APFS clones, so it can't be used directly to infer exclusive physical space.

A full build workspace belongs in `TOTK_build` next to the game image, or outside the source tree via `--work`; documents in the project keep only important conclusions, test methods and where external evidence lives, while raw analysis and run records stay outside the project. The initial read-only investigation details are in `shader-first-launch-8k/disk-usage-comparison.json`. The user then asked for leftovers that don't belong to the project to be cleaned up: the duplicate `full-build-work`, the old root-level `TOTK_build`, profile copies from multiple test rounds, regenerable binary diagnostic samples and temporary tool build caches were deleted; by pre-cleanup `du` this totals about 35 GiB, which is not the same as newly freed exclusive APFS space. `v143-port` went from about 36 GiB to about 753 MiB. Reproduction steps, reports, logs, screenshots, runtime layouts, the main development directory's unpacked/build output, the current App and the shader cache were kept; the real profile and pre-upgrade backup had identical hashes before and after, and no tracked file was deleted. Finder may recreate `.DS_Store` immediately; it is not cleaned repeatedly. The exact deletion list and checks are in `analysis/v143-port/cleanup-2026-10-18.json`. All these generated directories are ignored by Git; commits contain only source, tests and docs.

Later cleanup on 2026-10-18: the whole `games/totk/analysis` previously kept in the project (about 823 MiB, including the test directories in the screenshots and the 1.1.2/1.4.3 historical analysis) was moved out of the project to `/Volumes/mac-APFS/MacNativeGames-artifacts/2026-10-18/analysis`; no directory or symlink is left in the original place. File hashes and link targets matched before and after the move; the manifest is `analysis-relocation.json` next to the archive. Original paths in historical logs are left as they are. Relative paths of the evidence files above refer to that external archive; regenerate diagnostic data in an external workspace.

Final source-tree cleanup on 2026-10-18: the old `games/totk/build`, `extracted`, `rewritten` and temporary environment/tool links were moved to `/Volumes/mac-APFS/MacNativeGames-artifacts/development/totk`; the generated App moved to `/Volumes/mac-APFS/MacNativeGames-artifacts/apps/Tears of the Kingdom.app`. The single desktop entry now points directly at that external App. No corresponding large directories or links remain in the source tree; all tracked files had identical hashes before and after the move, and the App's runtime hash is unchanged. The project totals about 31 MiB (including about 14 MiB of `.git`); game data still lives outside the project, and this move is not claimed to have freed disk space. Move details are in the external `2026-10-18/source-tree-cleanup.json`. Use the production packager's external work directory for later builds and tests; don't generate diagnostic data directly in the source tree.
