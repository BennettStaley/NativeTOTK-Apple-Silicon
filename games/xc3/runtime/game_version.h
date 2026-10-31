#pragma once
#define GAME_XC3 1
#define GAME_TITLE_ID 0x010074F013262000ULL
/* Inherited from games/ot0 (a game that reserves 62 GiB): the alias region above 2^39. Xenoblade Chronicles 3 asks for a
   39-bit address space; it boots and plays with the region kept, its allocations sitting at 0x90_...-0x91_.... */
#define GUEST_ALIAS_HIGH 1
/* Names the host files copied from games/totk (frontend.m, settings_panel.h, contrib_submit.h) use. */
#define GAME_NAME "Xenoblade Chronicles 3"
#define GAME_FOLDER "XC3"   /* ~/Library/Application Support/MacNativeGames/XC3 */
#define GAME_LOG "xc3"      /* ~/Library/Logs/MacNativeGames/xc3.log */
#define GAME_CONTRIB_TEMPLATE ""   /* no shared shader list for this game: the panel's contribution button is hidden */
#define GAME_CONTRIB_TITLE "Shader list (Xenoblade Chronicles 3)"
/* No per-version addresses yet, on purpose (as in games/dread): an address left over from another game must not compile.
   Each one is added here, per version, once it is found and checked in this game's own code. */
#if defined(XC3_GAME_V100)
#define TOTK_GAME_VERSION "1.0.0"
#define TOTK_GAME_DIR "v100"
#elif defined(XC3_GAME_V221)
#define TOTK_GAME_VERSION "2.2.1"
#define TOTK_GAME_DIR "v221"
#else
#error "build with -DXC3_GAME_V100 or -DXC3_GAME_V221 (the Makefile's VERSION)"
#endif
