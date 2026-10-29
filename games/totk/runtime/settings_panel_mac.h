/* The settings menu on the Mac (included by runtime/frontend.m only): a sidebar drawn inside the game window, so it works
   in full screen and leaves the picture beside it in view while a change takes effect. Built from the shared settings
   table (runtime/settings_schema.h): four tabs (Game, Graphics, Content, Tools), each a bar of sections. Opened with
   Command-comma, F1, the app menu's Settings item, or a controller's Home button (or Options and Menu held together for a moment).
   Two levels: the section bar (← → or the D-pad choose a section, ↓ / Return / A open it) and the section's rows (↑ ↓
   choose, ← → change, Page Up / Down or LT / RT page, never wrapping round); Escape or B goes back one level and closes
   the menu only from the section bar. Q / E, Tab or the shoulder buttons switch tabs. Reset this tab (R, X on the bar)
   and Reset everything… (Shift-R, Y on the bar, then confirm) set settings back; cheats, mods, saves and the spawner's
   favorites are never reset. While it is open the game reads no buttons or keys, only the controller sticks (runtime_input_hold). Content › Mods is
   runtime/mods_panel_mac.h (the installed mods and the GameBanana browser; the sidebar widens while it shows), Tools ›
   Spawn is runtime/spawn_panel_mac.h, and Tools › Cheats lists the community
   cheat list the game downloads (cheat_db_fetch.h; shown by group through cheat_catalog.h, variants as one choice, the ones
   this port cannot run folded away) and then the cheat files of the player's own (runtime/cheats.c, cheat_files.h), with
   the folder, Turn off all and Restart now; it is built here like the settings pages. Content › Saves is Send saves:
   the profile's saves as one .totksave file (runtime/save_package.h) for AirDrop, Messages, Mail or Finder; the other
   device opens it to bring them in (docs/save-sync.md). The iPhone / iPad menu is ios/Sources/settings_menu.m. */
#pragma once
#import <Cocoa/Cocoa.h>
#import <GameController/GameController.h>
#include <sys/sysctl.h>
#include "runtime.h"
#include "settings.h"
#import "save_package.h"
#include "cheat_catalog.h"
#include "cheat_db_fetch.h"

@interface TOTKFlippedView : NSView @end   /* the scrolling rows start at the top */
@implementation TOTKFlippedView
- (BOOL)isFlipped { return YES; }
@end
#include "mods_panel_mac.h"
#include "spawn_panel_mac.h"
#include "world_survey.h"
#include "map_pins.h"
#include "landmarks.h"
/* runtime/romfs.c: the game's files, read on the side (as the shader list does) */
size_t runtime_romfs_file_count(void);
const char *runtime_romfs_file_name(size_t i);
uint8_t *runtime_romfs_host_read(const char *path, size_t *n);
uint8_t *runtime_romfs_zs_decompress(const uint8_t *src, size_t n, size_t *out);

@interface TOTKSettingsPanel : NSView
+ (void)toggle;
+ (BOOL)snapshotTo:(NSString *)path;   /* the menu as a PNG (test runs, TOTK_INPUT_FILE "settings-shot") */
+ (void)key:(NSEvent *)event;          /* a key for the open menu (test runs, TOTK_INPUT_FILE "menukey") */
+ (void)showCheats;                    /* opens the menu on Tools › Cheats (the app menu's Cheats… item) */
@end

static NSWindow *settings_window(void);   /* frontend.m: the game window */

@interface TOTKSettingsPanel () <NSSharingServicePickerDelegate, NSSearchFieldDelegate> @end
@implementation TOTKSettingsPanel {
    NSSearchField *find; NSString *findQuery;   /* search every setting (/ or ⌘F); a query lists the matches instead of the section */
    NSSegmentedControl *tabs, *sectionBar;   /* the four tabs; the sections of the tab shown */
    NSScrollView *scroll;
    NSStackView *list;
    NSTextField *status, *hint;
    NSButton *resetTab, *resetAll, *resetCancel, *restartNow;   /* restartNow: shown while a setting or the mods wait for a restart */
    TOTKModsPane *mods;                   /* Content › Mods */
    TOTKSpawnPane *spawn;                 /* Tools › Spawn (spawn_panel_mac.h) */
    NSMutableArray<NSView *> *rows;       /* the rows of the page shown, in order */
    NSMutableArray<NSNumber *> *rowSetting;   /* the setting of each row; SAVES_ROW: Send saves (Interface tab); CHEAT_ROW...: the Cheats tab */
    NSButton *sendButton; NSTextField *savesNote;
    NSInteger section, focus;             /* SETTINGS_SECTIONS index; the focused row */
    NSInteger lastSection[SETTINGS_NTABS];   /* each tab opens on the section it showed last */
    BOOL onBar, confirmReset;             /* the focus is on the section bar (the top level); Reset everything… asks */
    NSTimer *pollTimer, *meterTimer;   /* meterTimer: the GPU memory line, once a second */
    uint64_t padPrev; double padRepeatAt; int padDir;
    BOOL cheatsUnavailableShown, cheatDbAsked;
    Landmark marks[LANDMARKS];   /* the places with the last save's state and the story lock (landmarks.h), when the tab is built */
    MapPin pins[MAP_PINS_MAX]; int npins;   /* the Teleport tab's map pin rows (map_pins.h), read when the tab is built */
    NSString *autoShrineShown;   /* the auto shrine status the Teleport tab shows (rebuilt when it changes) */
}
static TOTKSettingsPanel *settingsPanel;
/* The towers', shrines' and story places' names and places (landmarks.h), read from the RomFS once a run, off the main thread. */
static Landmark landmark_places[LANDMARKS];
static int landmarks_read;   /* 0 not yet, 1 reading, 2 read, 3 none in this game; main thread */
enum { PANEL_WIDTH = 480, PANEL_MARGIN = 16, PAGE_STEP = 8 /* rows per LT / RT on a pane without row frames */,
       SAVES_ROW = -1, CHEATS_FOLDER_ROW = -2, CHEATS_OFF_ROW = -3, CHEATS_RESTART_ROW = -4, CHEATS_DB_ROW = -5, CHEATS_UNAVAILABLE_ROW = -6,
       CHEATS_NOCLIP_ROW = -7, CHEATS_NOCLIP_SPEED_ROW = -8, CHEATS_AUTOSHRINE_ROW = -9, CHEATS_AUTOSHRINE_CHESTS_ROW = -10, CHEATS_GROUP_ROW = -20 /* the places while the game's map is read */,
       CHEATS_PIN_ROW = -100 /* - k: map pin k */, CHEATS_LANDMARK_ROW = -200 /* - k: place k (landmarks.h) */,
       CHEAT_ROW = -1000 /* - k: cheat k, or the choice whose first variant is k */ };
static NSString *const BAR_HINT = @"←→ sections   ↓ open   Q E tabs   R reset tab   ⇧R reset all   Esc close\n"
                                   "Controller: D-pad, A open, LB RB tabs, X reset tab, Y reset all, B close";
static NSString *const SETTINGS_HINT = @"↑↓ choose   ←→ change   PgUp PgDn page   Q E tabs   Esc back\n"
                                       "Controller: D-pad, A, LT RT page, LB RB tabs, B back";
static NSString *const RESET_CONFIRM_HINT = @"Reset every setting to its default? Cheats, mods, saves and spawn favorites stay.\n"
                                            "Return or A: reset everything   Esc or B: cancel";

static BOOL settings_row_shown(const SettingDef *d) {
    if (!(d->platforms & SET_MAC)) return NO;
    return !d->shown_key || settings_get(d->shown_key) == d->shown_value;
}
static NSString *settings_value_text(const SettingDef *d, int v) {
    if (d->type == SET_CHOICE) { const SettingChoice *c = settings_choice(d, v); return c ? @(c->label) : @"?"; }
    if (d->type == SET_RANGE) return [NSString stringWithFormat:@"%d%s", v, d->unit ? d->unit : ""];
    return v ? @"On" : @"Off";
}
/* ---- GPU memory: choices that would not fit, and a chosen mode that is not running for lack of it ---- */
void mtl_gpu_memory(uint64_t *used, uint64_t *budget);   /* nvn_mtl.m: allocated now, and the budget the game stops at */
int mfx_obj_motion_on(void);   /* totk_mfx.h: TOTK_OBJ_MOTION=1, paused with interpolation */
static double settings_gb(uint64_t b) { return b / 1073741824.0; }
static BOOL settings_mem_row(const SettingDef *d) { return !strcmp(d->key, "graphics.interpolation") || !strcmp(d->key, "display.render_max") || !strcmp(d->key, "framerate.vrr"); }
/* display.render_max: the GPU memory in use at 1800p / 4K, from the one measurement there is (docs/settings.md: an 18 GB
   M3 Pro at a 4K display resolution, 2026-10-21: 1800p up to 6.1 GB, 4K about 1.5 GB more). An estimate, not a promise. */
static uint64_t settings_render_max_need(int v) { return v >= 2160 ? 7600ull << 20 : v >= 1800 ? 6100ull << 20 : 0; }
/* 1: choice v of d would clearly not fit. Frame interpolation: the use now plus its estimate (mfx_need_bytes at the display
   resolution) above the pause threshold (MFX_BP_HIGH percent); while it runs its memory is already in use, so no mode is
   marked. Maximum render resolution: the measured use above the budget. */
static BOOL settings_mem_short(const SettingDef *d, int v, uint64_t used, uint64_t budget) {
    if (!budget || !v) return NO;
    if (!strcmp(d->key, "graphics.interpolation")) {
        if (mfx_interp_state() == MFX_STATE_ON) return NO;
        unsigned w, h; settings_output_size(&w, &h);
        return used + mfx_need_bytes(w, h) > budget / 100 * MFX_BP_HIGH;
    }
    if (!strcmp(d->key, "display.render_max")) return settings_render_max_need(v) > budget;
    return NO;
}
static NSString *settings_choice_title(const SettingDef *d, int k, uint64_t used, uint64_t budget) {
    NSString *t = @(d->choices[k].label); unsigned w, h;
    if (!strcmp(d->key, "display.scale")) {   /* each render resolution with the size it gives at this display resolution */
        int capped = settings_render_size(d->choices[k].value, &w, &h); t = [NSString stringWithFormat:@"%@ · %u×%u%@", t, w, h, capped ? @" (max)" : @""];
    }
    if (settings_mem_short(d, d->choices[k].value, used, budget)) t = [t stringByAppendingString:@" (not enough GPU memory)"];
    return t;
}
/* The line under a memory row: the chosen value and why it is not running (or may not fit); nil when nothing to say. */
static NSString *settings_mem_note(const SettingDef *d, uint64_t used, uint64_t budget, NSColor **color) {
    int v = settings_get(d->key); NSString *chosen = settings_value_text(d, v);
    *color = NSColor.systemRedColor;
    if (!strcmp(d->key, "graphics.interpolation")) {
        NSString *also = mfx_obj_motion_on() ? @" Per-object motion is paused with it." : @"";
        switch (mfx_interp_state()) {
        case MFX_STATE_PAUSED_MEMORY:
            *color = NSColor.systemOrangeColor;
            return [NSString stringWithFormat:@"%@ — paused: not enough GPU memory (%.1f of %.1f GB). It resumes by itself below %d%%.%@",
                    chosen, settings_gb(used), settings_gb(budget), MFX_BP_LOW, also];
        case MFX_STATE_REFUSED_MEMORY:
            return [NSString stringWithFormat:@"%@ — not applied: not enough GPU memory (%.1f of %.1f GB in use, about %.1f GB more needed). It turns on by itself if memory frees up.%@",
                    chosen, settings_gb(used), settings_gb(budget), settings_gb(mfx_need_bytes(0, 0)), also];
        case MFX_STATE_OFF_MEMORY:
            return [NSString stringWithFormat:@"%@ — off for this session: not enough GPU memory (paused %d times). Choose Off and then a mode to try again.%@",
                    chosen, MFX_BP_MAX, also];
        }
        return nil;
    }
    if (!strcmp(d->key, "framerate.vrr")) {   /* variable refresh: active now, or why not (nvn_mtl.m vrr_eval), refreshed once a second */
        if (!v) return nil;
        *color = NSColor.secondaryLabelColor;
        switch (mtl_vrr_state()) {
        case MTL_VRR_ACTIVE: {
            *color = NSColor.systemGreenColor; double f = mtl_vrr_target_fps();
            if (f > 0 && mfx_interp_state() == MFX_STATE_ON) return [NSString stringWithFormat:@"Active: game %.0f fps, shown at %.0f fps with frame interpolation", f, 2 * f];
            return f > 0 ? [NSString stringWithFormat:@"Active: paced at %.0f fps, the display follows each frame", f] : @"Active: the display follows each frame";
        }
        case MTL_VRR_WINDOWED: *color = NSColor.systemOrangeColor; return @"Not active: windowed (variable refresh needs full screen)";
        case MTL_VRR_FIXED: return @"Not active: this display has a fixed refresh rate";
        case MTL_VRR_INTERP: *color = NSColor.systemOrangeColor; return @"Not active: this interpolation mode needs a fixed refresh (choose On)";
        default: return @"Not active yet: waiting for the game's first frame";
        }
    }
    if (!strcmp(d->key, "display.render_max") && settings_mem_short(d, v, used, budget)) {
        *color = NSColor.systemOrangeColor;
        return [NSString stringWithFormat:@"%@ — may not fit: it used about %.1f GB where it was measured, the limit here is %.1f GB, and the game stops past it.",
                chosen, settings_gb(settings_render_max_need(v)), settings_gb(budget)];
    }
    return nil;
}
static int settings_page_tab(const char *page) {
    for (int k = 0; k < SETTINGS_NSECTIONS; k++) if (!strcmp(SETTINGS_SECTIONS[k].name, page)) return SETTINGS_SECTIONS[k].tab;
    return 0;
}
static BOOL settings_matches(const SettingDef *d, NSString *q) {   /* every word of the query in the label, help or section */
    NSString *hay = [NSString stringWithFormat:@"%s %s %s", d->label, d->help ? d->help : "", d->page];
    for (NSString *w in [q componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceCharacterSet])
        if (w.length && [hay rangeOfString:w options:NSCaseInsensitiveSearch | NSDiacriticInsensitiveSearch].location == NSNotFound) return NO;
    return YES;
}
static NSColor *panel_color(double w, double a) { return [NSColor colorWithCalibratedWhite:w alpha:a]; }

+ (TOTKSettingsPanel *)shared {
    if (!settingsPanel) settingsPanel = [[TOTKSettingsPanel alloc] initWithFrame:NSMakeRect(0, 0, PANEL_WIDTH, 700)];
    return settingsPanel;
}

+ (void)key:(NSEvent *)event { if (settingsPanel.superview) [settingsPanel keyDown:event]; }

+ (void)toggle {
    TOTKSettingsPanel *p = [self shared];
    if (p.superview) [p hide]; else [p show];
}

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    self.wantsLayer = YES;
    self.layer.backgroundColor = [NSColor colorWithCalibratedRed:0.07 green:0.08 blue:0.09 alpha:0.94].CGColor;
    self.layer.cornerRadius = 14; self.layer.borderWidth = 1; self.layer.borderColor = panel_color(1, 0.08).CGColor;
    rows = [NSMutableArray array]; rowSetting = [NSMutableArray array];

    NSTextField *title = [NSTextField labelWithString:@"Settings"];
    title.font = [NSFont systemFontOfSize:22 weight:NSFontWeightBold]; title.textColor = NSColor.whiteColor;
    status = [NSTextField labelWithString:@""]; status.maximumNumberOfLines = 2; status.usesSingleLineMode = NO;
    status.font = [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular]; status.textColor = NSColor.secondaryLabelColor;
    find = [NSSearchField new]; find.placeholderString = @"Search settings (/)"; find.delegate = self; find.controlSize = NSControlSizeSmall;
    find.sendsSearchStringImmediately = YES; find.target = self; find.action = @selector(searched:); findQuery = @"";

    NSMutableArray *labels = [NSMutableArray array];
    for (int t = 0; t < SETTINGS_NTABS; t++) [labels addObject:@(SETTINGS_TABS[t])];
    tabs = [NSSegmentedControl segmentedControlWithLabels:labels trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(tabClicked:)];
    tabs.segmentStyle = NSSegmentStyleAutomatic; tabs.selectedSegment = 0; tabs.refusesFirstResponder = YES;
    tabs.segmentDistribution = NSSegmentDistributionFillEqually;
    sectionBar = [NSSegmentedControl segmentedControlWithLabels:@[@""] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(sectionClicked:)];
    sectionBar.segmentStyle = NSSegmentStyleRoundRect; sectionBar.refusesFirstResponder = YES; sectionBar.controlSize = NSControlSizeSmall;
    sectionBar.font = [NSFont systemFontOfSize:11]; sectionBar.wantsLayer = YES; sectionBar.layer.cornerRadius = 6;
    for (int t = 0; t < SETTINGS_NTABS; t++) lastSection[t] = settings_tab_first(t);
    onBar = YES;

    list = [NSStackView new]; list.orientation = NSUserInterfaceLayoutOrientationVertical;
    list.alignment = NSLayoutAttributeLeading; list.spacing = 4; list.translatesAutoresizingMaskIntoConstraints = NO;
    list.edgeInsets = NSEdgeInsetsMake(4, 0, 12, 0);
    scroll = [NSScrollView new]; scroll.hasVerticalScroller = YES; scroll.drawsBackground = NO; scroll.autohidesScrollers = YES;
    NSView *doc = [TOTKFlippedView new]; doc.translatesAutoresizingMaskIntoConstraints = NO;
    [doc addSubview:list]; scroll.documentView = doc;

    hint = [NSTextField wrappingLabelWithString:SETTINGS_HINT];
    hint.font = [NSFont systemFontOfSize:10.5]; hint.textColor = NSColor.tertiaryLabelColor;
    resetTab = [NSButton buttonWithTitle:@"Reset this tab" target:self action:@selector(resetThisTab)];
    resetAll = [NSButton buttonWithTitle:@"Reset everything…" target:self action:@selector(resetEverything)];
    resetCancel = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancelReset)]; resetCancel.hidden = YES;
    restartNow = [NSButton buttonWithTitle:@"Restart now" target:self action:@selector(restartGame)];
    restartNow.controlSize = NSControlSizeSmall; restartNow.refusesFirstResponder = YES; restartNow.hidden = YES;
    restartNow.bezelColor = NSColor.systemOrangeColor; restartNow.translatesAutoresizingMaskIntoConstraints = NO;
    for (NSButton *b in @[resetTab, resetAll, resetCancel]) { b.controlSize = NSControlSizeSmall; b.refusesFirstResponder = YES; b.bezelStyle = NSBezelStyleRounded; }
    NSStackView *resets = [NSStackView stackViewWithViews:@[resetTab, resetAll, resetCancel]]; resets.spacing = 6;

    mods = [[TOTKModsPane alloc] initWithMenu:self]; mods.hidden = YES;
    spawn = [self makeSpawnPane]; spawn.hidden = YES;
    [NSNotificationCenter.defaultCenter addObserverForName:TOTKCheatDbChanged object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
        (void)n; if (self.superview && self.kind == MENU_CHEATS) [self rebuildKeepingPlace];
    }];
    for (NSView *v in @[title, status, find, tabs, sectionBar, scroll, mods, spawn, hint, resets, restartNow]) { v.translatesAutoresizingMaskIntoConstraints = NO; [self addSubview:v]; }
    [NSLayoutConstraint activateConstraints:@[
        [title.topAnchor constraintEqualToAnchor:self.topAnchor constant:18], [title.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:20],
        [find.centerYAnchor constraintEqualToAnchor:title.centerYAnchor], [find.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-16],
        [find.widthAnchor constraintEqualToConstant:170], [find.leadingAnchor constraintGreaterThanOrEqualToAnchor:title.trailingAnchor constant:12],
        [status.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:4], [status.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:20],
        [status.trailingAnchor constraintLessThanOrEqualToAnchor:self.trailingAnchor constant:-16],
        [tabs.topAnchor constraintEqualToAnchor:status.bottomAnchor constant:10],
        [tabs.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16], [tabs.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-16],
        [sectionBar.topAnchor constraintEqualToAnchor:tabs.bottomAnchor constant:8],
        [sectionBar.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
        [sectionBar.trailingAnchor constraintLessThanOrEqualToAnchor:self.trailingAnchor constant:-16],
        [scroll.topAnchor constraintEqualToAnchor:sectionBar.bottomAnchor constant:10],
        [scroll.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:8], [scroll.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-8],
        [scroll.bottomAnchor constraintEqualToAnchor:resets.topAnchor constant:-8],
        [mods.topAnchor constraintEqualToAnchor:scroll.topAnchor], [mods.bottomAnchor constraintEqualToAnchor:scroll.bottomAnchor],
        [mods.leadingAnchor constraintEqualToAnchor:scroll.leadingAnchor], [mods.trailingAnchor constraintEqualToAnchor:scroll.trailingAnchor],
        [spawn.topAnchor constraintEqualToAnchor:scroll.topAnchor], [spawn.bottomAnchor constraintEqualToAnchor:scroll.bottomAnchor],
        [spawn.leadingAnchor constraintEqualToAnchor:scroll.leadingAnchor constant:8], [spawn.trailingAnchor constraintEqualToAnchor:scroll.trailingAnchor constant:-8],
        [hint.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:20], [hint.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-14],
        [hint.trailingAnchor constraintLessThanOrEqualToAnchor:self.trailingAnchor constant:-16],
        [resets.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-16], [resets.bottomAnchor constraintEqualToAnchor:hint.topAnchor constant:-8],
        [restartNow.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16], [restartNow.centerYAnchor constraintEqualToAnchor:resets.centerYAnchor],
        [list.topAnchor constraintEqualToAnchor:doc.topAnchor], [list.leadingAnchor constraintEqualToAnchor:doc.leadingAnchor],
        [list.trailingAnchor constraintEqualToAnchor:doc.trailingAnchor], [list.bottomAnchor constraintEqualToAnchor:doc.bottomAnchor],
        [doc.widthAnchor constraintEqualToAnchor:scroll.contentView.widthAnchor]]];
    return self;
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isFlipped { return YES; }
- (void)mouseDown:(NSEvent *)e { (void)e; [self.window makeFirstResponder:self]; }   /* clicks on the panel stay with the menu */

/* ---- rows ---- */

- (NSView *)controlFor:(const SettingDef *)d index:(int)i {
    int v = settings_get(d->key); NSControl *c;
    if (d->type == SET_BOOL) { NSSwitch *s = [NSSwitch new]; s.state = v ? NSControlStateValueOn : NSControlStateValueOff; c = s; }
    else if (d->type == SET_CHOICE) {
        NSPopUpButton *p = [NSPopUpButton new]; uint64_t used = 0, budget = 0; mtl_gpu_memory(&used, &budget);
        for (int k = 0; k < d->nchoices; k++) {   /* a choice that would not fit stays selectable, marked (settings_mem_short) */
            [p addItemWithTitle:settings_choice_title(d, k, used, budget)]; if (d->choices[k].value == v) [p selectItemAtIndex:k];
        }
        [p.widthAnchor constraintEqualToConstant:190].active = YES; c = p;
    } else {
        NSSlider *s = [NSSlider sliderWithValue:v minValue:d->min maxValue:d->max target:nil action:nil];
        s.continuous = YES; [s.widthAnchor constraintEqualToConstant:118].active = YES; c = s;
    }
    c.tag = i; c.target = self; c.action = @selector(changed:); c.refusesFirstResponder = YES;
    if (d->type != SET_RANGE) return c;
    NSTextField *value = [NSTextField labelWithString:settings_value_text(d, v)];
    value.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular]; value.alignment = NSTextAlignmentRight;
    value.identifier = @"value"; [value.widthAnchor constraintEqualToConstant:52].active = YES;
    NSStackView *row = [NSStackView stackViewWithViews:@[c, value]]; row.spacing = 6;
    return row;
}

- (NSView *)rowFor:(const SettingDef *)d index:(int)i {
    NSView *row = [NSView new]; row.wantsLayer = YES; row.layer.cornerRadius = 8; row.translatesAutoresizingMaskIntoConstraints = NO;
    NSString *name = settings_pending_restart(d->key) ? [NSString stringWithFormat:@"%s (after restart)", d->label] : @(d->label);
    NSMutableAttributedString *title = [[NSMutableAttributedString alloc] initWithString:name attributes:@{
        NSFontAttributeName:[NSFont systemFontOfSize:13 weight:NSFontWeightMedium], NSForegroundColorAttributeName:NSColor.labelColor}];
    if (d->apply == SET_RESTART && !settings_pending_restart(d->key))   /* said before a change, not only after it */
        [title appendAttributedString:[[NSAttributedString alloc] initWithString:@"  ↻ restart" attributes:@{
            NSFontAttributeName:[NSFont systemFontOfSize:10 weight:NSFontWeightMedium], NSForegroundColorAttributeName:NSColor.systemOrangeColor}]];
    if (findQuery.length)   /* a search result: where it lives */
        [title appendAttributedString:[[NSAttributedString alloc] initWithString:[NSString stringWithFormat:@"  %s ▸ %s", SETTINGS_TABS[settings_page_tab(d->page)], d->page]
            attributes:@{NSFontAttributeName:[NSFont systemFontOfSize:10], NSForegroundColorAttributeName:NSColor.tertiaryLabelColor}]];
    NSTextField *label = [NSTextField labelWithAttributedString:title];
    NSStackView *text = [NSStackView stackViewWithViews:@[label]]; text.orientation = NSUserInterfaceLayoutOrientationVertical;
    text.alignment = NSLayoutAttributeLeading; text.spacing = 2;
    if (d->help) {   /* one line; the whole text on the focused row (showFocus) */
        NSString *about = @(d->help);
#ifdef TOTK_GAME_V143
        about = [about stringByReplacingOccurrencesOfString:@" (version 1.4.3)" withString:@""];   /* this is 1.4.3: say it nowhere */
#endif
        NSTextField *help = [NSTextField wrappingLabelWithString:about];
        help.font = [NSFont systemFontOfSize:11]; help.textColor = NSColor.secondaryLabelColor; help.preferredMaxLayoutWidth = 176;
        help.identifier = @"help"; help.maximumNumberOfLines = 1; help.cell.truncatesLastVisibleLine = YES;
        help.lineBreakMode = NSLineBreakByTruncatingTail;
        [text addArrangedSubview:help];
    }
    if (settings_mem_row(d)) {   /* not running / may not fit for lack of GPU memory (refreshMemoryRows) */
        NSTextField *note = [NSTextField wrappingLabelWithString:@""]; note.identifier = @"memnote"; note.hidden = YES;
        note.font = [NSFont systemFontOfSize:11 weight:NSFontWeightMedium]; note.preferredMaxLayoutWidth = 176;
        [text addArrangedSubview:note];
    }
    NSView *control = [self controlFor:d index:i];
    for (NSView *v in @[text, control]) { v.translatesAutoresizingMaskIntoConstraints = NO; [row addSubview:v]; }
    [NSLayoutConstraint activateConstraints:@[
        [row.widthAnchor constraintEqualToConstant:PANEL_WIDTH - 2 * 8 - 14],
        [text.leadingAnchor constraintEqualToAnchor:row.leadingAnchor constant:12], [text.topAnchor constraintEqualToAnchor:row.topAnchor constant:9],
        [text.bottomAnchor constraintLessThanOrEqualToAnchor:row.bottomAnchor constant:-9], [text.widthAnchor constraintLessThanOrEqualToConstant:180],
        [control.trailingAnchor constraintEqualToAnchor:row.trailingAnchor constant:-10], [control.centerYAnchor constraintEqualToAnchor:row.centerYAnchor],
        [row.heightAnchor constraintGreaterThanOrEqualToAnchor:control.heightAnchor constant:18],
        [control.leadingAnchor constraintGreaterThanOrEqualToAnchor:text.trailingAnchor constant:8]]];
    [row setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationVertical];
    return row;
}

/* Send saves, at the end of the Interface tab: what is on disk now, sent as one file (runtime/save_package.h). */
static NSString *saves_profile(void) { const char *d = getenv("TOTK_PROFILE_DIR"); return d && *d ? @(d) : @"user-data"; }
- (NSView *)savesRow {
    NSView *row = [NSView new]; row.wantsLayer = YES; row.layer.cornerRadius = 8; row.translatesAutoresizingMaskIntoConstraints = NO;
    NSTextField *label = [NSTextField labelWithString:@"Send saves"];
    label.font = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium]; label.textColor = NSColor.labelColor;
    NSTextField *help = [NSTextField wrappingLabelWithString:[NSString stringWithFormat:@"%@. To an iPhone, iPad or another Mac with "
        "AirDrop: save in the game first, the newest saves on disk are what is sent. Opening the file there brings them in "
        "(the saves it replaces are kept).", TOTKSaveSummary(saves_profile())]];
    help.font = [NSFont systemFontOfSize:11]; help.textColor = NSColor.secondaryLabelColor; help.preferredMaxLayoutWidth = 250;
    savesNote = [NSTextField wrappingLabelWithString:@""]; savesNote.hidden = YES;
    savesNote.font = [NSFont systemFontOfSize:11 weight:NSFontWeightMedium]; savesNote.textColor = NSColor.systemOrangeColor; savesNote.preferredMaxLayoutWidth = 250;
    NSStackView *text = [NSStackView stackViewWithViews:@[label, help, savesNote]]; text.orientation = NSUserInterfaceLayoutOrientationVertical;
    text.alignment = NSLayoutAttributeLeading; text.spacing = 2;
    sendButton = [NSButton buttonWithTitle:@"Send…" target:self action:@selector(sendSaves)];
    sendButton.image = [NSImage imageWithSystemSymbolName:@"square.and.arrow.up" accessibilityDescription:nil]; sendButton.imagePosition = NSImageLeading;
    sendButton.controlSize = NSControlSizeSmall; sendButton.bezelStyle = NSBezelStyleRounded; sendButton.refusesFirstResponder = YES;
    for (NSView *v in @[text, sendButton]) { v.translatesAutoresizingMaskIntoConstraints = NO; [row addSubview:v]; }
    [NSLayoutConstraint activateConstraints:@[
        [row.widthAnchor constraintEqualToConstant:PANEL_WIDTH - 2 * 8 - 14],
        [text.leadingAnchor constraintEqualToAnchor:row.leadingAnchor constant:12], [text.topAnchor constraintEqualToAnchor:row.topAnchor constant:9],
        [text.bottomAnchor constraintEqualToAnchor:row.bottomAnchor constant:-9], [text.widthAnchor constraintLessThanOrEqualToConstant:256],
        [sendButton.trailingAnchor constraintEqualToAnchor:row.trailingAnchor constant:-10], [sendButton.topAnchor constraintEqualToAnchor:row.topAnchor constant:9],
        [sendButton.leadingAnchor constraintGreaterThanOrEqualToAnchor:text.trailingAnchor constant:8]]];
    return row;
}
- (void)savesNote:(NSString *)text {
    savesNote.stringValue = text ?: @""; savesNote.hidden = !text.length;
    if (text.length) fprintf(stderr, "[saves] %s\n", text.UTF8String);
}
- (void)sendSaves {
    NSString *profile = saves_profile();
    if (runtime_fs_save_busy() || TOTKSaveBusy(profile)) { [self savesNote:@"The game is saving right now. Try again in a moment."]; return; }
    char model[64] = ""; size_t n = sizeof model - 1; sysctlbyname("hw.model", model, &n, NULL, 0);
    NSError *error = nil;
    NSString *path = TOTKSaveExport(profile, [NSTemporaryDirectory() stringByAppendingPathComponent:@"totk-saves-out"], @TOTK_GAME_VERSION,
                                    @"mac", NSHost.currentHost.localizedName, @(model), &error);
    if (!path) { [self savesNote:error.localizedDescription]; return; }
    [self savesNote:nil];
    fprintf(stderr, "[saves] packed %s\n", path.fileSystemRepresentation);
    NSSharingServicePicker *picker = [[NSSharingServicePicker alloc] initWithItems:@[[NSURL fileURLWithPath:path]]];
    picker.delegate = self;
    [picker showRelativeToRect:sendButton.bounds ofView:sendButton preferredEdge:NSRectEdgeMinY];
}
/* AirDrop first; Finder's Reveal stays available through the picker's own services. */
- (NSArray<NSSharingService *> *)sharingServicePicker:(NSSharingServicePicker *)picker sharingServicesForItems:(NSArray *)items
                              proposedSharingServices:(NSArray<NSSharingService *> *)proposed {
    (void)picker; (void)items;
    NSSharingService *airdrop = [NSSharingService sharingServiceNamed:NSSharingServiceNameSendViaAirDrop];
    NSMutableArray *out = [NSMutableArray array];
    if (airdrop) [out addObject:airdrop];
    for (NSSharingService *service in proposed) if (!airdrop || ![service.title isEqual:airdrop.title]) [out addObject:service];
    return out;
}

/* ---- the Cheats tab (runtime/cheats.c) ---- */

/* Code cheats switched since this launch that wait for the game to start again (a conflict, or no live code patches). */
static int cheats_pending(void) {
    int n = 0;
    for (int i = 0; i < runtime_cheats_count(); i++) {
        int st = runtime_cheats_state(i);
        if ((st & CHEAT_AT_LAUNCH) && !(st & CHEAT_REFUSED) && !!(st & CHEAT_ON) != !!(st & CHEAT_APPLIED)) n++;
    }
    return n;
}
static int cheats_any_on(void) { for (int i = 0; i < runtime_cheats_count(); i++) if (runtime_cheats_state(i) & CHEAT_ON) return 1; return 0; }
/* The line under a cheat: how it applies, or why it cannot. */
static NSString *cheat_detail(const RuntimeCheatInfo *c, NSColor **color) {
    int st = c->state; *color = NSColor.secondaryLabelColor;
    if (st & CHEAT_REFUSED) { *color = NSColor.systemOrangeColor; return [NSString stringWithFormat:@"Can't be used here: %s", c->problem]; }
    if (c->problem[0]) { *color = NSColor.systemOrangeColor; return @(c->problem); }   /* a conflict, a refused write */
    if (st & CHEAT_AT_LAUNCH) {
        if ((st & CHEAT_ON) && !(st & CHEAT_APPLIED)) { *color = NSColor.systemOrangeColor; return @"Applies after restart"; }
        if (!(st & CHEAT_ON) && (st & CHEAT_APPLIED)) { *color = NSColor.systemOrangeColor; return @"Turns off after restart"; }
        if (runtime_cheats_live_code()) {
            if (st & CHEAT_ON) { *color = NSColor.systemGreenColor; return @"Code patch: running now"; }
            return @"Code patch: takes effect at once";
        }
        return st & CHEAT_ON ? @"Code patch: applied at launch" : @"Code patch: applies at the next start";
    }
    if (st & CHEAT_ON) { *color = NSColor.systemGreenColor; return @"Live: running now"; }
    return @"Live: takes effect at once";
}
/* A row of the Cheats tab: a title, a line about it (or none), a line under it, and a control on the right (or none). */
- (NSView *)cheatsRow:(NSString *)title detail:(NSString *)detail color:(NSColor *)color control:(NSView *)control {
    return [self cheatsRow:title about:nil detail:detail color:color control:control];
}
- (NSView *)cheatsRow:(NSString *)title about:(NSString *)about detail:(NSString *)detail color:(NSColor *)color control:(NSView *)control {
    NSView *row = [NSView new]; row.wantsLayer = YES; row.layer.cornerRadius = 8; row.translatesAutoresizingMaskIntoConstraints = NO;
    NSTextField *label = [NSTextField wrappingLabelWithString:title];
    label.font = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium]; label.textColor = NSColor.labelColor;
    CGFloat textWidth = control ? 300 : PANEL_WIDTH - 2 * 8 - 14 - 24;
    label.preferredMaxLayoutWidth = textWidth;
    NSStackView *text = [NSStackView stackViewWithViews:@[label]]; text.orientation = NSUserInterfaceLayoutOrientationVertical;
    text.alignment = NSLayoutAttributeLeading; text.spacing = 2;
    if (about.length) {
        NSTextField *a = [NSTextField wrappingLabelWithString:about];
        a.font = [NSFont systemFontOfSize:11]; a.textColor = NSColor.secondaryLabelColor; a.preferredMaxLayoutWidth = textWidth;
        a.selectable = NO; [text addArrangedSubview:a];
    }
    if (detail.length) {
        NSTextField *help = [NSTextField wrappingLabelWithString:detail];
        help.font = [NSFont systemFontOfSize:11]; help.textColor = color ?: NSColor.secondaryLabelColor; help.preferredMaxLayoutWidth = textWidth;
        help.selectable = NO; [text addArrangedSubview:help];
    }
    text.translatesAutoresizingMaskIntoConstraints = NO; [row addSubview:text];
    NSMutableArray *c = [NSMutableArray arrayWithArray:@[
        [row.widthAnchor constraintEqualToConstant:PANEL_WIDTH - 2 * 8 - 14],
        [text.leadingAnchor constraintEqualToAnchor:row.leadingAnchor constant:12], [text.topAnchor constraintEqualToAnchor:row.topAnchor constant:8],
        [text.bottomAnchor constraintEqualToAnchor:row.bottomAnchor constant:-8], [text.widthAnchor constraintLessThanOrEqualToConstant:textWidth]]];
    if (control) {
        control.translatesAutoresizingMaskIntoConstraints = NO; [row addSubview:control];
        [c addObjectsFromArray:@[[control.trailingAnchor constraintEqualToAnchor:row.trailingAnchor constant:-10],
            [control.centerYAnchor constraintEqualToAnchor:row.centerYAnchor],
            [control.leadingAnchor constraintGreaterThanOrEqualToAnchor:text.trailingAnchor constant:8]]];
    }
    [NSLayoutConstraint activateConstraints:c];
    return row;
}
- (NSButton *)cheatsButton:(NSString *)title action:(SEL)action {
    NSButton *b = [NSButton buttonWithTitle:title target:self action:action];
    b.controlSize = NSControlSizeSmall; b.bezelStyle = NSBezelStyleRounded; b.refusesFirstResponder = YES;
    return b;
}
/* A group's title over its rows (not focused): a file, or a heading the file itself has. */
- (void)cheatsHeading:(NSString *)title detail:(NSString *)detail problem:(NSString *)problem big:(BOOL)big {
    NSTextField *label = [NSTextField wrappingLabelWithString:title];
    label.font = big ? [NSFont systemFontOfSize:12 weight:NSFontWeightBold] : [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];
    label.textColor = big ? NSColor.labelColor : NSColor.secondaryLabelColor; label.preferredMaxLayoutWidth = PANEL_WIDTH - 60;
    NSStackView *box = [NSStackView stackViewWithViews:@[label]]; box.orientation = NSUserInterfaceLayoutOrientationVertical;
    box.alignment = NSLayoutAttributeLeading; box.spacing = 1; box.edgeInsets = NSEdgeInsetsMake(big ? 12 : 6, 12, 2, 12);
    for (int i = 0; i < 2; i++) {
        NSString *t = i ? problem : detail; if (!t.length) continue;
        NSTextField *l = [NSTextField wrappingLabelWithString:t];
        l.font = [NSFont systemFontOfSize:10.5]; l.textColor = i ? NSColor.systemOrangeColor : NSColor.tertiaryLabelColor;
        l.preferredMaxLayoutWidth = PANEL_WIDTH - 60; l.selectable = NO; [box addArrangedSubview:l];
    }
    [list addArrangedSubview:box];
}
- (void)addCheatsRow:(NSView *)row id:(int)rid { [list addArrangedSubview:row]; [rows addObject:row]; [rowSetting addObject:@(rid)]; }
/* The variants of the choice whose first variant is leader: all of them, and those this port can run. */
static int cheat_choice_members(int leader, int *all, int *usable, int max, int *nusable) {
    int n = 0; *nusable = 0;
    for (int k = leader, total = runtime_cheats_total(); k < total && n < max; k++) {
        RuntimeCheatInfo c; if (!runtime_cheats_info(k, &c) || c.hide || c.choice != leader) continue;
        all[n++] = k;
        if (!(c.state & CHEAT_REFUSED)) usable[(*nusable)++] = k;
    }
    return n;
}
/* The community cheat list: one heading per group, a switch per cheat, a menu per choice; the cheats this port cannot run
   folded into one row at the end. */
- (void)buildCheatList {
    int db = runtime_cheats_db_source(), total = runtime_cheats_total(), dl = settings_index("interface.download_cheats");
    NSString *dbStatus = [TOTKCheatDb status];
    int usable = 0;
    NSMutableArray<NSNumber *> *groups[CHEAT_GROUPS], *refused = [NSMutableArray array];
    for (int g = 0; g < CHEAT_GROUPS; g++) groups[g] = [NSMutableArray array];
    for (int k = 0; k < total && db >= 0; k++) {
        RuntimeCheatInfo c; if (!runtime_cheats_info(k, &c) || c.source != db || c.role != 0 || c.hide) continue;
        if (c.choice >= 0 && c.choice != k) continue;   /* a later variant: an option of its choice's row */
        int all[16], ok[16], nok = 0;
        if (c.choice >= 0) cheat_choice_members(k, all, ok, 16, &nok); else nok = !(c.state & CHEAT_REFUSED);
        if (!nok) { [refused addObject:@(k)]; continue; }
        [groups[c.group >= 0 && c.group < CHEAT_GROUPS ? c.group : CHEAT_GROUP_OTHER] addObject:@(k)];
        usable++;
    }
    NSString *about = db >= 0
        ? [NSString stringWithFormat:@"%d cheats for this game version from the community database " CDB_SOURCE ", by their authors. %@", usable, dbStatus]
        : [TOTKCheatDb busy] ? @"Downloading the community cheat list (" CDB_SOURCE ") for this game version…"
        : [NSString stringWithFormat:@"The community database (" CDB_SOURCE ") has no cheats for this game version yet, or the list could not be downloaded. %@", dbStatus];
    NSButton *update = [self cheatsButton:[TOTKCheatDb busy] ? @"Checking…" : @"Update" action:@selector(updateCheatList)];
    update.enabled = ![TOTKCheatDb busy];
    [self addCheatsRow:[self cheatsRow:@"Cheat list" detail:about color:[TOTKCheatDb problem] ? NSColor.systemOrangeColor : nil control:update] id:CHEATS_DB_ROW];
    if (dl >= 0) { NSView *row = [self rowFor:&SETTINGS[dl] index:dl]; [list addArrangedSubview:row]; [rows addObject:row]; [rowSetting addObject:@(dl)]; }
    for (int g = 0; g < CHEAT_GROUPS; g++) {
        if (!groups[g].count) continue;
        [self cheatsHeading:@(CHEAT_GROUP_NAMES[g]) detail:nil problem:nil big:NO];
        for (NSNumber *n in groups[g]) {
            int k = n.intValue; RuntimeCheatInfo c; runtime_cheats_info(k, &c);
            NSColor *color; NSString *detail;
            if (c.choice < 0) {
                detail = cheat_detail(&c, &color);
                NSSwitch *toggle = [NSSwitch new]; toggle.state = (c.state & CHEAT_ON) ? NSControlStateValueOn : NSControlStateValueOff;
                toggle.tag = k; toggle.target = self; toggle.action = @selector(cheatSwitched:); toggle.refusesFirstResponder = YES;
                [self addCheatsRow:[self cheatsRow:@(c.label) about:c.about ? @(c.about) : nil detail:detail color:color control:toggle] id:CHEAT_ROW - k];
                continue;
            }
            int all[16], ok[16], nok, on = -1;
            cheat_choice_members(k, all, ok, 16, &nok);
            NSPopUpButton *menu = [NSPopUpButton new]; [menu addItemWithTitle:@"Off"]; menu.lastItem.tag = -1;
            for (int i = 0; i < nok; i++) {
                RuntimeCheatInfo v; runtime_cheats_info(ok[i], &v);
                [menu addItemWithTitle:@(v.option ? v.option : v.label)]; menu.lastItem.tag = ok[i];
                if ((v.state & CHEAT_ON) && on < 0) { on = ok[i]; [menu selectItem:menu.lastItem]; }
            }
            RuntimeCheatInfo shown; runtime_cheats_info(on >= 0 ? on : ok[0], &shown);
            detail = cheat_detail(&shown, &color);
            if (on < 0 && !(shown.state & CHEAT_APPLIED)) {   /* Off: how a variant would apply, unless one turns off at the restart */
                for (int i = 0; i < nok; i++) { RuntimeCheatInfo v; runtime_cheats_info(ok[i], &v); if (v.state & CHEAT_APPLIED) { detail = cheat_detail(&v, &color); break; } }
            }
            menu.tag = k; menu.target = self; menu.action = @selector(cheatChoiceChanged:); menu.refusesFirstResponder = YES;
            [menu.widthAnchor constraintEqualToConstant:118].active = YES;
            [self addCheatsRow:[self cheatsRow:@(c.label) about:c.about ? @(c.about) : nil detail:detail color:color control:menu] id:CHEAT_ROW - k];
        }
    }
    if (refused.count) {
        NSButton *show = [self cheatsButton:cheatsUnavailableShown ? @"Hide" : @"Show" action:@selector(toggleCheatsUnavailable)];
        [self addCheatsRow:[self cheatsRow:[NSString stringWithFormat:@"%lu not available here", (unsigned long)refused.count]
                                    detail:cheatsUnavailableShown ? @"They patch the game's code in a way this port cannot follow: under a button condition, or where the port patches the game itself."
                                                           : @"They patch the game's code in a way this port cannot follow. Show says why for each."
                                     color:nil control:show] id:CHEATS_UNAVAILABLE_ROW];
        for (NSNumber *n in cheatsUnavailableShown ? refused : @[]) {
            RuntimeCheatInfo c; runtime_cheats_info(n.intValue, &c);
            [self cheatsHeading:@(c.label) detail:nil problem:@(c.problem) big:NO];
        }
    }
}
- (void)buildCheats {
    if (!cheatDbAsked) { cheatDbAsked = YES; [TOTKCheatDb update:NO]; }   /* the first look this run: check if the day is over */
    int total = runtime_cheats_total(), nsrc = runtime_cheats_sources(), db = runtime_cheats_db_source();
    const char *bid = runtime_cheats_build_id(), *folder = runtime_cheats_folder();
    NSString *file = [NSString stringWithFormat:@"%s.txt", *bid ? bid : "<build ID>"];
    BOOL hasFolder = folder && *folder && strcmp(folder, "0");
    NSString *where = hasFolder ? [@(folder) stringByAbbreviatingWithTildeInPath] : @"(off: TOTK_CHEATS_DIR=0)";
    if (runtime_noclip_available()) [self addNoclipRows];   /* world_survey.c: not a cheat file's code, so above the files' cheats */
    if (runtime_cheats_count()) {
        NSButton *off = [self cheatsButton:@"Turn off" action:@selector(cheatsAllOff)]; off.enabled = cheats_any_on() || runtime_noclip_on();
        NSString *how = runtime_cheats_live_code() ? @"All of them stop at once. " : @"Live cheats stop at once; code patches after a restart. ";
        [self addCheatsRow:[self cheatsRow:@"Turn off all cheats" detail:[how stringByAppendingString:
                            @"The first cheat you turn on backs up your saves (profile-before-cheats)."] color:nil control:off] id:CHEATS_OFF_ROW];
    }
    int pending = cheats_pending(), changed = runtime_cheats_files_changed();
    if (pending || changed) {
        NSString *why = pending ? [NSString stringWithFormat:@"%d code patch change%s waiting for the game to start again.", pending, pending == 1 ? "" : "s"]
                      : changed == 2 ? @"A newer cheat list was downloaded: it is read at the next start."
                                     : @"The cheat files changed since the game started: they are read again at the next start.";
        NSButton *restart = runtime_front_can_restart() ? [self cheatsButton:@"Restart now" action:@selector(restartGame)] : nil;
        restart.bezelColor = NSColor.systemOrangeColor;
        [self addCheatsRow:[self cheatsRow:@"Restart to apply" detail:why color:NSColor.systemOrangeColor control:restart] id:CHEATS_RESTART_ROW];
    }
    [self buildCheatList];
    [self cheatsHeading:@"Your own cheat files" detail:nil problem:nil big:YES];
    NSString *about = [NSString stringWithFormat:@"Add cheats of your own: %@ (Atmosphère format, as Yuzu and Ryujinx use; this game's build ID is %s) "
                       "in %@, or in a mod's cheats folder. Files are read when the game starts.", file, *bid ? bid : "unknown", where];
    [self addCheatsRow:[self cheatsRow:@"Cheats folder" detail:about color:nil control:hasFolder ? [self cheatsButton:@"Open" action:@selector(openCheatsFolder)] : nil]
                    id:CHEATS_FOLDER_ROW];
    for (int s = 0; s < nsrc; s++) {
        if (s == db) continue;   /* above */
        [self cheatsHeading:@(runtime_cheats_source_label(s)) detail:[@(runtime_cheats_source_path(s)) stringByAbbreviatingWithTildeInPath]
                    problem:@(runtime_cheats_source_problem(s)) big:YES];
        for (int k = 0; k < total; k++) {
            RuntimeCheatInfo c; if (!runtime_cheats_info(k, &c) || c.source != s) continue;
            if (c.role == 3) continue;   /* a group's end */
            if (c.role == 2) { [self cheatsHeading:@(c.name) detail:nil problem:nil big:NO]; continue; }
            NSColor *color; NSString *detail = cheat_detail(&c, &color);
            if (c.role == 1) {   /* a master code: always on with its file, nothing to switch */
                [self addCheatsRow:[self cheatsRow:@(c.name) detail:[@"Master code: always on. " stringByAppendingString:detail] color:color control:nil] id:CHEAT_ROW - k];
                continue;
            }
            NSSwitch *toggle = [NSSwitch new]; toggle.state = (c.state & CHEAT_ON) ? NSControlStateValueOn : NSControlStateValueOff;
            toggle.enabled = !(c.state & CHEAT_REFUSED); toggle.tag = k; toggle.target = self; toggle.action = @selector(cheatSwitched:);
            toggle.refusesFirstResponder = YES;
            [self addCheatsRow:[self cheatsRow:@(c.name) detail:detail color:color control:toggle] id:CHEAT_ROW - k];
        }
    }
}
/* Noclip's switch and speed: on the Cheats tab, and on the Teleport tab, where switching it off lands. */
- (void)addNoclipRows {
    NSSwitch *fly = [NSSwitch new]; fly.state = runtime_noclip_on() ? NSControlStateValueOn : NSControlStateValueOff;
    fly.target = self; fly.action = @selector(noclipSwitched:);
    [self addCheatsRow:[self cheatsRow:@"Noclip (fly through walls)"
                                about:@"Left stick flies Link relative to the camera; RT / ZR rises, LT / ZL sinks, LB / RB change the speed; the game does not get those four while noclip is on (keyboard: WASD, Shift, Option). On a controller, hold L3 + d-pad down for half a second to turn it on or off. Link does not lose hearts while it is on and for 8 seconds after; switched off in the air, he is lowered to the ground and let go once he stands, so there is no fall. One blow bigger than all his hearts can still kill. Save first: flying out of bounds can break events or drop you into unloaded ground."
                               detail:runtime_noclip_on() ? @"On: Link is held where you steer him" : @"Off at every start"
                                color:runtime_noclip_on() ? NSColor.systemGreenColor : nil control:fly] id:CHEATS_NOCLIP_ROW];
    NSPopUpButton *speed = [NSPopUpButton new]; speed.controlSize = NSControlSizeSmall; speed.refusesFirstResponder = YES;
    for (int i = 0; i < NOCLIP_NSPEEDS; i++) { [speed addItemWithTitle:[NSString stringWithFormat:@"%d m/s", runtime_noclip_speeds[i]]]; speed.lastItem.tag = runtime_noclip_speeds[i]; }
    [speed selectItemWithTag:(NSInteger)lroundf(runtime_noclip_speed())];
    speed.target = self; speed.action = @selector(noclipSpeedChosen:);
    [self addCheatsRow:[self cheatsRow:@"Noclip speed" detail:@"How fast Link flies (← → change it)" color:nil control:speed] id:CHEATS_NOCLIP_SPEED_ROW];
}
/* ---- the Teleport tab: the story lock, noclip, the map pins and the story's places (landmarks.h LANDMARK_GROUPS[0]); each Go
   flies Link there with noclip ---- */
- (void)buildTeleport {
    if (!runtime_noclip_available()) {
        [self addCheatsRow:[self cheatsRow:@"Teleport" detail:@"Not available in this game version: teleporting flies Link there with noclip, which needs the 1.0.0 or 1.4.3 build." color:nil control:nil] id:CHEATS_UNAVAILABLE_ROW];
        return;
    }
    BOOL lockOn = [self addStoryLockRow];
    [self addNoclipRows];
    const char *profile = getenv("TOTK_PROFILE_DIR");
    [self cheatsHeading:@"Map pins" detail:@"Never locked: you placed them." problem:nil big:YES];
    npins = profile ? map_pins_read(profile, pins, MAP_PINS_MAX, NULL, 0) : -1;
    for (int k = 0; k < npins; k++) {
        NSButton *go = [self cheatsButton:@"Go" action:@selector(pinGo:)]; go.tag = k;
        [self addCheatsRow:[self cheatsRow:[NSString stringWithFormat:@"Teleport to the %@ map pin", [@(pins[k].colour) isEqualToString:@"LightBlue"] ? @"light blue" : @(pins[k].colour).lowercaseString]
                                    detail:[NSString stringWithFormat:@"%.0f, %.0f, %.0f · flies you there with noclip (120 m/s; the stick stops it), then holds you: switch noclip off to land", pins[k].xyz[0], pins[k].xyz[1], pins[k].xyz[2]]
                                     color:nil control:go] id:CHEATS_PIN_ROW - k];
    }
    if (npins <= 0)
        [self addCheatsRow:[self cheatsRow:@"Teleport to a map pin" detail:npins < 0 ? @"No save yet." :
                             @"No map pins in the last save: place a pin on the map, then save (or wait for an autosave)." color:nil control:nil] id:CHEATS_PIN_ROW - MAP_PINS_MAX];
    [self buildLandmarks:profile lock:lockOn group:0];
}
/* The story lock's switch (teleport.story_lock) and direct or fly (teleport.direct) at the top of Teleport, Towers,
   Shrines and Lightroots; YES while the lock is on. */
- (BOOL)addStoryLockRow {
    int lock = settings_index("teleport.story_lock");
    for (int i = 0; i < 2; i++) {
        int k = i ? settings_index("teleport.direct") : lock;
        if (k >= 0) { NSView *row = [self rowFor:&SETTINGS[k] index:k]; [list addArrangedSubview:row]; [rows addObject:row]; [rowSetting addObject:@(k)]; }
    }
    return lock >= 0 && settings_get("teleport.story_lock");
}
/* ---- the Towers, Shrines and Lightroots tabs: one category of landmarks.h each (LANDMARK_GROUPS g), the story lock above it; Shrines
   also has auto shrine, and a Finish per shrine ---- */
- (void)buildPlaces:(int)g {
    if (!runtime_noclip_available()) {
        [self addCheatsRow:[self cheatsRow:@(LANDMARK_GROUPS[g].title) detail:@"Not available in this game version: teleporting flies Link there with noclip, which needs the 1.0.0 or 1.4.3 build." color:nil control:nil] id:CHEATS_UNAVAILABLE_ROW];
        return;
    }
    BOOL lockOn = [self addStoryLockRow];
    if (landmark_is_shrine(LANDMARK_GROUPS[g].first) && runtime_autoshrine_available()) [self addAutoShrineRows];
    [self buildLandmarks:getenv("TOTK_PROFILE_DIR") lock:lockOn group:g];
}
/* Auto shrine (world_survey.c, shrine_auto.h): Start / Stop, the chests switch, and the run's status under them. */
- (void)addAutoShrineRows {
    char line[600]; int running = runtime_autoshrine_status(line, sizeof line);
    autoShrineShown = @(line);
    [self cheatsHeading:@"Auto shrine" detail:@"Completes shrines one after another, nearest first: the game itself gives each Light of Blessing." problem:nil big:YES];
    NSButton *go = [self cheatsButton:running ? @"Stop" : @"Start" action:@selector(autoShrineStartStop)];
    if (running) go.bezelColor = NSColor.systemOrangeColor;
    [self addCheatsRow:[self cheatsRow:@"Complete shrines automatically"
                                 about:@"Flies Link with noclip to the nearest shrine not completed yet, opens its gate (A) and walks him in, skips the cutscenes the game lets you skip, takes him to the altar for the Light of Blessing (the game completes the shrine and takes him out, as if you had walked there), then on to the next. Leaves out shrines the story lock greys, hidden and crystal shrines not open yet, and shrines in caves. Any key, button or stick of yours stops it. Save first."
                                detail:autoShrineShown color:running ? NSColor.systemGreenColor : nil control:go] id:CHEATS_AUTOSHRINE_ROW];
    NSSwitch *chests = [NSSwitch new]; chests.state = runtime_autoshrine_chests() ? NSControlStateValueOn : NSControlStateValueOff;
    chests.target = self; chests.action = @selector(autoShrineChestsSwitched:); chests.refusesFirstResponder = YES;
    [self addCheatsRow:[self cheatsRow:@"Open chests too" detail:@"Stops at each chest in the shrine on the way to the altar and presses A in front of it (best effort: some chests sit behind walls or under water)."
                                 color:nil control:chests] id:CHEATS_AUTOSHRINE_CHESTS_ROW];
}
- (void)autoShrineStartStop {
    char line[600];
    if (runtime_autoshrine_status(line, sizeof line)) { runtime_autoshrine_stop(); [self rebuildKeepingPlace]; return; }
    if (runtime_autoshrine_start(0)) [self hide];
}
- (void)autoShrineChestsSwitched:(NSSwitch *)s { runtime_autoshrine_set_chests(s.state == NSControlStateValueOn); [self.window makeFirstResponder:self]; }
/* Category only's rows: the story's places as they are; the towers, shrines and Lightroots what the newest save has not done
   yet first, then the done ones, each part nearest Link first (by name while there is no player). Locked places (the story lock) stay listed, greyed with the reason and no Go. A shrine also has Finish (auto shrine
   for that shrine alone; → on the row). */
- (void)buildLandmarks:(const char *)profile lock:(BOOL)lockOn group:(int)only {
    if (!landmarks_read) {
        landmarks_read = 1;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            Landmark *got = calloc(LANDMARKS, sizeof *got);
            LandmarkRomfs rom = {runtime_romfs_file_count, runtime_romfs_file_name, runtime_romfs_host_read, runtime_romfs_zs_decompress};
            int placed = got ? landmarks_load(&rom, got) : 0;
            dispatch_async(dispatch_get_main_queue(), ^{
                if (placed) memcpy(landmark_places, got, sizeof landmark_places);
                free(got); landmarks_read = placed ? 2 : 3;
                if (self.superview && (self.kind == MENU_TELEPORT || self.kind == MENU_TOWERS || self.kind == MENU_SHRINES || self.kind == MENU_LIGHTROOTS || self.kind == MENU_TEARS)) [self rebuildKeepingPlace];
            });
        });
    }
    if (landmarks_read == 1) {
        [self cheatsHeading:@(LANDMARK_GROUPS[only].title) detail:nil problem:nil big:YES];
        [self addCheatsRow:[self cheatsRow:@"Reading the game's map…" detail:nil color:nil control:nil] id:CHEATS_GROUP_ROW];
        return;
    }
    if (landmarks_read != 2) return;
    memcpy(marks, landmark_places, sizeof marks);
    LandmarkProgress progress;
    int saved = profile ? landmarks_state_read(profile, marks, &progress) : -1;
    if (saved < 0) progress = (LandmarkProgress){0, 0, 0};
    landmarks_lock(marks, &progress, lockOn);
    for (int g = only; g == only && g < LANDMARK_NGROUPS; g++) {
        const LandmarkGroup *group = &LANDMARK_GROUPS[g];
        int done = 0, known = 0, locked = 0;
        NSMutableArray<NSNumber *> *order = [NSMutableArray array];
        for (int k = group->first; k < group->first + group->count; k++) {
            if (!marks[k].placed) continue;
            [order addObject:@(k)]; known += marks[k].state != LANDMARK_UNKNOWN; done += marks[k].state == LANDMARK_DONE; locked += marks[k].lock != LANDMARK_OPEN;
        }
        if (!order.count) continue;
        Landmark *m = marks; float xyz[3]; BOOL near = runtime_player_pos(xyz); float lx = xyz[0], ly = xyz[1], lz = xyz[2];   /* blocks cannot capture an array */
        float (^away)(int) = ^float(int k) { float dx = m[k].at[0] - lx, dy = m[k].at[1] - ly, dz = m[k].at[2] - lz; return sqrtf(dx * dx + dy * dy + dz * dz); };
        if (group->fold) [order sortUsingComparator:^NSComparisonResult(NSNumber *a, NSNumber *b) {
            int x = (m[a.intValue].state == LANDMARK_DONE), y = (m[b.intValue].state == LANDMARK_DONE);
            if (x != y) return x < y ? NSOrderedAscending : NSOrderedDescending;
            if (near) { float da = away(a.intValue), db = away(b.intValue); if (da != db) return da < db ? NSOrderedAscending : NSOrderedDescending; }
            return [@(m[a.intValue].name) localizedStandardCompare:@(m[b.intValue].name)];
        }];
        BOOL towers = group->first == 0, story = landmark_is_story(group->first), shrines = landmark_is_shrine(group->first), roots = landmark_is_lightroot(group->first), tears = landmark_is_tear(group->first);
        char autoLine[600]; BOOL running = shrines && runtime_autoshrine_status(autoLine, sizeof autoLine);
        NSString *count = !known ? (saved < 0 ? @"No save yet: shown by name." : @"The last save does not say which are done: shown by name.")
                        : [NSString stringWithFormat:story ? @"%d of %lu completed in the last save." : towers ? @"%d of %lu activated in the last save; the rest first."
                                                     : roots ? @"%d of %lu lit in the last save; the rest first." : tears ? @"%d of %lu seen in the last save; the rest first."
                                                     : @"%d of %lu completed in the last save; the rest first.", done, (unsigned long)order.count];
        if (group->fold && known) count = [count stringByAppendingString:near ? @" Nearest first." : @" By name (no player yet)."];
        if (locked) count = [count stringByAppendingFormat:@" %d locked by the story so far.", locked];
        else if (lockOn && !progress.known && story)
            count = [count stringByAppendingString:saved < 0 ? @" Nothing is locked without a save." : @" The last save does not say where the story is: nothing is locked."];
        NSString *lands = story ? @"Lands at the place's travel point; flies you there with noclip, as a map pin does."
                        : towers ? @"Lands inside the tower by its door; flies you there with noclip, as a map pin does."
                        : roots ? @"Lands on the Lightroot's travel point; flies you there with noclip, as a map pin does."
                        : tears ? @"Lands on the tear, in its geoglyph; flies you there with noclip, as a map pin does."
                                 : @"Go lands at the shrine's travel point (noclip, as a map pin does); Finish (→ on the row) completes that shrine alone, as auto shrine does.";
        [self cheatsHeading:@(group->title) detail:[NSString stringWithFormat:@"%@ %@", count, lands] problem:nil big:YES];
        for (NSNumber *n in order) {
            int k = n.intValue; const Landmark *l = &marks[k];
            static const char *const tower_state[] = {"", "Not activated", "Found, not activated", "Activated"}, *const shrine_state[] = {"", "Not found yet", "Found, not completed", "Completed"},
                              *const story_state[] = {"", "Not visited yet", "Visited", "Completed"}, *const root_state[] = {"", "Not found yet", "Found, not lit", "Lit"},
                              *const tear_state[] = {"", "Not seen yet", "Not seen yet", "Seen"};
            const char *state = (story ? story_state : towers ? tower_state : roots ? root_state : tears ? tear_state : shrine_state)[l->state];
            NSButton *go = [self cheatsButton:@"Go" action:@selector(landmarkGo:)]; go.tag = k; go.enabled = l->lock == LANDMARK_OPEN;
            NSView *control = go;
            if (shrines) {
                NSButton *finish = [self cheatsButton:@"Finish" action:@selector(shrineFinish:)]; finish.tag = k;
                finish.enabled = l->lock == LANDMARK_OPEN && l->state != LANDMARK_DONE && !running;
                NSStackView *both = [NSStackView stackViewWithViews:@[go, finish]]; both.spacing = 6; control = both;
            }
            NSString *detail = [NSString stringWithFormat:@"%s%s%.0f, %.0f, %.0f%s", state, *state ? " · " : "", l->at[0], l->at[1], l->at[2], l->depths && !roots ? " · in the Depths" : ""];
            if (near && group->fold) { float d = away(k); detail = [detail stringByAppendingFormat:d < 1000 ? @" · %.0f m away" : @" · %.1f km away", d < 1000 ? d : d / 1000]; }
            if (l->lock != LANDMARK_OPEN) detail = [NSString stringWithFormat:@"%s · %@", LANDMARK_LOCK_TEXT[l->lock], detail];
            [self addCheatsRow:[self cheatsRow:@(l->name) detail:detail color:l->lock != LANDMARK_OPEN ? NSColor.tertiaryLabelColor : l->state == LANDMARK_DONE ? NSColor.systemGreenColor : nil
                                       control:control] id:CHEATS_LANDMARK_ROW - k];
            if (l->lock != LANDMARK_OPEN) rows.lastObject.alphaValue = 0.55;
        }
    }
}
- (void)rebuildKeepingPlace {   /* the focus stays on the same row, though Restart to apply may come or go above it */
    NSNumber *keep = focus < (NSInteger)rowSetting.count ? rowSetting[focus] : nil; CGFloat y = scroll.contentView.bounds.origin.y;
    [self buildPage];
    NSUInteger at = keep ? [rowSetting indexOfObject:keep] : NSNotFound;
    if (at != NSNotFound) focus = (NSInteger)at;
    [self showFocus];
    [scroll.contentView scrollToPoint:NSMakePoint(0, y)]; [scroll reflectScrolledClipView:scroll.contentView];
}
- (void)setCheat:(int)k on:(int)on {
    RuntimeCheatInfo c;
    if (!runtime_cheats_info(k, &c) || c.role != 0 || (c.state & CHEAT_REFUSED) || !!(c.state & CHEAT_ON) == !!on) return;
    runtime_cheats_set_entry(k, on);
    [self rebuildKeepingPlace];
}
- (void)cheatSwitched:(NSSwitch *)toggle {
    NSUInteger at = [rowSetting indexOfObject:@(CHEAT_ROW - (int)toggle.tag)]; if (at != NSNotFound) { focus = at; onBar = NO; }
    [self setCheat:(int)toggle.tag on:toggle.state == NSControlStateValueOn];
    [self.window makeFirstResponder:self];
}
/* A choice: Off (chosen = -1) or one variant; the others are switched off first. */
- (void)setCheatChoice:(int)leader to:(int)chosen {
    int all[16], ok[16], nok, n = cheat_choice_members(leader, all, ok, 16, &nok);
    for (int i = 0; i < n; i++) {
        RuntimeCheatInfo c;
        if (all[i] != chosen && runtime_cheats_info(all[i], &c) && (c.state & CHEAT_ON)) runtime_cheats_set_entry(all[i], 0);
    }
    RuntimeCheatInfo c;
    if (chosen >= 0 && runtime_cheats_info(chosen, &c) && !(c.state & CHEAT_ON)) runtime_cheats_set_entry(chosen, 1);
    [self rebuildKeepingPlace];
}
- (void)cheatChoiceChanged:(NSPopUpButton *)menu {
    NSUInteger at = [rowSetting indexOfObject:@(CHEAT_ROW - (int)menu.tag)]; if (at != NSNotFound) { focus = at; onBar = NO; }
    [self setCheatChoice:(int)menu.tag to:(int)menu.selectedItem.tag];
    [self.window makeFirstResponder:self];
}
/* Left / right: the previous / next option; Return / A: the next, round to Off. */
- (void)stepCheatChoice:(int)leader dir:(int)dir {
    int all[16], ok[16], nok, at = 0;
    cheat_choice_members(leader, all, ok, 16, &nok);
    for (int i = 0; i < nok; i++) { RuntimeCheatInfo c; if (runtime_cheats_info(ok[i], &c) && (c.state & CHEAT_ON)) { at = i + 1; break; } }
    int next = dir ? at + dir : (at + 1) % (nok + 1);
    if (next < 0 || next > nok || next == at) return;
    [self setCheatChoice:leader to:next ? ok[next - 1] : -1];
}
- (void)updateCheatList { [TOTKCheatDb update:YES]; }
- (void)toggleCheatsUnavailable { cheatsUnavailableShown = !cheatsUnavailableShown; [self rebuildKeepingPlace]; }
- (void)goToPin:(int)k {
    if (k < 0 || k >= npins) return;
    float at[3] = {pins[k].xyz[0], pins[k].xyz[1] + 2, pins[k].xyz[2]};   /* a little above the pin: it sits on the ground */
    if ([self teleport:at]) [self hide];
}
/* Go: flown there with noclip, or put there at once (teleport.direct). */
- (BOOL)teleport:(const float *)at { return settings_get("teleport.direct") ? runtime_noclip_jump(at) : runtime_noclip_goto(at); }
- (void)pinGo:(NSButton *)b { [self goToPin:(int)b.tag]; }
- (void)goToLandmark:(int)k {
    if (k < 0 || k >= LANDMARKS || !marks[k].placed || marks[k].lock != LANDMARK_OPEN) return;   /* locked: the story lock */
    if ([self teleport:marks[k].at]) [self hide];   /* at: already above the floor (landmarks_place) */
}
- (void)landmarkGo:(NSButton *)b { [self goToLandmark:(int)b.tag]; }
- (void)finishShrine:(int)k {   /* auto shrine for that shrine alone */
    if (!landmark_is_shrine(k) || !marks[k].placed || marks[k].lock != LANDMARK_OPEN || marks[k].state == LANDMARK_DONE) return;
    if (runtime_autoshrine_finish(k - LANDMARK_FIRST_SHRINE)) [self hide];
}
- (void)shrineFinish:(NSButton *)b { [self finishShrine:(int)b.tag]; }
- (void)noclipSwitched:(NSSwitch *)s { runtime_noclip_set(s.state == NSControlStateValueOn); [self rebuildKeepingPlace]; }
- (void)noclipSpeedChosen:(NSPopUpButton *)b { runtime_noclip_set_speed((float)b.selectedTag); [self.window makeFirstResponder:self]; }
- (void)stepNoclipSpeed:(int)dir {
    runtime_noclip_step_speed(dir); [self rebuildKeepingPlace];
}
- (void)cheatsAllOff {
    runtime_noclip_set(0);
    for (int i = 0; i < runtime_cheats_count(); i++) if (runtime_cheats_state(i) & CHEAT_ON) runtime_cheats_set(i, 0);
    [self rebuildKeepingPlace];
}
- (void)openCheatsFolder {
    const char *folder = runtime_cheats_folder();
    if (folder && *folder && strcmp(folder, "0")) [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:@(folder) isDirectory:YES]];
}
+ (void)showCheats {
    TOTKSettingsPanel *p = [self shared];
    for (int k = 0; k < SETTINGS_NSECTIONS; k++) if (SETTINGS_SECTIONS[k].kind == MENU_CHEATS) p->section = k;
    p->focus = 0; p->onBar = NO;
    if (p.superview) [p buildPage]; else [p show];
}

/* ---- tabs and sections ---- */

/* Tools › Spawn: spawn_panel_mac.h does the work; the menu keeps the levels above it. */
- (TOTKSpawnPane *)makeSpawnPane { return [[TOTKSpawnPane alloc] initWithMenu:self]; }
- (void)refreshSpawn { [spawn refresh]; }
- (void)spawnShowFocus { [spawn showFocus:!onBar]; }
- (NSString *)spawnHint { return spawn.hint; }
- (void)spawnEnter { [spawn enter]; }
- (BOOL)spawnBack { return [spawn back]; }
- (void)spawnMove:(int)dir { [spawn moveFocus:dir]; }
- (void)spawnStep:(int)dir { [spawn step:dir]; }
- (void)spawnPage:(int)dir { [spawn page:dir]; }
- (BOOL)spawnKey:(NSEvent *)e { return [spawn key:e]; }
- (void)spawnButton:(char)b { [spawn button:b]; }

- (int)kind { return SETTINGS_SECTIONS[section].kind; }
- (int)tab { return SETTINGS_SECTIONS[section].tab; }
- (void)loadSectionBar {
    int first = settings_tab_first(self.tab), n = settings_tab_sections(self.tab);
    sectionBar.segmentCount = n;
    for (int k = 0; k < n; k++) { [sectionBar setLabel:@(SETTINGS_SECTIONS[first + k].name) forSegment:k]; [sectionBar setWidth:0 forSegment:k]; }
    sectionBar.selectedSegment = section - first;
    tabs.selectedSegment = self.tab;
}
- (void)showSection:(NSInteger)k {
    if (k < 0 || k >= SETTINGS_NSECTIONS) return;
    section = k; lastSection[self.tab] = k; focus = 0; confirmReset = NO;
    [self buildPage];
}

- (void)refreshHint {
    int kind = self.kind;
    hint.stringValue = confirmReset ? RESET_CONFIRM_HINT : onBar ? BAR_HINT : kind == MENU_MODS ? MODS_HINT : kind == MENU_SPAWN ? [self spawnHint] : SETTINGS_HINT;
    hint.textColor = confirmReset ? NSColor.systemOrangeColor : NSColor.tertiaryLabelColor;
    resetTab.hidden = confirmReset; resetCancel.hidden = !confirmReset;
    resetAll.title = confirmReset ? @"Reset everything" : @"Reset everything…";
    resetAll.bezelColor = confirmReset ? NSColor.systemRedColor : nil;
    resetTab.title = [NSString stringWithFormat:@"Reset %s", SETTINGS_TABS[self.tab]];
}
- (void)buildPage {
    int kind = findQuery.length ? MENU_SETTINGS : self.kind;   /* a search lists matching settings from every section */
    BOOL modsPage = kind == MENU_MODS, spawnPage = kind == MENU_SPAWN;
    scroll.hidden = modsPage || spawnPage; mods.hidden = !modsPage; spawn.hidden = !spawnPage;
    [self loadSectionBar]; [self refreshHint];
    if (self.superview) [self place];
    if (modsPage) { [mods refresh]; [self refreshStatus]; [self showFocus]; return; }
    if (spawnPage) { [self refreshSpawn]; [self refreshStatus]; [self showFocus]; return; }
    for (NSView *v in list.arrangedSubviews.copy) { [list removeArrangedSubview:v]; [v removeFromSuperview]; }
    [rows removeAllObjects]; [rowSetting removeAllObjects];
    for (int i = 0; i < SETTINGS_COUNT && kind == MENU_SETTINGS; i++) {
        const SettingDef *d = &SETTINGS[i];
        if (!settings_row_shown(d)) continue;
        if (findQuery.length ? !settings_matches(d, findQuery) : strcmp(d->page, SETTINGS_SECTIONS[section].name) != 0) continue;
        NSView *row = [self rowFor:d index:i];
        [list addArrangedSubview:row]; [rows addObject:row]; [rowSetting addObject:@(i)];
    }
    if (kind == MENU_CHEATS) [self buildCheats];
    else if (kind == MENU_TELEPORT) [self buildTeleport];
    else if (kind == MENU_TOWERS) [self buildPlaces:1];
    else if (kind == MENU_SHRINES) [self buildPlaces:2];
    else if (kind == MENU_LIGHTROOTS) [self buildPlaces:3];
    else if (kind == MENU_TEARS) [self buildPlaces:4];
    else if (kind == MENU_SAVES) {
        NSView *saves = [self savesRow];
        [list addArrangedSubview:saves]; [rows addObject:saves]; [rowSetting addObject:@(SAVES_ROW)];
    }
    if (!rows.count) {
        NSTextField *none = [NSTextField labelWithString:findQuery.length ? @"No setting matches." : @"Nothing to set here on the Mac."]; none.textColor = NSColor.secondaryLabelColor;
        [list addArrangedSubview:none];
    }
    if (focus >= (NSInteger)rows.count) focus = rows.count ? rows.count - 1 : 0;
    [self refreshStatus]; [self showFocus];
}

- (void)refreshStatus {
    const char *s = settings_display_status();
    /* The memory meter: what the GPU holds against the budget the game stops at (bigger sizes, Maximum render resolution). */
    uint64_t used = 0, budget = 0; mtl_gpu_memory(&used, &budget);
    NSMutableAttributedString *line = [[NSMutableAttributedString alloc] initWithString:s && *s ? @(s) : @"" attributes:@{
        NSFontAttributeName:status.font, NSForegroundColorAttributeName:NSColor.secondaryLabelColor}];
    if (budget) {
        double f = (double)used / budget;
        NSColor *c = f > 0.9 ? NSColor.systemRedColor : f > 0.8 ? NSColor.systemOrangeColor : NSColor.secondaryLabelColor;
        [line appendAttributedString:[[NSAttributedString alloc] initWithString:[NSString stringWithFormat:@"\nGPU memory %.1f of %.1f GB%@",
            used / 1073741824.0, budget / 1073741824.0, f > 0.8 ? @": near the limit, the game stops past it" : @""]
            attributes:@{NSFontAttributeName:status.font, NSForegroundColorAttributeName:c}]];
    }
    status.attributedStringValue = line;
    [self refreshMemoryRows:used budget:budget];
    int pending = mods_changed_since_start();   /* Restart now: a setting marked (after restart), or mod changes */
    for (int i = 0; i < SETTINGS_COUNT && !pending; i++) pending = settings_pending_restart(SETTINGS[i].key);
    if (!pending) pending = cheats_pending() || runtime_cheats_files_changed();   /* code patches switched, cheat files changed */
    restartNow.hidden = !pending || !runtime_front_can_restart();
}
/* The memory rows on this page: the note under the label and the marks in the selector, as memory changes. */
static NSView *settings_find_id(NSView *v, NSString *ident) {
    if ([v.identifier isEqual:ident]) return v;
    for (NSView *sub in v.subviews) { NSView *t = settings_find_id(sub, ident); if (t) return t; }
    return nil;
}
static NSPopUpButton *settings_find_popup(NSView *v) {
    if ([v isKindOfClass:NSPopUpButton.class]) return (NSPopUpButton *)v;
    for (NSView *sub in v.subviews) { NSPopUpButton *p = settings_find_popup(sub); if (p) return p; }
    return nil;
}
- (void)refreshMemoryRows:(uint64_t)used budget:(uint64_t)budget {
    for (NSUInteger k = 0; k < rows.count && k < rowSetting.count; k++) {
        int i = rowSetting[k].intValue; if (i < 0 || i >= SETTINGS_COUNT || !settings_mem_row(&SETTINGS[i])) continue;
        const SettingDef *d = &SETTINGS[i];
        NSTextField *note = (NSTextField *)settings_find_id(rows[k], @"memnote"); NSColor *c = nil;
        NSString *text = settings_mem_note(d, used, budget, &c);
        if (note && ![note.stringValue isEqualToString:text ?: @""]) { note.stringValue = text ?: @""; note.hidden = !text; }
        if (note && c) note.textColor = c;
        NSPopUpButton *p = settings_find_popup(rows[k]);
        for (int j = 0; p && j < d->nchoices && j < p.numberOfItems; j++) {
            NSString *t = settings_choice_title(d, j, used, budget);
            if (![[p itemAtIndex:j].title isEqualToString:t]) [p itemAtIndex:j].title = t;
        }
    }
}
- (void)restartGame { [self hide]; runtime_front_restart(); }
- (void)meterTick {
    if (!self.superview) return;
    [self refreshStatus];
    if (self.kind == MENU_SHRINES && autoShrineShown) {   /* the auto shrine status line follows the run */
        char line[600]; runtime_autoshrine_status(line, sizeof line);
        if (![autoShrineShown isEqualToString:@(line)]) [self rebuildKeepingPlace];
    }
}   /* the memory meter, once a second while the menu shows */

- (void)focusFind { [self.window makeFirstResponder:find]; }
- (void)searched:(NSSearchField *)f {
    NSString *q = [f.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if ([q isEqualToString:findQuery]) return;
    findQuery = q; focus = 0; onBar = !q.length; [self buildPage];
}
- (BOOL)control:(NSControl *)c textView:(NSTextView *)tv doCommandBySelector:(SEL)cmd {
    (void)tv; if (c != find) return NO;
    if (cmd == @selector(cancelOperation:)) {   /* Esc: clear and back to the sections */
        find.stringValue = @""; [self searched:find]; [self.window makeFirstResponder:self]; return YES;
    }
    if (cmd == @selector(moveDown:) || cmd == @selector(insertNewline:)) {   /* into the results */
        [self.window makeFirstResponder:self]; onBar = !rows.count; focus = 0; [self showFocus]; [self refreshHint]; return YES;
    }
    return NO;
}
static NSTextField *settings_help_label(NSView *v) {   /* a setting row's help text (rowFor:), if it has one */
    if ([v.identifier isEqual:@"help"]) return (NSTextField *)v;
    for (NSView *sub in v.subviews) { NSTextField *t = settings_help_label(sub); if (t) return t; }
    return nil;
}
- (void)showFocus {
    sectionBar.layer.borderWidth = onBar ? 2 : 0;   /* the top level: the section bar has the focus */
    sectionBar.layer.borderColor = NSColor.controlAccentColor.CGColor;
    if (self.kind == MENU_SPAWN) { [self spawnShowFocus]; return; }
    for (NSUInteger k = 0; k < rows.count; k++) {
        BOOL on = !onBar && (NSInteger)k == focus;
        rows[k].layer.backgroundColor = on ? panel_color(1, 0.10).CGColor : NSColor.clearColor.CGColor;
        NSTextField *help = settings_help_label(rows[k]);
        if (help && help.maximumNumberOfLines != (on ? 0 : 1)) { help.maximumNumberOfLines = on ? 0 : 1; help.lineBreakMode = on ? NSLineBreakByWordWrapping : NSLineBreakByTruncatingTail; [help invalidateIntrinsicContentSize]; }
    }
    if (!onBar && focus < (NSInteger)rows.count) {
        [list layoutSubtreeIfNeeded];
        [rows[focus] scrollRectToVisible:NSInsetRect(rows[focus].bounds, 0, -8)];
    }
}

/* ---- changes ---- */

- (void)apply:(int)i value:(int)v {
    const SettingDef *d = &SETTINGS[i];
    if (!settings_valid(d, v) || v == settings_get(d->key)) return;
    settings_set(d->key, v);
    if (!strcmp(d->key, "interface.download_cheats") && v) [TOTKCheatDb update:NO];   /* turned on: check if the day is over */
    NSInteger keep = focus; CGFloat y = scroll.contentView.bounds.origin.y;
    [self buildPage];   /* the row's label (next start), rows that depend on it, the status line */
    focus = keep < (NSInteger)rows.count ? keep : focus; [self showFocus];
    [scroll.contentView scrollToPoint:NSMakePoint(0, y)]; [scroll reflectScrolledClipView:scroll.contentView];
}

- (void)changed:(NSControl *)control {
    int i = (int)control.tag; const SettingDef *d = &SETTINGS[i]; int v;
    if ([control isKindOfClass:NSSwitch.class]) v = ((NSSwitch *)control).state == NSControlStateValueOn;
    else if ([control isKindOfClass:NSPopUpButton.class]) v = d->choices[((NSPopUpButton *)control).indexOfSelectedItem].value;
    else v = d->min + (int)lround((((NSSlider *)control).doubleValue - d->min) / d->step) * d->step;
    NSUInteger at = [rowSetting indexOfObject:@(i)]; if (at != NSNotFound) { focus = at; onBar = NO; [self refreshHint]; }
    if (d->type == SET_RANGE) {   /* a drag: change the value and its text, keep the slider (rebuilding would end the drag) */
        for (NSView *v2 in control.superview.subviews) if ([v2.identifier isEqual:@"value"]) ((NSTextField *)v2).stringValue = settings_value_text(d, v);
        if (v != settings_get(d->key)) settings_set(d->key, v);
        [self refreshStatus]; [self showFocus];
        return;
    }
    [self apply:i value:v];
    [self.window makeFirstResponder:self];
}

/* One step left (-1) or right (+1) on the focused row; 0 = activate (toggle, next choice). On the section bar: the
   previous / next section, or 0 to open it. */
- (void)step:(int)dir {
    if (confirmReset) { if (!dir) [self confirmResetEverything]; return; }
    if (onBar) {
        if (!dir) { [self enter]; return; }
        NSInteger first = settings_tab_first(self.tab), next = section + dir;
        if (next >= first && next < first + settings_tab_sections(self.tab)) { [self showSection:next]; }
        return;
    }
    int kind = self.kind;
    if (kind == MENU_MODS) { [mods step:dir]; return; }
    if (kind == MENU_SPAWN) { [self spawnStep:dir]; return; }
    if (focus >= (NSInteger)rows.count) return;
    int i = rowSetting[focus].intValue;
    if (i == SAVES_ROW) { if (!dir) [self sendSaves]; return; }
    if (i == CHEATS_FOLDER_ROW) { if (!dir) [self openCheatsFolder]; return; }
    if (i == CHEATS_OFF_ROW) { if (!dir) [self cheatsAllOff]; return; }
    if (i == CHEATS_NOCLIP_ROW) { runtime_noclip_set(dir ? dir > 0 : !runtime_noclip_on()); [self rebuildKeepingPlace]; return; }
    if (i == CHEATS_NOCLIP_SPEED_ROW) { if (dir) [self stepNoclipSpeed:dir]; return; }
    if (i == CHEATS_AUTOSHRINE_ROW) { if (!dir) [self autoShrineStartStop]; return; }
    if (i == CHEATS_AUTOSHRINE_CHESTS_ROW) { runtime_autoshrine_set_chests(dir ? dir > 0 : !runtime_autoshrine_chests()); [self rebuildKeepingPlace]; return; }
    if (i <= CHEATS_PIN_ROW && i > CHEATS_PIN_ROW - MAP_PINS_MAX - 1) { if (!dir) [self goToPin:CHEATS_PIN_ROW - i]; return; }
    if (i <= CHEATS_LANDMARK_ROW && i > CHEATS_LANDMARK_ROW - LANDMARKS) {   /* A: Go; → on a shrine: Finish */
        if (!dir) [self goToLandmark:CHEATS_LANDMARK_ROW - i]; else if (dir > 0) [self finishShrine:CHEATS_LANDMARK_ROW - i];
        return;
    }
    if (i == CHEATS_RESTART_ROW) { if (!dir && runtime_front_can_restart()) [self restartGame]; return; }
    if (i == CHEATS_DB_ROW) { if (!dir) [self updateCheatList]; return; }
    if (i == CHEATS_UNAVAILABLE_ROW) { if (!dir) [self toggleCheatsUnavailable]; return; }
    if (i <= CHEAT_ROW) {   /* Return / A switches it, left off, right on; a choice steps through its options */
        RuntimeCheatInfo c; int k = CHEAT_ROW - i;
        if (runtime_cheats_info(k, &c) && c.choice == k) [self stepCheatChoice:k dir:dir];
        else if (runtime_cheats_info(k, &c)) [self setCheat:k on:dir ? dir > 0 : !(c.state & CHEAT_ON)];
        return;
    }
    const SettingDef *d = &SETTINGS[i]; int v = settings_get(d->key);
    if (d->type == SET_BOOL) v = dir ? dir > 0 : !v;
    else if (d->type == SET_RANGE) { if (!dir) return; v += dir * d->step; v = v < d->min ? d->min : v > d->max ? d->max : v; }
    else {
        int k = 0; while (k < d->nchoices && d->choices[k].value != v) k++;
        k = dir ? k + dir : (k + 1) % d->nchoices;
        if (k < 0 || k >= d->nchoices) return;
        v = d->choices[k].value;
    }
    [self apply:i value:v];
}

/* From the section bar into the section's rows (the first one). */
- (void)enter {
    if (!onBar) return;
    onBar = NO; focus = 0; [self refreshHint];
    if (self.kind == MENU_SPAWN) [self spawnEnter];
    [self showFocus];
}
/* B / Escape: one level up; the menu closes only from the section bar. */
- (void)back {
    if (confirmReset) { [self cancelReset]; return; }
    if (onBar) { [self hide]; return; }
    if (self.kind == MENU_SPAWN && [self spawnBack]) return;   /* the spawn list goes back to its categories first */
    onBar = YES; [self refreshHint]; [self showFocus];
}

/* Up / down: the rows never wrap round; up from the first row goes back to the section bar, down from it opens it. */
- (void)moveFocus:(int)dir {
    if (confirmReset) return;
    if (onBar) { if (dir > 0) [self enter]; return; }
    int kind = self.kind;
    if (kind == MENU_MODS) { [mods moveFocus:dir]; return; }
    if (kind == MENU_SPAWN) { [self spawnMove:dir]; return; }
    if (!rows.count) { if (dir < 0) [self back]; return; }
    if (focus + dir < 0) { if (dir == -1) [self back]; else { focus = 0; [self showFocus]; } return; }
    focus = MIN((NSInteger)rows.count - 1, focus + dir); [self showFocus];
}
/* LT / RT, Page Up / Down: about one screen of rows. */
- (void)pageFocus:(int)dir {
    if (confirmReset || onBar) return;
    int kind = self.kind;
    if (kind == MENU_MODS) { [mods moveFocus:dir * PAGE_STEP]; return; }
    if (kind == MENU_SPAWN) { [self spawnPage:dir]; return; }
    if (!rows.count) return;
    NSView *doc = scroll.documentView;
    CGFloat height = scroll.contentView.bounds.size.height, at = NSMinY([rows[focus] convertRect:rows[focus].bounds toView:doc]);
    CGFloat target = at + dir * MAX(40, height - 40);
    NSInteger k = focus;
    if (dir > 0) while (k + 1 < (NSInteger)rows.count && NSMinY([rows[k + 1] convertRect:rows[k + 1].bounds toView:doc]) <= target) k++;
    else while (k > 0 && NSMinY([rows[k - 1] convertRect:rows[k - 1].bounds toView:doc]) >= target) k--;
    if (k == focus) k = MAX(0, MIN((NSInteger)rows.count - 1, focus + dir));
    focus = k; [self showFocus];
}
- (void)movePage:(int)dir {   /* the tabs (Q / E, LB / RB): round and round, each on the section it showed last */
    int tab = (self.tab + dir + SETTINGS_NTABS) % SETTINGS_NTABS;
    section = lastSection[tab]; focus = 0; onBar = YES; confirmReset = NO; [self buildPage];
}
- (void)tabClicked:(NSSegmentedControl *)s {
    section = lastSection[s.selectedSegment]; focus = 0; onBar = YES; confirmReset = NO; [self buildPage];
    [self.window makeFirstResponder:self];
}
- (void)sectionClicked:(NSSegmentedControl *)s {
    onBar = YES; [self showSection:settings_tab_first(self.tab) + s.selectedSegment];
    [self.window makeFirstResponder:self];
}

/* ---- reset: settings only (cheats, mods, saves and the spawner's lists are not settings) ---- */

- (void)resetThisTab {
    int tab = self.tab;
    for (int i = 0; i < SETTINGS_COUNT; i++)
        if ((SETTINGS[i].platforms & SET_MAC) && settings_in_tab(i, tab)) settings_set(SETTINGS[i].key, SETTINGS[i].def_mac);
    fprintf(stderr, "[settings] reset tab %s\n", SETTINGS_TABS[tab]);
    confirmReset = NO; [self rebuildKeepingPlace];
}
- (void)resetEverything {
    if (confirmReset) { [self confirmResetEverything]; return; }
    confirmReset = YES; [self refreshHint];
}
- (void)confirmResetEverything {
    for (int i = 0; i < SETTINGS_COUNT; i++) if (SETTINGS[i].platforms & SET_MAC) settings_set(SETTINGS[i].key, SETTINGS[i].def_mac);
    fprintf(stderr, "[settings] reset everything\n");
    confirmReset = NO; [self rebuildKeepingPlace];
}
- (void)cancelReset { confirmReset = NO; [self refreshHint]; }

- (void)keyDown:(NSEvent *)e {
    BOOL shift = (e.modifierFlags & NSEventModifierFlagShift) != 0;
    BOOL cmdKey = (e.modifierFlags & NSEventModifierFlagCommand) != 0;
    if (!confirmReset && (e.keyCode == 44 || (e.keyCode == 3 && cmdKey)) && (onBar || (self.kind != MENU_SPAWN && self.kind != MENU_MODS))) { [self focusFind]; return; }
    if (findQuery.length && e.keyCode == 53) { find.stringValue = @""; [self searched:find]; return; }   /* Esc leaves the results first */
    if (!confirmReset && !onBar && self.kind == MENU_MODS && e.keyCode != 53 && [mods key:e]) return;
    if (!confirmReset && !onBar && self.kind == MENU_SPAWN && [self spawnKey:e]) return;
    switch (e.keyCode) {
    case 126: [self moveFocus:-1]; return;   /* up */
    case 125: [self moveFocus:1]; return;    /* down */
    case 123: [self step:-1]; return;        /* left */
    case 124: [self step:1]; return;         /* right */
    case 116: [self pageFocus:-1]; return;   /* Page Up */
    case 121: [self pageFocus:1]; return;    /* Page Down */
    case 36: case 76: case 49: if (!e.isARepeat) [self step:0]; return;   /* Return, Enter, Space */
    case 12: if (!confirmReset) [self movePage:-1]; return;     /* Q */
    case 14: case 48: if (!confirmReset) [self movePage:shift ? -1 : 1]; return;   /* E, Tab */
    case 15: if (!e.isARepeat && !confirmReset) { if (shift) [self resetEverything]; else [self resetThisTab]; } return;   /* R */
    case 53: if (!e.isARepeat) [self back]; return;   /* Escape: one level up */
    case 122: if (!e.isARepeat) [self hide]; return;  /* F1 */
    default: return;   /* every other key stays with the menu, away from the game */
    }
}
- (void)keyUp:(NSEvent *)e { (void)e; }
- (void)flagsChanged:(NSEvent *)e { (void)e; }

/* ---- controller ---- */

/* The face buttons are the game's (2026-10-28): A, B, X and Y are the buttons the game reads as the Switch's A, B, X and
   Y under the controller type (gamepad_layout.h gamepad_face_bits, as gamepad.m maps them; layout-file remaps aside), so
   on an Xbox controller A is the right button and B the bottom one, as in the game, not GameController's names. */
- (void)poll {
    uint64_t now = 0; int type = runtime_gamepad_type(NULL);
    for (GCController *c in GCController.controllers) {
        GCExtendedGamepad *p = c.extendedGamepad; if (!p) continue;
        if (p.dpad.up.isPressed) now |= 1; if (p.dpad.down.isPressed) now |= 2;
        if (p.dpad.left.isPressed) now |= 4; if (p.dpad.right.isPressed) now |= 8;
        uint64_t face = gamepad_face_bits(p.buttonA.isPressed, p.buttonB.isPressed, p.buttonX.isPressed, p.buttonY.isPressed,
                                          gamepad_type_face_by_position(type, c.productCategory.UTF8String, c.vendorName.UTF8String));
        if (face & GAMEPAD_SWITCH_A) now |= 16; if (face & GAMEPAD_SWITCH_B) now |= 32;
        if (p.leftShoulder.isPressed) now |= 64; if (p.rightShoulder.isPressed) now |= 128;
        if (p.buttonMenu.isPressed) now |= 256;
        if (face & GAMEPAD_SWITCH_X) now |= 512; if (face & GAMEPAD_SWITCH_Y) now |= 1024;
        if (p.leftTrigger.value > 0.5f) now |= 2048; if (p.rightTrigger.value > 0.5f) now |= 4096;
        /* the sticks are left to the game (runtime_input_hold): the menu navigates with the d-pad only */
    }
    uint64_t edge = now & ~padPrev; padPrev = now;
    double t = CACurrentMediaTime();
    /* held direction or trigger, repeated */
    int dir = now & 1 ? 1 : now & 2 ? 2 : now & 4 ? 3 : now & 8 ? 4 : now & 2048 ? 12 : now & 4096 ? 13 : 0;
    uint64_t bit = dir ? 1ull << (dir - 1) : 0, repeating = 15 | 2048 | 4096;
    if (!dir) padDir = 0;
    else if (dir != padDir) { padDir = dir; padRepeatAt = t + 0.35; edge |= bit; }
    else if (t >= padRepeatAt) { padRepeatAt = t + 0.09; edge |= bit; }
    else edge &= ~repeating;
    edge &= ~(repeating & ~bit);   /* only the direction that repeats */
    if (edge & 1) [self moveFocus:-1];
    if (edge & 2) [self moveFocus:1];
    if (edge & 4) [self step:-1];
    if (edge & 8) [self step:1];
    if (edge & 2048) [self pageFocus:-1];
    if (edge & 4096) [self pageFocus:1];
    if (edge & 16) [self step:0];
    if (!confirmReset && (edge & 64)) [self movePage:-1];
    if (!confirmReset && (edge & 128)) [self movePage:1];
    if (onBar && !confirmReset && (edge & 512)) [self resetThisTab];   /* X / Y on the section bar: the resets */
    else if (onBar && !confirmReset && (edge & 1024)) [self resetEverything];
    else if (!onBar && self.kind == MENU_MODS && (edge & 512)) [mods moveMod:-1];   /* X / Y: the mod's place in the load order */
    else if (!onBar && self.kind == MENU_MODS && (edge & 1024)) [mods moveMod:1];
    else if (!onBar && self.kind == MENU_SPAWN && (edge & (512 | 1024))) [self spawnButton:(edge & 1024) ? 'Y' : 'X'];
    if (edge & 32) [self back];
    if (edge & 256) [self hide];
}

/* ---- showing ---- */

- (void)place {
    NSView *host = settings_window().contentView;
    int kind = self.kind;
    CGFloat wide = kind == MENU_MODS ? MODS_PANEL_WIDTH : kind == MENU_SPAWN ? SPAWN_PANEL_WIDTH : PANEL_WIDTH;
    CGFloat width = MIN(wide, MAX(PANEL_WIDTH, host.bounds.size.width - 2 * PANEL_MARGIN));
    self.frame = NSMakeRect(PANEL_MARGIN, PANEL_MARGIN, width, MAX(320, host.bounds.size.height - 2 * PANEL_MARGIN));
    self.autoresizingMask = NSViewHeightSizable | NSViewMaxXMargin;
}

- (void)show {
    NSWindow *w = settings_window(); if (!w || self.superview) return;
    confirmReset = NO;
    [self place]; [self buildPage];
    [w.contentView addSubview:self positioned:NSWindowAbove relativeTo:nil];
    [w makeFirstResponder:self];
    runtime_input_hold(1);
    padPrev = ~0ull;   /* the buttons that opened the menu do nothing until released */
    pollTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 target:self selector:@selector(poll) userInfo:nil repeats:YES];
    [meterTimer invalidate]; meterTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self selector:@selector(meterTick) userInfo:nil repeats:YES];
    fprintf(stderr, "[settings] menu open\n");
}

- (void)hide {
    if (!self.superview) return;
    onBar = YES; confirmReset = NO;   /* the next open starts at the top level */
    [meterTimer invalidate]; meterTimer = nil;
    [pollTimer invalidate]; pollTimer = nil;
    NSWindow *w = self.window;
    [self removeFromSuperview];
    [w makeFirstResponder:w.contentView];
    runtime_input_hold(0);
    fprintf(stderr, "[settings] menu closed\n");
}

+ (BOOL)snapshotTo:(NSString *)path {
    TOTKSettingsPanel *p = [self shared];
    BOOL shown = p.superview != nil;
    if (!shown) { [p place]; [p buildPage]; }
    [p layoutSubtreeIfNeeded];
    NSBitmapImageRep *rep = [p bitmapImageRepForCachingDisplayInRect:p.bounds];
    if (!rep) return NO;
    __block BOOL ok = NO;
    [p.appearance performAsCurrentDrawingAppearance:^{
        [p cacheDisplayInRect:p.bounds toBitmapImageRep:rep];
        NSBitmapImageRep *out = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:rep.pixelsWide pixelsHigh:rep.pixelsHigh
            bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
        out.size = p.bounds.size;
        [NSGraphicsContext saveGraphicsState];
        NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:out];
        [[NSColor colorWithCalibratedRed:0.07 green:0.08 blue:0.09 alpha:1] setFill]; NSRectFill(p.bounds);   /* the layer's own background */
        [rep drawInRect:p.bounds fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];
        [NSGraphicsContext restoreGraphicsState];
        ok = [[out representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
    }];
    return ok;
}
@end
