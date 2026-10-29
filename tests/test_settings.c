#include <limits.h>
/* Test the settings table and settings.txt (games/totk/runtime/settings_schema.h) with synthetic files. */
#include "../games/totk/runtime/settings_schema.h"
#include <stdio.h>

static int failures;
static void expect(int ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; } }
static int value(const int *values, const char *key) { return values[settings_index(key)]; }

int main(void) {
    int v[SETTINGS_COUNT], again[SETTINGS_COUNT];
    for (int i = 0; i < SETTINGS_COUNT; i++) {
        const SettingDef *d = &SETTINGS[i];
        expect(settings_valid(d, d->def_mac) && settings_valid(d, d->def_ios), "every default is a valid value");
        int on_page = 0;
        for (int p = 0; p < SETTINGS_NPAGES; p++) on_page |= !strcmp(d->page, SETTINGS_PAGES[p]);
        expect(on_page, "every setting is on a known page");
        for (int j = 0; j < i; j++) expect(strcmp(d->key, SETTINGS[j].key) != 0, "keys are unique");
        expect(d->platforms != 0 && (d->platforms == SET_ALL || d->unavailable), "a setting missing on a platform says why");
        if (d->type == SET_RANGE) expect(d->step > 0 && (d->max - d->min) % d->step == 0, "a range is whole steps");
    }

    settings_defaults(v, 1);
    expect(value(v, "framerate.cap") == 0 && value(v, "interface.shader_counter") == 0, "iOS defaults: display rate, no shader counter");
    settings_defaults(v, 0);
    expect(value(v, "framerate.cap") == 80 && value(v, "graphics.interpolation") == 2 && value(v, "audio.volume") == 100, "Mac defaults: limit 80 with frame interpolation On");
    {   /* Auto (steady): its word in settings.txt and in TOTK_UNCAPPED_HZ, the fixed limits still read */
        int a[SETTINGS_COUNT]; settings_defaults(a, 1);
        expect(settings_parse_text("framerate.cap = auto\n", a) == 1 && value(a, "framerate.cap") == -1, "auto by its word");
        expect(settings_parse_text("framerate.cap = 120\n", a) == 1 && value(a, "framerate.cap") == 120, "120 still reads");
        char b[16]; const SettingDef *d = &SETTINGS[settings_index("framerate.cap")];
        expect(!strcmp(settings_env_text(d, -1, b, sizeof b), "auto") && !strcmp(settings_token(d, -1, b, sizeof b), "auto"), "auto goes out as auto");
        setenv("TOTK_UNCAPPED_HZ", "auto", 1); a[settings_index("framerate.cap")] = 60; settings_take_env(a);
        expect(value(a, "framerate.cap") == -1, "TOTK_UNCAPPED_HZ=auto reads as auto"); unsetenv("TOTK_UNCAPPED_HZ");
    }

    int n = settings_parse_text("# comment\n framerate.cap = 60 \naudio.volume=47\ninterface.readout = off # trailing\n"
                                "unknown.key = 3\ngraphics.interpolation = bogus\ninterface.touch_opacity = 500\n", v);
    expect(n == 4, "four known keys with good values");
    expect(value(v, "framerate.cap") == 60, "a choice by its word");
    expect(value(v, "audio.volume") == 45, "a range is rounded down to its step");
    expect(value(v, "interface.readout") == 0, "off, with a trailing comment");
    expect(value(v, "graphics.interpolation") == 2, "a bad word keeps the default (On on the Mac)");
    expect(value(v, "interface.touch_opacity") == 100, "a range is clamped");

    settings_defaults(v, 0);
    expect(value(v, "controls.camera_speed_x") == 100 && value(v, "controls.camera_speed_y") == 100 && value(v, "graphics.bloom") == 100 &&
           value(v, "graphics.dof") == 1 && value(v, "interface.hide_hud") == 0 &&
           value(v, "interface.readout_detail") == 0, "the effects and the camera start as the game's own");
    expect(settings_parse_text("controls.camera_speed_x = 999\ncontrols.camera_speed_y = 12\ngraphics.bloom = 55\ninterface.readout_detail = detailed\n", v) == 4,
           "camera speed, bloom and readout detail read");
    expect(value(v, "controls.camera_speed_x") == 300 && value(v, "controls.camera_speed_y") == 25 && value(v, "graphics.bloom") == 50 &&
           value(v, "interface.readout_detail") == 1, "camera speed clamped to 25..300, bloom in steps of 10");
    {
        const SettingDef *d = &SETTINGS[settings_index("interface.readout_detail")];
        expect(d->shown_key && !strcmp(d->shown_key, "interface.readout") && d->shown_value == 1, "readout detail shows only with the readout on");
    }
    settings_parse_text("framerate.cap = 60\naudio.volume=47\ninterface.readout = off\ninterface.touch_opacity = 500\n", v);

    char text[4096]; int len = settings_format_text(v, text, sizeof text);
    expect(len > 0 && len < (int)sizeof text, "the file fits");
    settings_defaults(again, 1);   /* other defaults underneath: everything must come from the text */
    expect(settings_parse_text(text, again) == SETTINGS_COUNT, "the written file sets every setting");
    expect(!memcmp(v, again, sizeof v), "write then read gives the same values");
    expect(settings_format_text(v, text, 8) > 8, "a short buffer reports the full length");

    setenv("TOTK_UNCAPPED_HZ", "display", 1); setenv("TOTK_FACE_BUTTONS", "label", 1); setenv("TOTK_MFX", "interp", 1);
    settings_take_env(v);
    expect(value(v, "framerate.cap") == 0 && value(v, "controls.face_buttons") == 2 && value(v, "graphics.interpolation") == 2,
           "the environment wins, in the game's own words");
    unsetenv("TOTK_UNCAPPED_HZ"); unsetenv("TOTK_FACE_BUTTONS"); unsetenv("TOTK_MFX"); unsetenv("TOTK_HINTS");
    setenv("TOTK_SHADER_OVERLAY", "1", 1);
    v[settings_index("interface.readout")] = 0; v[settings_index("framerate.cap")] = 40; v[settings_index("interface.shader_counter")] = 0;
    settings_export_env(v);
    expect(getenv("TOTK_HINTS") && !strcmp(getenv("TOTK_HINTS"), "0"), "a bool goes out as 0 / 1");
    expect(getenv("TOTK_UNCAPPED_HZ") && !strcmp(getenv("TOTK_UNCAPPED_HZ"), "40"), "a choice goes out in the game's words");
    expect(!strcmp(getenv("TOTK_SHADER_OVERLAY"), "1"), "a variable already set is kept");
    v[settings_index("controls.face_buttons")] = 0; unsetenv("TOTK_FACE_BUTTONS"); settings_export_env(v);
    expect(!getenv("TOTK_FACE_BUTTONS"), "automatic leaves the variable unset");

    /* Fog (live, every platform) and shadow resolution (live, Mac; TOTK_SHADOW_RES for the start): defaults, the file's words, the game's variables. */
    settings_defaults(v, 0);
    expect(value(v, "graphics.fog") == 100 && value(v, "graphics.shadow_res") == 0, "fog and shadows default to the game's own");
    settings_defaults(v, 1);
    expect(value(v, "graphics.fog") == 100 && value(v, "graphics.shadow_res") == 0, "iOS defaults for fog and shadows");
    expect(settings_parse_text("graphics.fog = 55\ngraphics.shadow_res = 4\n", v) == 2 && value(v, "graphics.fog") == 50 &&
           value(v, "graphics.shadow_res") == 4, "fog rounds to its step, shadows by their word");
    expect(settings_parse_text("graphics.shadow_res = 3\n", v) == 0 && value(v, "graphics.shadow_res") == 4, "no 3x shadows");
    expect(settings_parse_text("graphics.shadow_res = 0.25\n", v) == 1 && value(v, "graphics.shadow_res") == -4, "0.25x shadows by their word");
    unsetenv("TOTK_SHADOW_RES"); settings_export_env(v);
    expect(getenv("TOTK_SHADOW_RES") && !strcmp(getenv("TOTK_SHADOW_RES"), "0.25"), "0.25x goes out as TOTK_SHADOW_RES=0.25");
    setenv("TOTK_SHADOW_RES", "0.5", 1); settings_take_env(v);
    expect(value(v, "graphics.shadow_res") == -2, "TOTK_SHADOW_RES=0.5 shows as 0.5x");
    expect(SETTINGS[settings_index("graphics.shadow_res")].nchoices == 5, "shadows: 0.25x, 0.5x, the game's own, 2x, 4x");
    unsetenv("TOTK_SHADOW_RES"); settings_parse_text("graphics.fog = 50\ngraphics.shadow_res = 4\n", v);
    expect(SETTINGS[settings_index("graphics.fog")].apply == SET_LIVE && SETTINGS[settings_index("graphics.shadow_res")].apply == SET_LIVE &&
           SETTINGS[settings_index("graphics.shadow_res")].platforms == SET_MAC, "fog is live everywhere, shadows live on the Mac");
    unsetenv("TOTK_FOG"); unsetenv("TOTK_SHADOW_RES"); settings_export_env(v);
    expect(getenv("TOTK_FOG") && !strcmp(getenv("TOTK_FOG"), "50") && getenv("TOTK_SHADOW_RES") && !strcmp(getenv("TOTK_SHADOW_RES"), "4"),
           "fog and shadows go out as TOTK_FOG / TOTK_SHADOW_RES");
    unsetenv("TOTK_SHADOW_RES"); v[settings_index("graphics.shadow_res")] = 0; settings_export_env(v);
    expect(!getenv("TOTK_SHADOW_RES"), "the game's own shadows leave the variable unset");
    setenv("TOTK_SHADOW_RES", "2", 1); settings_take_env(v);
    expect(value(v, "graphics.shadow_res") == 2, "TOTK_SHADOW_RES=2 shows as 2x");
    unsetenv("TOTK_FOG"); unsetenv("TOTK_SHADOW_RES");
    /* World page (runtime/world_patch.h): the time of day in percent, paused included; the render distance in 25 % steps. */
    /* The menu: Game, Graphics, Content, Tools; every settings page is exactly one section of them, in the same order. */
    expect(SETTINGS_NTABS == 5 && !strcmp(SETTINGS_TABS[0], "Game") && !strcmp(SETTINGS_TABS[3], "Tools") && !strcmp(SETTINGS_TABS[4], "Places"), "five top tabs");
    {
        int seen = 0, last_tab = 0;
        for (int k = 0; k < SETTINGS_NSECTIONS; k++) {
            const SettingsSection *sec = &SETTINGS_SECTIONS[k];
            expect(sec->tab >= last_tab && sec->tab < SETTINGS_NTABS, "sections are grouped by tab, in tab order");
            last_tab = sec->tab;
            if (sec->kind != MENU_SETTINGS) {
                for (int p = 0; p < SETTINGS_NPAGES; p++) expect(strcmp(SETTINGS_PAGES[p], sec->name) != 0, "Mods, Saves, Cheats and Spawn hold no settings (Reset leaves them alone)");
                continue;
            }
            expect(seen < SETTINGS_NPAGES && !strcmp(SETTINGS_PAGES[seen], sec->name), "settings pages follow the menu's order");
            seen++;
        }
        expect(seen == SETTINGS_NPAGES, "every settings page is a section");
        for (int t = 0; t < SETTINGS_NTABS; t++) expect(settings_tab_sections(t) >= 3 && settings_tab_sections(t) <= 6, "three to six sections a tab (short names: the bar fits six)");
        expect(SETTINGS_SECTIONS[settings_tab_first(3) + 1].kind == MENU_SPAWN, "Tools: Cheats, then Spawn");
        int tabs_of[SETTINGS_COUNT];
        for (int i = 0; i < SETTINGS_COUNT; i++) { tabs_of[i] = 0; for (int t = 0; t < SETTINGS_NTABS; t++) tabs_of[i] += settings_in_tab(i, t); }
        for (int i = 0; i < SETTINGS_COUNT; i++) expect(tabs_of[i] == 1, "every setting is on exactly one tab (Reset this tab)");
        expect(!strcmp(SETTINGS[settings_index("graphics.interpolation")].page, "Frame rate"), "frame interpolation sits with the frame rate");
        expect(!strcmp(SETTINGS[settings_index("interface.hide_hud")].page, "Camera") && settings_in_tab(settings_index("interface.hide_hud"), 3),
               "Hide the HUD: Tools > Camera");
        expect(!strcmp(SETTINGS[settings_index("world.time_speed")].page, "World") && settings_in_tab(settings_index("world.time_speed"), 3),
               "time of day: Tools > World");
        expect(settings_in_tab(settings_index("interface.download_cheats"), 2) && settings_in_tab(settings_index("graphics.bloom"), 1) &&
               settings_in_tab(settings_index("audio.volume"), 0), "Downloads on Content, effects on Graphics, audio on Game");
    }
    for (int i = 0; i < SETTINGS_COUNT; i++) expect(strncmp(SETTINGS[i].key, "cheats.", 7) != 0, "cheats are not settings: settings.txt and Reset all never hold them");
    expect(SETTINGS_COUNT == 42, "42 settings");
    {   /* Variable refresh: Auto on the Mac (TOTK_VRR=auto), Off reads as TOTK_VRR=0, a Frame rate row */
        const SettingDef *vr = &SETTINGS[settings_index("framerate.vrr")]; int a[SETTINGS_COUNT]; settings_defaults(a, 0);
        expect(settings_index("framerate.vrr") >= 0 && !strcmp(vr->page, "Frame rate") && !strcmp(vr->env, "TOTK_VRR") && vr->apply == SET_LIVE &&
               vr->platforms == SET_MAC && value(a, "framerate.vrr") == 1, "variable refresh: a live Mac setting, Auto by default");
        expect(settings_parse_text("framerate.vrr = off\n", a) == 1 && value(a, "framerate.vrr") == 0 && !strcmp(settings_choice(vr, 0)->env, "0") &&
               !strcmp(settings_choice(vr, 1)->env, "auto"), "variable refresh: off is TOTK_VRR=0, auto is TOTK_VRR=auto");
    }
    { int a[SETTINGS_COUNT]; settings_defaults(a, 0);
      expect(value(a, "graphics.mfx_debug_view") == 0 && settings_parse_text("graphics.mfx_debug_view = split\n", a) == 1 &&
             value(a, "graphics.mfx_debug_view") == 3, "the motion / depth debug view is off by default and reads by its word"); }
    {
        /* Dynamic resolution: the game's own by default, TOTK_FIXED_RES=1 for Off at the next start, Mac only. */
        const SettingDef *dr = &SETTINGS[settings_index("graphics.dynamic_res")];
        expect(dr->type == SET_CHOICE && dr->def_mac == 0 && dr->def_ios == 0 && dr->apply == SET_RESTART && dr->platforms == SET_MAC &&
               dr->env && !strcmp(dr->env, "TOTK_FIXED_RES") && dr->unavailable && !strcmp(dr->page, "World detail"),
               "dynamic resolution: on by default, after a restart, Mac only");
        settings_defaults(v, 0);
        unsetenv("TOTK_FIXED_RES"); settings_export_env(v);
        expect(!getenv("TOTK_FIXED_RES"), "the game's dynamic resolution leaves the variable unset");
        expect(settings_parse_text("graphics.dynamic_res = off\n", v) == 1 && value(v, "graphics.dynamic_res") == 1, "dynamic resolution off parses");
        settings_export_env(v);
        expect(getenv("TOTK_FIXED_RES") && !strcmp(getenv("TOTK_FIXED_RES"), "1"), "off goes out as TOTK_FIXED_RES=1");
        settings_defaults(v, 0); settings_take_env(v);
        expect(value(v, "graphics.dynamic_res") == 1, "TOTK_FIXED_RES=1 shows as off");
        unsetenv("TOTK_FIXED_RES");
    }
    {
        const SettingDef *ds = &SETTINGS[settings_index("interface.download_shaders")];
        expect(ds->type == SET_BOOL && ds->def_mac == 1 && ds->def_ios == 0 && ds->apply == SET_LIVE && ds->platforms == SET_MAC &&
               !ds->env && !strcmp(ds->page, "Downloads") && ds->unavailable,
               "download new shaders: on by default on the Mac, live, no start variable, greyed on iPhone and iPad");
        settings_defaults(v, 0);
        expect(settings_parse_text("interface.download_shaders = off\n", v) == 1 && value(v, "interface.download_shaders") == 0, "download new shaders off parses");
    }
    {
        const SettingDef *dc = &SETTINGS[settings_index("interface.download_cheats")];
        expect(dc->type == SET_BOOL && dc->def_mac == 1 && dc->def_ios == 0 && dc->apply == SET_LIVE && dc->platforms == SET_MAC &&
               !dc->env && !strcmp(dc->page, "Downloads") && dc->unavailable,
               "download cheat list: on by default on the Mac, live, no start variable, greyed on iPhone and iPad");
        settings_defaults(v, 0);
        expect(settings_parse_text("interface.download_cheats = off\n", v) == 1 && value(v, "interface.download_cheats") == 0, "download cheat list off parses");
    }
    {
        const SettingDef *lf = &SETTINGS[settings_index("graphics.lens_flare")];
        expect(lf->type == SET_BOOL && lf->def_mac == 1 && lf->def_ios == 1 && lf->apply == SET_LIVE && lf->platforms == SET_ALL && !lf->env,
               "lens flare: on (the game's) by default, live everywhere, no start variable");
        settings_defaults(v, 0);
        expect(settings_parse_text("graphics.lens_flare = off\n", v) == 1 && value(v, "graphics.lens_flare") == 0, "lens flare off parses");
    }
    const SettingDef *ts = &SETTINGS[settings_index("world.time_speed")], *rd = &SETTINGS[settings_index("graphics.render_distance")];
    expect(!strcmp(ts->page, "World") && ts->nchoices == 7 && ts->def_mac == 100 && ts->def_ios == 100 && ts->apply == SET_LIVE && ts->platforms == SET_ALL,
           "time of day speed: seven choices, normal by default, live everywhere");
    expect(rd->min == 10 && rd->max == 300 && rd->step == 10 && rd->def_mac == 100 && rd->apply == SET_LIVE && rd->platforms == SET_ALL,
           "render distance: 10..300 % in 10 % steps, live everywhere");
    settings_defaults(v, 0);
    expect(settings_parse_text("world.time_speed = paused\ngraphics.render_distance = 265\n", v) == 2, "the world settings parse");
    expect(value(v, "world.time_speed") == 0 && value(v, "graphics.render_distance") == 260, "paused is 0 %, a distance rounds to its step");
    setenv("TOTK_TIME_SPEED", "800", 1); settings_take_env(v); unsetenv("TOTK_TIME_SPEED");
    expect(value(v, "world.time_speed") == 800, "TOTK_TIME_SPEED in percent");

    /* Graphics presets: each preset's members are valid values; the defaults match none (Custom) on the Mac; a preset's
       values match it, one changed member shows Custom, and iOS ignores members it does not have (shadow resolution). */
    for (int p = SET_PRESET_BATTERY; p <= SET_PRESET_QUALITY; p++)
        for (int m = 0; m < SETTINGS_PRESET_NMEMBERS; m++) {
            int i = settings_index(SETTINGS_PRESET_MEMBERS[m].key), x = settings_preset_value(p, m, 0);
            expect(i >= 0 && settings_valid(&SETTINGS[i], x), "a preset member is a setting and its value valid");
        }
    for (int ios = 0; ios < 2; ios++) {
        settings_defaults(v, ios);
        expect(settings_preset_match(v, ios) == SET_PRESET_BALANCED, "the defaults are Balanced");
        for (int p = SET_PRESET_BATTERY; p <= SET_PRESET_QUALITY; p++) {
            settings_defaults(v, ios);
            for (int m = 0; m < SETTINGS_PRESET_NMEMBERS; m++) {
                int x = settings_preset_value(p, m, ios);
                if (x != INT_MIN) v[settings_index(SETTINGS_PRESET_MEMBERS[m].key)] = x;
            }
            expect(settings_preset_match(v, ios) == p, "a preset's values match it");
            v[settings_index("graphics.aniso")] = 8;
            expect(settings_preset_match(v, ios) == SET_PRESET_CUSTOM, "one member changed by hand: Custom");
        }
    }
    expect(settings_preset_value(SET_PRESET_QUALITY, 4, 1) == INT_MIN, "shadow resolution is not a preset member on iOS");

    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    printf("settings: all checks passed (%d settings)\n", SETTINGS_COUNT);
    return 0;
}
