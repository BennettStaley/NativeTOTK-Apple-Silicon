/* Player settings (the in-game settings menu): one table that every platform shares. Each row is a setting with its type,
   range or choices, default, platforms and how it applies: SET_LIVE takes effect at once through settings_apply (runtime/
   settings.c), SET_RESTART is handed to the game as its environment variable at the next start. The file is settings.txt
   (Mac: ~/Library/Application Support/MacNativeGames/TOTK/, iPhone / iPad: the app's Documents, visible in Files), one
   "key = value" line per setting in the controls.txt grammar; # starts a comment. Unknown keys and bad values are ignored
   (the default stays), so a file from a newer build still starts an older one. A variable already in the environment wins
   over the file (open --env TOTK_UNCAPPED_HZ=60 ... runs a test without touching the player's settings).
   Plain C, header only: the Mac launcher (app/launcher.m), the runtime and tests/test_settings.c include it. */
#pragma once
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { SET_MAC = 1, SET_IPAD = 2, SET_IPHONE = 4, SET_IOS = SET_IPAD | SET_IPHONE, SET_ALL = SET_MAC | SET_IOS };
enum { SET_BOOL, SET_RANGE, SET_CHOICE };
enum { SET_LIVE = 1, SET_RESTART = 2 };

/* token: the word in settings.txt; env: the environment value at start (NULL: leave the variable unset). */
typedef struct { const char *label, *token, *env; int value; } SettingChoice;
typedef struct {
    const char *key, *page, *label, *help;
    const char *env;                 /* the variable the game reads at start, or NULL for a live-only setting */
    int type, apply, platforms;
    int def_mac, def_ios;            /* defaults: the behaviour without a settings file */
    int min, max, step;              /* SET_RANGE */
    const char *unit;                /* SET_RANGE: shown after the value ("%", " fps") */
    const SettingChoice *choices; int nchoices;
    const char *unavailable;         /* why the row is greyed on a platform outside `platforms` */
    const char *shown_key; int shown_value;   /* shown only while that setting has that value (NULL: always) */
} SettingDef;

/* The menu: four top tabs, each a row of sections. A section either lists the settings whose page is its name
   (SETTINGS_PAGES, in menu order: settings.txt is grouped the same way) or is a page of its own (Mods, Saves, Cheats,
   Teleport, Towers, Shrines, Spawn), which holds no settings, so Reset never touches mods, saves, cheats or the spawner's favorites. The one
   exception is Teleport's story lock (teleport.story_lock): the page shows it at its top, and Reset puts it back on. */
static const char *const SETTINGS_PAGES[] = {"Display", "Frame rate", "Controls", "Audio", "Interface", "Image", "Effects",
                                             "World detail", "Downloads", "Teleport", "World", "Camera"};
enum { SETTINGS_NPAGES = sizeof SETTINGS_PAGES / sizeof *SETTINGS_PAGES };
/* Places (2026-10-28): the teleport sections had their own tab once Tools held nine and the bar cut their names. */
static const char *const SETTINGS_TABS[] = {"Game", "Graphics", "Content", "Tools", "Places"};
enum { SETTINGS_NTABS = sizeof SETTINGS_TABS / sizeof *SETTINGS_TABS };
enum { MENU_SETTINGS, MENU_MODS, MENU_SAVES, MENU_CHEATS, MENU_SPAWN, MENU_TELEPORT, MENU_TOWERS, MENU_SHRINES, MENU_LIGHTROOTS, MENU_TEARS, MENU_KOROKS };
typedef struct { int tab; const char *name; int kind; } SettingsSection;
static const SettingsSection SETTINGS_SECTIONS[] = {
    {0, "Display", MENU_SETTINGS}, {0, "Frame rate", MENU_SETTINGS}, {0, "Controls", MENU_SETTINGS}, {0, "Audio", MENU_SETTINGS},
    {0, "Interface", MENU_SETTINGS},
    {1, "Image", MENU_SETTINGS}, {1, "Effects", MENU_SETTINGS}, {1, "World detail", MENU_SETTINGS},
    {2, "Mods", MENU_MODS}, {2, "Saves", MENU_SAVES}, {2, "Downloads", MENU_SETTINGS},
    {3, "Cheats", MENU_CHEATS}, {3, "Spawn", MENU_SPAWN}, {3, "World", MENU_SETTINGS}, {3, "Camera", MENU_SETTINGS},
    {4, "Teleport", MENU_TELEPORT}, {4, "Towers", MENU_TOWERS}, {4, "Shrines", MENU_SHRINES}, {4, "Lightroots", MENU_LIGHTROOTS},
    {4, "Dragon's Tears", MENU_TEARS}, {4, "Koroks", MENU_KOROKS},
};
enum { SETTINGS_NSECTIONS = sizeof SETTINGS_SECTIONS / sizeof *SETTINGS_SECTIONS };
/* The first section of a tab and how many it has. */
static inline int settings_tab_first(int tab) { for (int i = 0; i < SETTINGS_NSECTIONS; i++) if (SETTINGS_SECTIONS[i].tab == tab) return i; return 0; }
static inline int settings_tab_sections(int tab) { int n = 0; for (int i = 0; i < SETTINGS_NSECTIONS; i++) n += SETTINGS_SECTIONS[i].tab == tab; return n; }
/* 1 when setting i belongs to a section of the tab (Reset this tab). */
static inline int settings_in_tab(int i, int tab);

/* Auto (steady): the highest rate that divides the display's and that the game holds (totk_autocap.h). */
static const SettingChoice SET_CAP[] = {{"Auto (fastest steady)", "auto", "auto", -1}, {"30 fps", "30", "30", 30}, {"40 fps", "40", "40", 40},
                                        {"60 fps", "60", "60", 60}, {"80 fps (variable refresh)", "80", "80", 80}, {"120 fps", "120", "120", 120}, {"Unlimited", "display", "display", 0}};
/* Variable refresh (nvn_mtl.m, totk_vrr.h; TOTK_VRR): Auto goes full screen on a ProMotion / Adaptive-Sync display. */
static const SettingChoice SET_VRR[] = {{"Auto", "auto", "auto", 1}, {"Off", "off", "0", 0}};
static const SettingChoice SET_INTERP[] = {{"Off", "off", "0", 0}, {"30 + 30 (shown at 60)", "interp30", "interp30", 1},
                                           {"On", "half", "interp", 2}, {"Every frame (unpaced, testing)", "max", "interpmax", 3},
                                           {"40 + 40 (shown at 80)", "interp40", "interp40", 4}, {"50 + 50 (shown at 100)", "interp50", "interp50", 5},
                                           {"60 + 60 (shown at 120)", "interp60", "interp60", 6}};   /* 4, 5: upstream's held rates (totk_mfx_mode.h) */
/* Motion / depth from the render drawn over the picture (totk_mfx.m mfx_debug_encode / mfx_debug_frame_encode,
   TOTK_MFX_DEBUG_VIEW), with frame interpolation on or off. The key graphics.mfx_debug_view stays for saved settings. */
static const SettingChoice SET_MFX_DEBUG[] = {{"Off", "off", NULL, 0}, {"Motion", "motion", "motion", 1}, {"Depth", "depth", "depth", 2},
                                              {"Split (picture, motion, depth)", "split", "split", 3}};
/* Display (settings.c display_apply): the output is the 16:9 picture the game composes, text and icons included (live up to
   the headroom); the render scale sizes the 3D scene against it; the aspect ratio is the shape it is shown at. */
enum { SET_OUT_WINDOW = 0, SET_OUT_NATIVE = 1, SET_OUT_CUSTOM = 2 };
static const SettingChoice SET_OUTPUT[] = {{"Match the window", "window", NULL, SET_OUT_WINDOW}, {"Display native", "native", NULL, SET_OUT_NATIVE},
                                           {"4K (3840 x 2160)", "2160", NULL, 2160}, {"1440p (2560 x 1440)", "1440", NULL, 1440},
                                           {"1080p (game's own)", "1080", NULL, 1080}, {"900p (1600 x 900)", "900", NULL, 900},
                                           {"720p (1280 x 720)", "720", NULL, 720}, {"Custom width", "custom", NULL, SET_OUT_CUSTOM}};
static const SettingChoice SET_SCALE[] = {{"The game's own", "game", NULL, 0}, {"25%", "25", NULL, 25}, {"33%", "33", NULL, 33},
                                          {"50%", "50", NULL, 50}, {"67%", "67", NULL, 67}, {"75%", "75", NULL, 75}, {"85%", "85", NULL, 85},
                                          {"100%", "100", NULL, 100}, {"125%", "125", NULL, 125}, {"150%", "150", NULL, 150},
                                          {"175%", "175", NULL, 175}, {"200%", "200", NULL, 200}};
static const SettingChoice SET_ASPECT[] = {{"Fill the screen", "auto", NULL, 0}, {"16:9 (game's own)", "16:9", NULL, 1},
                                           {"16:10", "16:10", NULL, 2}, {"21:9", "21:9", NULL, 3}, {"32:9", "32:9", NULL, 4},
                                           {"Native 16:10 (restart)", "native-16:10", NULL, 5}};
enum { SET_ASPECT_NATIVE = 5 };   /* settings_native_env: the game itself lays out for a 2560x1600 canvas */
static const SettingChoice SET_HEADROOM[] = {{"Auto (4K on 16 GB)", "auto", NULL, 0}, {"1440p", "1440", "2560x1440", 1440},
                                             {"4K", "2160", "3840x2160", 2160}};
/* The biggest 3D world (runtime_render_reserve, TOTK_RENDER_RESERVE): above 1440p the texture heap passes 1 GiB, so the
   launcher lets a memory pool's GPU buffer grow to 2 GiB and raises the GPU memory budget. Needs the 4K maximum output. */
static const SettingChoice SET_RENDER_MAX[] = {{"1440p (2560 x 1440)", "1440", NULL, 1440}, {"1800p (3200 x 1800)", "1800", "3200x1800", 1800},
                                               {"4K (3840 x 2160)", "2160", "3840x2160", 2160}};
static const SettingChoice SET_AA[] = {{"The game's own", "game", "0", 0}, {"Off", "off", "1", 1}, {"FXAA", "fxaa", "2", 2}, {"SMAA", "smaa", "3", 3}};
static const SettingChoice SET_SHADOW[] = {{"0.25x", "0.25", "0.25", -4}, {"0.5x", "0.5", "0.5", -2}, {"The game's own", "game", NULL, 0},
                                           {"2x", "2", "2", 2}, {"4x", "4", "4", 4}};
/* Dynamic resolution (TOTK_FIXED_RES, runtime/game_patches_v143.c patch_fixed_res): the game's own drops the 3D world to 90 or
   80 % under load; Off holds every step at 100 %. A code patch at start, so after a restart, and only on the Mac. */
static const SettingChoice SET_DYNRES[] = {{"On (the game's own)", "on", NULL, 0}, {"Off (always 100 %)", "off", "1", 1}};
static const SettingChoice SET_ANISO[] = {{"The game's", "game", "0", 0}, {"2x", "2", "2", 2}, {"4x", "4", "4", 4}, {"8x", "8", "8", 8}, {"16x", "16", "16", 16}};
static const SettingChoice SET_TOUCH[] = {{"Always", "always", NULL, 0}, {"Hide with a controller", "controller", NULL, 1},
                                          {"Until touched", "touch", NULL, 2}, {"Off", "off", NULL, 3}};
static const SettingChoice SET_PROMPTS[] = {{"Automatic (your controller)", "auto", NULL, 0}, {"Switch (the game's own)", "switch", NULL, 1},
                                            {"Xbox", "xbox", NULL, 2}, {"PlayStation", "playstation", NULL, 3}};
static const SettingChoice SET_DETAIL[] = {{"Simple", "simple", NULL, 0}, {"Detailed (average, 1% low)", "detailed", NULL, 1}};
static const SettingChoice SET_FACE[] = {{"Automatic", "auto", NULL, 0}, {"By position", "position", "position", 1},
                                         {"By label", "label", "label", 2}};
/* World (runtime/world_patch.h): the time of day's speed, in percent of the game's own (0 = the clock stands still). */
static const SettingChoice SET_TIME_SPEED[] = {{"Paused", "paused", "0", 0}, {"0.25x", "0.25", "25", 25}, {"0.5x", "0.5", "50", 50},
                                               {"Normal (1x)", "1", "100", 100}, {"2x", "2", "200", 200}, {"4x", "4", "400", 400},
                                               {"8x", "8", "800", 800}};
/* Graphics presets (settings.c): choosing one sets the settings in SETTINGS_PRESET_MEMBERS; changing any of them by hand
   shows Custom again, and so does a mix that matches no preset. Never stored as anything but what it matches. */
enum { SET_PRESET_CUSTOM = 0, SET_PRESET_BATTERY, SET_PRESET_BALANCED, SET_PRESET_QUALITY };
static const SettingChoice SET_PRESET[] = {{"Custom", "custom", NULL, SET_PRESET_CUSTOM}, {"Battery saver", "battery", NULL, SET_PRESET_BATTERY},
                                           {"Balanced", "balanced", NULL, SET_PRESET_BALANCED}, {"Quality", "quality", NULL, SET_PRESET_QUALITY}};
#define SET_N(a) (int)(sizeof a / sizeof *a)

static const SettingDef SETTINGS[] = {
    {"graphics.preset", "Image", "Preset", "Balanced is the game's own look. Sets the render scale, anti-aliasing, sharpening, filtering, shadows, render distance and frame rate limit together. Changing one of them shows Custom.",
     NULL, SET_CHOICE, SET_LIVE, SET_ALL, SET_PRESET_BALANCED, SET_PRESET_BALANCED, 0, 0, 0, NULL, SET_PRESET, SET_N(SET_PRESET), NULL, NULL, 0},
    {"display.output", "Display", "Display resolution", "The size of the finished picture. Text, icons and menus are drawn at this size, so they stay sharp; the 3D world is drawn at the render resolution below and stretched to fill it. Display native is your screen's own pixels.",
     NULL, SET_CHOICE, SET_LIVE, SET_MAC, 1080, 1080, 0, 0, 0, NULL, SET_OUTPUT, SET_N(SET_OUTPUT), "iPhone and iPad show 720p.", NULL, 0},
    {"display.custom_width", "Display", "Custom width", "The output's width in pixels; the height follows at 16:9.",
     NULL, SET_RANGE, SET_LIVE, SET_MAC, 2560, 2560, 640, 3840, 64, " px", NULL, 0, "iPhone and iPad show 720p.", "display.output", SET_OUT_CUSTOM},
    {"display.scale", "Display", "Render resolution", "The size the 3D world is drawn at, as a share of the display resolution: lower is faster but softer, higher is sharper and slower. Text and icons stay at the display resolution. The 3D world goes up to the Maximum render resolution (1440p unless raised); each choice shows the size it gives now.",
     NULL, SET_CHOICE, SET_LIVE, SET_ALL, 0, 0, 0, 0, 0, NULL, SET_SCALE, SET_N(SET_SCALE), NULL, NULL, 0},
    {"display.aspect", "Display", "Aspect ratio", "Fill the screen shows more of the world on a wider or taller screen; text and icons keep their shape. Key 4 also cycles it. Native 16:10 (a MacBook screen; restart) has the game itself draw a 16:10 picture and lay its menus out for it: the HUD at the real top and bottom, menus, fades and backdrops filling the screen. Pre-rendered cutscenes play stretched and in-game photos come out squished, as with the community's ultrawide mods.",
     NULL, SET_CHOICE, SET_LIVE, SET_MAC, 1, 1, 0, 0, 0, NULL, SET_ASPECT, SET_N(SET_ASPECT), "Not on iPhone and iPad yet.", NULL, 0},
    {"display.headroom", "Display", "Maximum output", "The biggest output resolution you can switch to while playing. Higher sets more memory aside; takes effect after a restart.",
     "TOTK_OUTPUT_RES", SET_CHOICE, SET_RESTART, SET_MAC, 0, 0, 0, 0, 0, NULL, SET_HEADROOM, SET_N(SET_HEADROOM), "Fixed on iPhone and iPad.", NULL, 0},
    {"display.render_max", "Display", "Maximum render resolution", "The biggest the 3D world can be drawn at; set Render resolution to reach it. Above 1440p needs the 4K maximum output and costs memory and speed: on an 18 GB M3 Pro, 1800p ran at 60 fps and used about 6 of 7.2 GB of GPU memory; 4K needs about 1.5 GB more and runs slower. The top of the menu shows GPU memory against the limit the game stops at; a choice above the measured use is marked (not enough GPU memory). Experimental.",
     "TOTK_RENDER_RESERVE", SET_CHOICE, SET_RESTART, SET_MAC, 1440, 1440, 0, 0, 0, NULL, SET_RENDER_MAX, SET_N(SET_RENDER_MAX), "Fixed on iPhone and iPad.", NULL, 0},
    {"graphics.fov", "World detail", "Field of view", "How wide the camera sees, against the game's own (version 1.4.3).",
     "TOTK_FOV", SET_RANGE, SET_LIVE, SET_ALL, 100, 100, 70, 140, 5, "%", NULL, 0, NULL, NULL, 0},
    {"graphics.fog", "Effects", "Cave fog", "The fog in caves and other enclosed places, against the game's own: 50% reaches it at twice the distance, 0% has none. The outdoor haze is separate and not changed yet (version 1.4.3).",
     "TOTK_FOG", SET_RANGE, SET_LIVE, SET_ALL, 100, 100, 0, 100, 10, "%", NULL, 0, NULL, NULL, 0},
    {"graphics.shadow_res", "World detail", "Shadow resolution", "The sun's shadow map: 0.5x and 0.25x are below the game's own quality (smaller maps, blurrier shadows; measured no faster: the shadow passes cost their draws, not their size); 2x and 4x give sharper shadow edges and use more memory (version 1.4.3).",
     "TOTK_SHADOW_RES", SET_CHOICE, SET_LIVE, SET_MAC, 0, 0, 0, 0, 0, NULL, SET_SHADOW, SET_N(SET_SHADOW), "Not on iPhone and iPad yet.", NULL, 0},
    {"graphics.aa", "Image", "Anti-aliasing", "Smooths the 3D scene's edges before the game draws its text and icons on top (version 1.4.3).",
     "TOTK_AA", SET_CHOICE, SET_LIVE, SET_ALL, 0, 0, 0, 0, 0, NULL, SET_AA, SET_N(SET_AA), NULL, NULL, 0},
    {"graphics.sharpen", "Image", "Sharpening", "AMD FidelityFX CAS on the finished picture, under the text and icons.",
     "TOTK_SHARPEN", SET_RANGE, SET_LIVE, SET_ALL, 0, 0, 0, 100, 5, "%", NULL, 0, NULL, NULL, 0},
    {"graphics.aniso", "Image", "Anisotropic filtering", "Sharper ground and walls seen at an angle.",
     "TOTK_ANISO", SET_CHOICE, SET_LIVE, SET_ALL, 0, 0, 0, 0, 0, NULL, SET_ANISO, SET_N(SET_ANISO), NULL, NULL, 0},
    {"graphics.dof", "Effects", "Depth of field", "The game's blur of the distance. Off keeps far away sharp (version 1.4.3).",
     NULL, SET_BOOL, SET_LIVE, SET_ALL, 1, 1, 0, 1, 1, NULL, NULL, 0, NULL, NULL, 0},
    {"graphics.bloom", "Effects", "Bloom", "The glow around bright light, against the game's own (version 1.4.3).",
     NULL, SET_RANGE, SET_LIVE, SET_ALL, 100, 100, 0, 100, 10, "%", NULL, 0, NULL, NULL, 0},
    {"graphics.lens_flare", "Effects", "Lens flare", "The game's rings and glints across the picture when the sun is in view (version 1.4.3).",
     NULL, SET_BOOL, SET_LIVE, SET_ALL, 1, 1, 0, 1, 1, NULL, NULL, 0, NULL, NULL, 0},
    {"graphics.render_distance", "World detail", "Render distance", "How far the camera draws before distant land is cut off, against the game's own 25 km (version 1.4.3); below 50% is below the game's own quality: far land is cut off visibly, in exchange for fewer things drawn. The game's own is the most: farther breaks the horizon seen from high in the sky.",
     "TOTK_RENDER_DISTANCE", SET_RANGE, SET_LIVE, SET_ALL, 100, 100, 10, 100, 10, "%", NULL, 0, NULL, NULL, 0},
    {"graphics.dynamic_res", "World detail", "Dynamic resolution", "The game lowers the 3D world to 90 or 80 % of the render resolution when frames run long; Off keeps it at 100 %, sharper but slower in busy places (version 1.4.3).",
     "TOTK_FIXED_RES", SET_CHOICE, SET_RESTART, SET_MAC, 0, 0, 0, 0, 0, NULL, SET_DYNRES, SET_N(SET_DYNRES), "Not on iPhone and iPad: their game code is fixed when the app is built.", NULL, 0},
    {"framerate.cap", "Frame rate", "Frame rate limit", "How many frames a second the game draws. Auto picks the fastest rate the game holds steadily: with variable refresh (below) 120, 80, 60 or 48 on the built-in display; without it 120, 60, 40 or 30 on a 120 Hz display, so frames never alternate between one and two refreshes. 80 fps is even only with variable refresh. Unlimited draws as fast as it can (uneven on a fixed-refresh display). With frame interpolation the limit is what you see: the game runs at half of it.",
     "TOTK_UNCAPPED_HZ", SET_CHOICE, SET_LIVE, SET_ALL, 80, 0, 0, 0, 0, NULL, SET_CAP, SET_N(SET_CAP), NULL, NULL, 0},   /* Mac default 80: with interpolation On and variable refresh, game 40 shown at an even 80 (2026-10-23) */
    {"framerate.vrr", "Frame rate", "Variable refresh (full screen)", "On a ProMotion or Adaptive-Sync display the screen refreshes when each frame is ready, so rates such as 80 fps are shown evenly. Needs full screen: Auto enters it when turned on and at launch (leave it with the green button and the game stays windowed). Works with frame interpolation (On). The row says whether it is active now and at what rate.",
     "TOTK_VRR", SET_CHOICE, SET_LIVE, SET_MAC, 1, 0, 0, 0, 0, NULL, SET_VRR, SET_N(SET_VRR), "Mac only.", NULL, 0},
    {"graphics.interpolation", "Frame rate", "Frame interpolation", "MetalFX makes a picture between every two game frames, so you see twice the game's rate (for example game 40, shown 80). On: paced with the frame rate limit and variable refresh. 30 + 30 to 60 + 60: the game held at 30, 40, 50 or 60 and shown at twice that, without variable refresh. Every frame: unpaced, for testing. Near the GPU memory limit it pauses (resuming once memory frees up, off for the session after 3 pauses); a mode that would not fit is marked and not turned on. The row says when the chosen mode is not running.",
     "TOTK_MFX", SET_CHOICE, SET_LIVE, SET_MAC | SET_IPAD, 2, 0, 0, 0, 0, NULL, SET_INTERP, SET_N(SET_INTERP),
     "Not tested on iPhone yet.", NULL, 0},
    {"graphics.obj_motion", "Frame rate", "Per-object motion", "With frame interpolation, moving characters, animals and props get their own motion instead of only the camera's, so they smear less in the made-up pictures. Costs a little GPU time; the first start after turning it on compiles the game's shaders again (a few minutes).",
     "TOTK_OBJ_MOTION", SET_BOOL, SET_RESTART, SET_MAC, 0, 0, 0, 1, 1, NULL, NULL, 0, "Mac only for now.", NULL, 0},
    {"graphics.mfx_debug_view", "Frame rate", "Motion / depth debug view", "Draws the motion and depth taken from the game's render over the picture: motion (direction as colour, brighter is faster, outlined where per-object motion was used), depth (near is bright), or the picture, motion and depth side by side. Works with frame interpolation on or off; with it on, this is what MetalFX is given.",
     "TOTK_MFX_DEBUG_VIEW", SET_CHOICE, SET_LIVE, SET_MAC | SET_IPAD, 0, 0, 0, 0, 0, NULL, SET_MFX_DEBUG, SET_N(SET_MFX_DEBUG),
     "Not tested on iPhone yet.", NULL, 0},
    {"graphics.lod_bias", "Image", "Texture detail bias", "Below 0 picks sharper texture levels, in hundredths of a level; too low shimmers.",
     "TOTK_LOD_BIAS", SET_RANGE, SET_LIVE, SET_ALL, 0, 0, -100, 0, 25, NULL, NULL, 0, NULL, NULL, 0},
    {"interface.readout", "Interface", "Performance readout", "Frames per second and the game picture's size.",
     "TOTK_HINTS", SET_BOOL, SET_LIVE, SET_ALL, 1, 1, 0, 1, 1, NULL, NULL, 0, NULL, NULL, 0},
    {"interface.readout_detail", "Interface", "Readout detail", "Detailed adds the average and the 1% low frame rate over the last 5 seconds.",
     NULL, SET_CHOICE, SET_LIVE, SET_ALL, 0, 0, 0, 0, 0, NULL, SET_DETAIL, SET_N(SET_DETAIL), NULL, "interface.readout", 1},
    {"interface.hide_hud", "Camera", "Hide the HUD", "Hides the game's hearts, map, prompts and menus for clean screenshots (F2 on a Mac keyboard). Turn it off to use menus.",
     NULL, SET_BOOL, SET_LIVE, SET_ALL, 0, 0, 0, 1, 1, NULL, NULL, 0, NULL, NULL, 0},
    {"interface.shader_counter", "Interface", "Shader counter", "New shaders collected while you play.",
     "TOTK_SHADER_OVERLAY", SET_BOOL, SET_LIVE, SET_ALL, 1, 0, 0, 1, 1, NULL, NULL, 0, NULL, NULL, 0},
    /* The shared shader list (third_party/d2r_gfx/gfx/nvn/totk_shader_list.m): once a day the list of variants players met
       is downloaded and the new ones are prepared from the player's own game files in the background. Download only. */
    {"interface.download_shaders", "Downloads", "Download new shaders", "Once a day, gets the shared list of shaders other players met and prepares the new ones from your game files in the background, so you do not stutter there. Download only: nothing is uploaded.",
     NULL, SET_BOOL, SET_LIVE, SET_MAC, 1, 0, 0, 1, 1, NULL, NULL, 0, "iPhone and iPad cannot prepare shaders while playing; their app carries the list from its build.", NULL, 0},
    /* The community cheat list (runtime/cheat_db_fetch.h): once a day this game version's part of sthetix/nx-cheats-db is
       downloaded for the Cheats tab. Download only. */
    {"interface.download_cheats", "Downloads", "Download cheat list", "Once a day, gets the community cheat list for this game version, so the Cheats tab offers its cheats without any files. Download only: nothing is uploaded.",
     NULL, SET_BOOL, SET_LIVE, SET_MAC, 1, 0, 0, 1, 1, NULL, NULL, 0, "Cheats are on the Mac only for now.", NULL, 0},
    {"interface.touch_controls", "Interface", "Touch controls", "When the on-screen controller shows.",
     NULL, SET_CHOICE, SET_LIVE, SET_IOS, 1, 1, 0, 0, 0, NULL, SET_TOUCH, SET_N(SET_TOUCH), "Only on iPhone and iPad.", NULL, 0},
    {"interface.touch_opacity", "Interface", "Touch control opacity", NULL,
     NULL, SET_RANGE, SET_LIVE, SET_IOS, 32, 32, 10, 100, 5, "%", NULL, 0, "Only on iPhone and iPad.", NULL, 0},
    {"interface.touch_size", "Interface", "Touch control size", NULL,
     NULL, SET_RANGE, SET_LIVE, SET_IOS, 100, 100, 75, 150, 5, "%", NULL, 0, "Only on iPhone and iPad.", NULL, 0},
    {"interface.menu_button", "Interface", "Settings button", "A small gear at the top of the screen. Three fingers also open the menu.",
     NULL, SET_BOOL, SET_LIVE, SET_IOS, 1, 1, 0, 1, 1, NULL, NULL, 0, "Only on iPhone and iPad.", NULL, 0},
    {"controls.prompts", "Controls", "Button prompts", "The button icons the game shows. Xbox and PlayStation use a button-icon mod from the Mods tab (e.g. \"Xbox UI for v1.4.3\"). Automatic follows the last controller you connected.",
     NULL, SET_CHOICE, SET_RESTART, SET_ALL, 0, 0, 0, 0, 0, NULL, SET_PROMPTS, SET_N(SET_PROMPTS), NULL, NULL, 0},
    {"controls.face_buttons", "Controls", "Face buttons", "PlayStation and Xbox buttons by position (as the game's prompts show them) or by their letters.",
     "TOTK_FACE_BUTTONS", SET_CHOICE, SET_LIVE, SET_ALL, 0, 0, 0, 0, 0, NULL, SET_FACE, SET_N(SET_FACE), NULL, NULL, 0},
    {"controls.camera_speed_x", "Controls", "Camera speed, horizontal", "The right stick's left and right against the game's own. Above 100% a partial push turns faster, up to the game's top speed; the arrow keys are already at it and only slow down below 100%.",
     NULL, SET_RANGE, SET_LIVE, SET_ALL, 100, 100, 25, 300, 5, "%", NULL, 0, NULL, NULL, 0},
    {"controls.camera_speed_y", "Controls", "Camera speed, vertical", "The right stick's up and down against the game's own. Above 100% a partial push turns faster, up to the game's top speed.",
     NULL, SET_RANGE, SET_LIVE, SET_ALL, 100, 100, 25, 300, 5, "%", NULL, 0, NULL, NULL, 0},
    {"controls.rumble", "Controls", "Rumble", NULL,
     "TOTK_RUMBLE", SET_BOOL, SET_LIVE, SET_MAC, 1, 1, 0, 1, 1, NULL, NULL, 0, "Not on iPhone and iPad yet.", NULL, 0},
    {"controls.gyro", "Controls", "Motion controls", "Gyro aiming with a controller that has a gyroscope.",
     "TOTK_GYRO", SET_BOOL, SET_LIVE, SET_MAC, 1, 1, 0, 1, 1, NULL, NULL, 0, "Not on iPhone and iPad yet.", NULL, 0},
    {"audio.volume", "Audio", "Volume", NULL,
     NULL, SET_RANGE, SET_LIVE, SET_ALL, 100, 100, 0, 100, 5, "%", NULL, 0, NULL, NULL, 0},
    /* Tools › Teleport, Towers, Shrines and Lightroots (settings_panel_mac.h buildTeleport / buildPlaces, landmarks.h landmarks_lock): places the newest save has not reached
       in the story are greyed, with the reason, and cannot be flown to. Map pins never are. */
    {"teleport.story_lock", "Teleport", "Only places the story has opened", "Greys out the places your last save has not reached in the story, so a teleport cannot skip ahead: everything off Great Sky Island until you leave it, the Depths until you have been down, a temple until its region's quest has started. Map pins are never locked.",
     NULL, SET_BOOL, SET_LIVE, SET_MAC, 1, 1, 0, 1, 1, NULL, NULL, 0, "Teleport is on the Mac only.", NULL, 0},
    {"teleport.direct", "Teleport", "Teleport straight there", "Puts Link at the place at once instead of flying him there with noclip. Faster, but a far place is not loaded yet: the game may treat him as fallen out of the world and put him back where he stood. Off: the flight, which loads the world on the way.",
     NULL, SET_BOOL, SET_LIVE, SET_MAC, 0, 0, 0, 1, 1, NULL, NULL, 0, "Teleport is on the Mac only.", NULL, 0},
    {"world.time_speed", "World", "Time of day speed", "How fast the day passes: the clock, the sun and the sky (version 1.4.3). Paused stops the time of day.",
     "TOTK_TIME_SPEED", SET_CHOICE, SET_LIVE, SET_ALL, 100, 100, 0, 0, 0, NULL, SET_TIME_SPEED, SET_N(SET_TIME_SPEED), NULL, NULL, 0},
};
enum { SETTINGS_COUNT = sizeof SETTINGS / sizeof *SETTINGS };

static inline int settings_in_tab(int i, int tab) {
    for (int k = 0; k < SETTINGS_NSECTIONS; k++)
        if (SETTINGS_SECTIONS[k].tab == tab && (SETTINGS_SECTIONS[k].kind == MENU_SETTINGS || SETTINGS_SECTIONS[k].kind == MENU_TELEPORT)
            && !strcmp(SETTINGS[i].page, SETTINGS_SECTIONS[k].name)) return 1;
    return 0;
}
static inline int settings_index(const char *key) {
    for (int i = 0; i < SETTINGS_COUNT; i++) if (!strcmp(SETTINGS[i].key, key)) return i;
    return -1;
}
static inline int settings_default(const SettingDef *d, int ios) { return ios ? d->def_ios : d->def_mac; }
/* What each preset sets (Battery saver, Balanced, Quality). SET_PRESET_DEFAULT: the platform's default for that setting. */
#define SET_PRESET_DEFAULT (-1000000)
typedef struct { const char *key; int value[3]; } SettingPresetMember;
static const SettingPresetMember SETTINGS_PRESET_MEMBERS[] = {
    {"display.scale", {67, 0, 100}},
    {"graphics.aa", {0, 0, 3}},
    {"graphics.sharpen", {0, 0, 20}},
    {"graphics.aniso", {0, 0, 16}},
    {"graphics.shadow_res", {0, 0, 2}},
    {"graphics.render_distance", {100, 100, 100}},
    {"framerate.cap", {30, SET_PRESET_DEFAULT, SET_PRESET_DEFAULT}},
};
enum { SETTINGS_PRESET_NMEMBERS = sizeof SETTINGS_PRESET_MEMBERS / sizeof *SETTINGS_PRESET_MEMBERS };
/* The value preset p (SET_PRESET_BATTERY..) gives member m, or INT_MIN when the member is not on this platform. */
static inline int settings_preset_value(int p, int m, int ios) {
    int i = settings_index(SETTINGS_PRESET_MEMBERS[m].key);
    if (i < 0 || !(SETTINGS[i].platforms & (ios ? SET_IOS : SET_MAC))) return -2147483647 - 1;
    int v = SETTINGS_PRESET_MEMBERS[m].value[p - SET_PRESET_BATTERY];
    return v == SET_PRESET_DEFAULT ? settings_default(&SETTINGS[i], ios) : v;
}
/* The preset values match, or SET_PRESET_CUSTOM. */
static inline int settings_preset_match(const int *values, int ios) {
    for (int p = SET_PRESET_BATTERY; p <= SET_PRESET_QUALITY; p++) {
        int all = 1;
        for (int m = 0; m < SETTINGS_PRESET_NMEMBERS && all; m++) {
            int v = settings_preset_value(p, m, ios);
            if (v != -2147483647 - 1 && values[settings_index(SETTINGS_PRESET_MEMBERS[m].key)] != v) all = 0;
        }
        if (all) return p;
    }
    return SET_PRESET_CUSTOM;
}
static inline const SettingChoice *settings_choice(const SettingDef *d, int value) {
    for (int i = 0; i < d->nchoices; i++) if (d->choices[i].value == value) return &d->choices[i];
    return NULL;
}
static inline int settings_valid(const SettingDef *d, int v) {
    if (d->type == SET_BOOL) return v == 0 || v == 1;
    if (d->type == SET_CHOICE) return settings_choice(d, v) != NULL;
    return v >= d->min && v <= d->max;
}
/* A value from its settings.txt word: 0 on success. Ranges are clamped and rounded to their step. */
static inline int settings_parse_value(const SettingDef *d, const char *word, int *out) {
    if (d->type == SET_BOOL) {
        if (!strcmp(word, "on") || !strcmp(word, "1") || !strcmp(word, "true")) { *out = 1; return 0; }
        if (!strcmp(word, "off") || !strcmp(word, "0") || !strcmp(word, "false")) { *out = 0; return 0; }
        return -1;
    }
    if (d->type == SET_CHOICE) {
        for (int i = 0; i < d->nchoices; i++) if (!strcmp(word, d->choices[i].token)) { *out = d->choices[i].value; return 0; }
        return -1;
    }
    char *end; long v = strtol(word, &end, 10);
    if (end == word || *end) return -1;
    if (v < d->min) v = d->min;
    if (v > d->max) v = d->max;
    *out = d->min + (int)((v - d->min) / d->step) * d->step;
    return 0;
}
/* The value from the environment variable's own words (the game's meaning): 0 on success. */
static inline int settings_parse_env(const SettingDef *d, const char *text, int *out) {
    if (d->type == SET_CHOICE) {
        for (int i = 0; i < d->nchoices; i++) if (d->choices[i].env && !strcmp(text, d->choices[i].env)) { *out = d->choices[i].value; return 0; }
        return -1;
    }
    return settings_parse_value(d, text, out);
}
static inline const char *settings_token(const SettingDef *d, int v, char *buf, size_t n) {
    if (d->type == SET_BOOL) return v ? "on" : "off";
    if (d->type == SET_CHOICE) { const SettingChoice *c = settings_choice(d, v); return c ? c->token : "?"; }
    snprintf(buf, n, "%d", v); return buf;
}
/* What the game reads at start for this value, or NULL to leave the variable unset. */
static inline const char *settings_env_text(const SettingDef *d, int v, char *buf, size_t n) {
    if (d->type == SET_BOOL) return v ? "1" : "0";
    if (d->type == SET_CHOICE) { const SettingChoice *c = settings_choice(d, v); return c ? c->env : NULL; }
    snprintf(buf, n, "%d", v); return buf;
}

static inline void settings_defaults(int *values, int ios) {
    for (int i = 0; i < SETTINGS_COUNT; i++) values[i] = settings_default(&SETTINGS[i], ios);
}
/* Reads settings.txt text over values (start from settings_defaults). Returns the number of settings it set. */
static inline int settings_parse_text(const char *text, int *values) {
    int count = 0;
    while (text && *text) {
        const char *eol = strchr(text, '\n'); size_t len = eol ? (size_t)(eol - text) : strlen(text);
        char line[256]; if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, text, len); line[len] = 0; text = eol ? eol + 1 : text + len;
        char *hash = strchr(line, '#'); if (hash) *hash = 0;
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; char *key = line, *word = eq + 1;
        while (isspace((unsigned char)*key)) key++;
        for (char *e = key + strlen(key); e > key && isspace((unsigned char)e[-1]); ) *--e = 0;
        while (isspace((unsigned char)*word)) word++;
        for (char *e = word + strlen(word); e > word && isspace((unsigned char)e[-1]); ) *--e = 0;
        int i = settings_index(key), v;
        if (i >= 0 && !settings_parse_value(&SETTINGS[i], word, &v)) { values[i] = v; count++; }
    }
    return count;
}
/* settings.txt for values: a header, then every setting, grouped by page. Returns the length (snprintf rules). */
static inline int settings_format_text(const int *values, char *out, size_t n) {
    size_t used = 0; char buf[16];
#define SET_PUT(...) do { int k = snprintf(out + (used < n ? used : n), used < n ? n - used : 0, __VA_ARGS__); if (k > 0) used += (size_t)k; } while (0)
    SET_PUT("# Tears of the Kingdom settings. The in-game settings menu writes this file; edits here apply at the next start.\n");
    for (int p = 0; p < SETTINGS_NPAGES; p++) {
        SET_PUT("\n# %s\n", SETTINGS_PAGES[p]);
        for (int i = 0; i < SETTINGS_COUNT; i++)
            if (!strcmp(SETTINGS[i].page, SETTINGS_PAGES[p])) SET_PUT("%s = %s\n", SETTINGS[i].key, settings_token(&SETTINGS[i], values[i], buf, sizeof buf));
    }
#undef SET_PUT
    return (int)used;
}
/* Values the environment already sets win over the file: the menu shows what the game is running with. */
static inline void settings_take_env(int *values) {
    for (int i = 0; i < SETTINGS_COUNT; i++) {
        const char *e = SETTINGS[i].env ? getenv(SETTINGS[i].env) : NULL; int v;
        if (e && *e && !settings_parse_env(&SETTINGS[i], e, &v)) values[i] = v;
    }
}
/* Hands every setting the game reads at start to it as its environment variable, unless the variable is already set. */
/* Native 16:10 (display.aspect, 2026-10-26): the game draws a 2560x1600 canvas with its UI layouts edited for it
   (scripts/tall_layout.py, built into extracted/v143/romfs-patches-2560x1600 by build_app.py) and heaps for that canvas and
   a 2560x1600 scene (romfs-patches-out/2560x1600-scene-2560x1600). Nothing is squeezed (TOTK_UI_SQUEEZE=0 turns off the
   per-draw squeeze and the UI-layer composite); the camera takes 16:10 (TOTK_ASPECT). Variables already set win. Only
   when the build made those files (an older app keeps Fill the screen); cwd is the game folder (launcher / loader). */
static inline int settings_native_env(const int *values) {
    int a = settings_index("display.aspect");
    if (a < 0 || values[a] != SET_ASPECT_NATIVE) return 0;
    if (access("extracted/v143/romfs-patches-2560x1600/manifest.tsv", R_OK) || access("extracted/v143/romfs-patches-out/2560x1600-scene-2560x1600/manifest.tsv", R_OK)) {
        fprintf(stderr, "[settings] Native 16:10: this app was built without its layouts (rebuild it); Fill the screen instead\n");
        return 0;
    }
    setenv("TOTK_NATIVE_ASPECT", "16:10", 0);
    setenv("TOTK_OUTPUT_RES", "2560x1600", 0); setenv("TOTK_RENDER_RESERVE", "2560x1600", 0);
    setenv("TOTK_ROMFS_PATCHES", "extracted/v143/romfs-patches-2560x1600", 0);
    setenv("TOTK_UI_SQUEEZE", "0", 0); setenv("TOTK_ASPECT", "16:10", 0);
    return 1;
}
static inline void settings_export_env(const int *values) {
    char buf[16];
    settings_native_env(values);   /* before the loop: its output size and reserve take the place of the headroom's */
    for (int i = 0; i < SETTINGS_COUNT; i++) {
        const SettingDef *d = &SETTINGS[i];
        if (!d->env || getenv(d->env)) continue;
        const char *text = settings_env_text(d, values[i], buf, sizeof buf);
        if (text) setenv(d->env, text, 1);
    }
}
