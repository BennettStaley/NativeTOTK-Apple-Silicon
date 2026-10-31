// The launcher inside the packaged app (build_app.py compiles it as Contents/MacOS/TOTK).
// The game itself is Contents/Resources/totk/build/totk-native; everything it writes goes to the player's own folders:
//   ~/Library/Application Support/MacNativeGames/TOTK/   profile (account, saves), shader cache, game slot, RomFS proof
//                                                       and game-data/: the RomFS image and prepared shaders the app
//                                                       points at (game-data.json; read-only for the game). BotW/ for
//                                                       Breath of the Wild.
//   ~/Library/Logs/MacNativeGames/totk.log             this run, only with the debug log on (totk.previous.log: the one before; botw.log for Breath of the Wild)
// If the game is already running, the launcher only brings it to the front.
#import <AppKit/AppKit.h>
#include <sys/sysctl.h>
#import <GameController/GameController.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/proc.h>
#include <unistd.h>
#include "dmsh_chain.h"   // DMSH_CHAIN: the shader translation chain the cache folder is named after
#include "game_version.h"
#include "controller_map.h"
#include "settings_schema.h"
#include "totk_shader_contrib.h"   // the shared shader list contribution and its reminder
#import "save_package.h"           // .totksave: saves sent from another Mac or an iPhone / iPad
#include "contrib_submit.h"        // its "Submit automatically" (gh): run_tool, gh_path, contrib_submit; shared with the settings panel

static void show_error(NSString *detail) {
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"The game could not start";
    alert.informativeText = detail;
    [alert runModal];
}

/* Whether a game controller is connected: a HID joystick / gamepad (DualSense, Pro Controller, Xbox over Bluetooth),
   or one the GameController framework adopts (Xbox over USB), which it reports a moment after the first run loop turns. */
static BOOL controller_connected(void) {
    IOHIDManagerRef hid = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    NSMutableArray *matching = [NSMutableArray array];
    for (NSNumber *usage in @[@(kHIDUsage_GD_Joystick), @(kHIDUsage_GD_GamePad), @(kHIDUsage_GD_MultiAxisController)])
        [matching addObject:@{@kIOHIDDeviceUsagePageKey: @(kHIDPage_GenericDesktop), @kIOHIDDeviceUsageKey: usage}];
    IOHIDManagerSetDeviceMatchingMultiple(hid, (__bridge CFArrayRef)matching);
    CFSetRef devices = IOHIDManagerCopyDevices(hid);
    BOOL found = devices && CFSetGetCount(devices) > 0;
    if (devices) CFRelease(devices);
    CFRelease(hid);
    for (int i = 0; !found && i < 10; i++) {
        for (GCController *controller in GCController.controllers)
            if (controller.extendedGamepad) found = YES;
        if (!found) [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    return found;
}

static NSDictionary<NSString *, NSString *> *kv_read(NSString *path) {
    NSMutableDictionary *out = [NSMutableDictionary dictionary];
    NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        NSRange eq = [line rangeOfString:@"="];
        if (eq.location == NSNotFound) continue;
        NSString *key = [line substringToIndex:eq.location];
        if (!out[key]) out[key] = [line substringFromIndex:eq.location + 1];
    }
    return out;
}

static uint64_t pid_start(pid_t pid) {   // 0 if gone or a zombie; with the pid it names one process exactly
    struct proc_bsdinfo bi;
    if (pid <= 0 || proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != (int)sizeof bi || bi.pbi_status == SZOMB) return 0;
    return bi.pbi_start_tvsec;
}

/* The game slot (runtime/game_slot.c): the running game holds an exclusive lock on <slot>/lock and names itself in
   <slot>/holder. Returns the running game's pid, or 0. */
static pid_t running_game(NSString *slot) {
    int fd = open([slot stringByAppendingPathComponent:@"lock"].fileSystemRepresentation, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int held = flock(fd, LOCK_EX | LOCK_NB) != 0;
    close(fd);
    if (!held) return 0;
    NSDictionary *h = kv_read([slot stringByAppendingPathComponent:@"holder"]);
    /* A player launch must reach runtime/game_slot.c to preempt an agent's
       test. Only an existing player game (or another test for a test request)
       is brought forward here. */
    const char *requestOwner = getenv("TOTK_SLOT_OWNER");
    if (![h[@"owner"] isEqualToString:@"user"] &&
        (!requestOwner || !*requestOwner || !strcmp(requestOwner, "user"))) return 0;
    pid_t pid = [h[@"game_pid"] intValue];
    return pid > 0 && pid_start(pid) == (uint64_t)[h[@"game_start"] longLongValue] ? pid : 0;
}

static void note(NSString *text) {
    NSAlert *alert = [NSAlert new];
    alert.messageText = text;
    [alert runModal];
}

/* Every TOTK_CONTRIB_STEP shaders and render pipelines the shared list does not have yet (totk_shader_contrib.h), before the game starts: ask
   the player to send the file in a GitHub issue. "Send" opens the issue form and shows the file in Finder to drag in.
   With gh signed in, "Submit automatically" takes the place of "Remind me next time" and sends this file at once.
   The star button only opens the project page (the player stars it there) and asks again next time. */
static void contrib_reminder(NSString *path) {
    NSUInteger rows = totk_contrib_due(path);
    if (!rows) return;
    totk_contrib_reminded(path, rows);   // "next time": TOTK_CONTRIB_STEP rows later; "don't remind me again": never
    NSString *gh = gh_path();
    [NSApplication sharedApplication]; [NSApp activateIgnoringOtherApps:YES];
    [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:path]]];   // the folder, before the question
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"We would love your contribution";
    alert.informativeText = [NSString stringWithFormat:@"%@%@", [NSString stringWithFormat:@"You have %lu shaders and render pipelines to contribute: ones your game met that the shared shader list does not have yet. They are kept in a small file with shader and pipeline identifiers only: no game data, saves or personal information. Sent to GitHub and added to the shared list, they come with everyone's next build, and nobody stutters there again.\n\n", (unsigned long)rows],
                             gh ? @"\"Submit automatically\" sends it now with the GitHub CLI (gh) signed in on this Mac, without the browser. \"Send\" opens the GitHub form and shows the file in Finder: drag it into the form."
                                : @"\"Send\" opens the GitHub form and shows the file in Finder: drag it into the form."];
    [alert addButtonWithTitle:@"Send"];
    [alert addButtonWithTitle:gh ? @"Submit automatically" : @"Remind me next time"];
    [alert addButtonWithTitle:@"Don't remind me again"];
    [alert addButtonWithTitle:@"⭐ Like it? Star it on GitHub"];
    NSModalResponse choice = [alert runModal];
    if (choice == NSAlertThirdButtonReturn + 1) [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@TOTK_REPO_URL]];
    if (choice == NSAlertThirdButtonReturn) totk_contrib_never(path);
    if (choice == NSAlertSecondButtonReturn && gh) {
        NSString *issue = contrib_submit(gh, path, rows);
        if (issue) { note([NSString stringWithFormat:@"%@\n%@", @"Thank you for your shader contribution! Let's make the game smoother together!", issue]); return; }
        note(@"Submitting automatically did not work; opening the form instead.");
        choice = NSAlertFirstButtonReturn;
    }
    if (choice != NSAlertFirstButtonReturn) return;
    [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@TOTK_CONTRIB_ISSUE_URL]];
    [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:path]]];
}

/* The game data outside the app (build_app.py's default Mac layout, docs/how-it-works.md "Where the game data lives"):
   Contents/Resources/totk/game-data.json names two content-addressed folders in
   ~/Library/Application Support/MacNativeGames/TOTK/game-data: <version>-romfs-<id> (romfs.bin, runtime-index.bin) and
   <version>-shaders-<id> (shader-seed/, shader-libs/). Each folder's store.json carries the same id, and the files the
   pointer lists must have their sizes; the game itself still checks the RomFS's SHA-256 (romfs.c: once per file
   identity, the record kept in that folder) and every prepared shader against the seed's manifest. An app built with
   --embed-data has no pointer and carries the data inside, as before. Returns nil when the data is there (and with
   apply, points the game at it), else what is wrong for the error dialog. */
static NSDictionary *json_read(NSString *path) {
    NSData *raw = [NSData dataWithContentsOfFile:path];
    id value = raw ? [NSJSONSerialization JSONObjectWithData:raw options:0 error:nil] : nil;
    return [value isKindOfClass:NSDictionary.class] ? value : nil;
}
static NSString *game_data(NSString *root, BOOL apply) {
    NSString *pointerPath = [root stringByAppendingPathComponent:@"game-data.json"];
    if (![NSFileManager.defaultManager fileExistsAtPath:pointerPath]) return nil;   // self-contained app (--embed-data)
    NSDictionary *pointer = json_read(pointerPath);
    NSString *rebuild = [pointer[@"rebuild"] isKindOfClass:NSString.class] ? pointer[@"rebuild"]
        : @"python3.14 build_app.py <game> -k <prod.keys> [--update <update>] -o <this app>";
    if (!pointer || [pointer[@"format"] intValue] != 1)
        return [NSString stringWithFormat:@"%@ is unreadable. Build the app again:\n\n%@", pointerPath, rebuild];
    NSMutableDictionary<NSString *, NSString *> *dirs = [NSMutableDictionary dictionary];
    for (NSString *part in @[@"romfs", @"shaders"]) {
        NSDictionary *want = pointer[part];
        NSString *dir = [want isKindOfClass:NSDictionary.class] && [want[@"path"] isKindOfClass:NSString.class] ? want[@"path"] : nil;
        NSString *problem = nil;
        if (!dir) problem = @"the app's game-data.json names no folder";
        else {
            NSDictionary *have = json_read([dir stringByAppendingPathComponent:@"store.json"]);
            if (!have) problem = @"the folder is missing (deleted, or the app came from another Mac)";
            else if (![have[@"id"] isEqual:want[@"id"]]) problem = @"the folder holds other data than this app was built with";
            NSDictionary *sizes = [want[@"sizes"] isKindOfClass:NSDictionary.class] ? want[@"sizes"] : @{};
            for (NSString *name in sizes) {
                if (problem) break;
                NSDictionary *a = [NSFileManager.defaultManager attributesOfItemAtPath:[dir stringByAppendingPathComponent:name] error:nil];
                if (!a || a.fileSize != [sizes[name] unsignedLongLongValue])
                    problem = [NSString stringWithFormat:@"%@ is missing or has another size", name];
            }
        }
        if (problem)
            return [NSString stringWithFormat:@"The game data this app uses is not there: %@.\n\n%@\n\nBuild the app again; it writes the data "
                    @"there (in a minute or two when the build's work folder still has it):\n\n%@", problem, dir ?: pointerPath, rebuild];
        dirs[part] = dir;
    }
    if (apply) {
        setenv("TOTK_ROMFS", dirs[@"romfs"].fileSystemRepresentation, 1);
        setenv("TOTK_ROMFS_PROOF_DIR", dirs[@"romfs"].fileSystemRepresentation, 1);   // the verification record goes with the image
        setenv("TOTK_SHADER_SEED_DIR", [dirs[@"shaders"] stringByAppendingPathComponent:@"shader-seed"].fileSystemRepresentation, 1);
        setenv("TOTK_SHADER_LIBS", [dirs[@"shaders"] stringByAppendingPathComponent:@"shader-libs"].fileSystemRepresentation, 1);
    }
    return nil;
}

/* Files opened with the app (a .totksave double-clicked in Downloads, where AirDrop puts it): Launch Services hands them
   over as an Apple event once the app runs, before applicationDidFinishLaunching, so the launcher runs the event loop
   just that long (2 s at most). A path given on the command line counts too. */
@interface TOTKLaunchDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic, strong) NSMutableArray<NSURL *> *opened;
@end
@implementation TOTKLaunchDelegate
- (void)application:(NSApplication *)app openURLs:(NSArray<NSURL *> *)urls { (void)app; [self.opened addObjectsFromArray:urls]; }
- (void)applicationDidFinishLaunching:(NSNotification *)note { (void)note; [self stop]; }
- (void)stop {
    if (!NSApp.isRunning) return;
    [NSApp stop:nil];
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0
                                     windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:YES];
}
@end
static NSArray<NSString *> *opened_packages(int argc, char **argv) {
    TOTKLaunchDelegate *delegate = [TOTKLaunchDelegate new]; delegate.opened = [NSMutableArray array];
    for (int i = 1; i < argc; i++) if (argv[i][0] != '-') [delegate.opened addObject:[NSURL fileURLWithPath:@(argv[i])]];
    [NSApplication sharedApplication]; NSApp.delegate = delegate;
    [delegate performSelector:@selector(stop) withObject:nil afterDelay:2.0];
    [NSApp run];
    [NSObject cancelPreviousPerformRequestsWithTarget:delegate];
    NSApp.delegate = nil;
    NSMutableArray *out = [NSMutableArray array];
    for (NSURL *url in delegate.opened) if (url.isFileURL && [url.pathExtension.lowercaseString isEqual:TOTKSavePackageExtension]) [out addObject:url.path];
    return out;
}

/* A save package opened with the app: checked (runtime/save_package.h: same game version, every file's SHA-256), shown,
   and on the player's yes put into this Mac's profile, the current saves copied to <data>/save-backups first. The game is
   not running here (the launcher checked), so nothing can save over it. Returns NO when the player chose Quit. */
static BOOL import_package(NSString *package, NSString *profile, NSString *backups) {
    [NSApp activateIgnoringOtherApps:YES];
    NSError *error = nil;
    TOTKSavePackageInfo *info = TOTKSaveInspect(package, @TOTK_GAME_VERSION, &error);
    NSAlert *alert = [NSAlert new];
    if (info) {
        alert.messageText = [NSString stringWithFormat:@"Use the saves from %@?", info.fromText];
        alert.informativeText = [NSString stringWithFormat:@"%@\nSent %@ (game version %@).\n\n%@\n\nThey replace the saves on this Mac. "
                                 @"The current saves are copied to %@ first (the last %d copies are kept).",
                                 package.lastPathComponent, info.created ? [NSDateFormatter localizedStringFromDate:info.created
                                 dateStyle:NSDateFormatterMediumStyle timeStyle:NSDateFormatterShortStyle] : @"at an unknown time",
                                 info.gameVersion, info.slotsText, backups, TOTK_SAVE_BACKUPS_KEPT];
        [alert addButtonWithTitle:@"Replace Saves and Play"];
        [alert addButtonWithTitle:@"Play Without Them"];
        [alert addButtonWithTitle:@"Quit"];
        NSModalResponse choice = [alert runModal];
        if (choice == NSAlertThirdButtonReturn) return NO;
        if (choice == NSAlertSecondButtonReturn) return YES;
        info = TOTKSaveImport(package, profile, backups, @TOTK_GAME_VERSION, &error);
        fprintf(stderr, "[launcher] save import %s: %s\n", package.fileSystemRepresentation,
                info ? "done" : error.localizedDescription.UTF8String);
        if (info) {
            NSAlert *done = [NSAlert new];
            done.messageText = [NSString stringWithFormat:@"Saves from %@ are on this Mac now", info.fromText];
            done.informativeText = [NSString stringWithFormat:@"%@\n\n%@ Load one from the title screen.", info.slotsText,
                                    info.backup ? [NSString stringWithFormat:@"The saves they replaced are in %@.", info.backup] : @""];
            [done addButtonWithTitle:@"Play"]; [done addButtonWithTitle:@"Quit"];
            return [done runModal] == NSAlertFirstButtonReturn;
        }
        alert = [NSAlert new];
    }
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = @"These saves can't be used";
    alert.informativeText = [NSString stringWithFormat:@"%@\n\n%@\n\nThe saves on this Mac are unchanged.", package.lastPathComponent,
                             error.localizedDescription ?: @"Unknown error."];
    [alert addButtonWithTitle:@"Play"]; [alert addButtonWithTitle:@"Quit"];
    return [alert runModal] == NSAlertFirstButtonReturn;
}

/* The contribution file: in the project folder the app was built from, <clone>/shader-list-contribution/shader-list-contribution.txt
   (MNGContributionFolder in Info.plist, build_app.py), where players look and where the iPhone / iPad copies go too; the
   data folder when that folder is gone (user 2026-10-22: nobody found the file in ~/Library). The file that was in the data
   folder moves there once, with its reminder state. */
static NSString *contrib_file(NSString *data) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *old = [data stringByAppendingPathComponent:@"shader-list-contribution.txt"];
    NSString *folder = [NSBundle.mainBundle objectForInfoDictionaryKey:@"MNGContributionFolder"];
    if (![folder isKindOfClass:NSString.class] || !folder.length || ![fm fileExistsAtPath:folder.stringByDeletingLastPathComponent]) return old;
    if (![fm createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil]) return old;
    // one file for the Mac and every iPhone / iPad (ios/tools/shader_contrib_watch.py merges theirs in): the rows are the same
    // for every platform; Breath of the Wild has a list of its own
    NSString *mine = [folder stringByAppendingPathComponent:strcmp(GAME_FOLDER, "TOTK") ? @"shader-list-contribution-botw.txt" : @"shader-list-contribution.txt"];
    if (![fm fileExistsAtPath:mine] && [fm fileExistsAtPath:old]) {
        [fm moveItemAtPath:old toPath:mine error:nil];
        NSString *state = [old stringByAppendingString:@".reminded"];   // where the state was kept before (totk_contrib_state)
        if ([fm fileExistsAtPath:state]) [fm moveItemAtPath:state toPath:totk_contrib_state(mine) error:nil];
    }
    return mine;
}

static BOOL make_dir(NSString *path) {
    return [NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil];
}

/* build_app.py --mac-game-data library: the app carries no RomFS image. A folder holds it when its romfs.bin (in it, or in
   <folder>/<version>) has the size the build pinned (romfs-config.txt) and a romfs.sha256 beside it, if any, names the
   pinned image: the build copies to romfs.bin.partial first, so a romfs.bin of the full size is a complete copy. Its hash
   is checked by the runtime at the first start (TOTK_ROMFS_PROOF_DIR). Returns the folder of the image, or nil. */
static NSString *game_data_in(NSString *folder, unsigned long long size, NSString *sha) {
    if (!folder.length) return nil;
    for (NSString *place in @[[folder stringByAppendingPathComponent:@TOTK_GAME_DIR], folder]) {
        NSDictionary *image = [NSFileManager.defaultManager attributesOfItemAtPath:[place stringByAppendingPathComponent:@"romfs.bin"] error:nil];
        if (!image || image.fileSize != size) continue;
        NSString *mark = [NSString stringWithContentsOfFile:[place stringByAppendingPathComponent:@"romfs.sha256"] encoding:NSUTF8StringEncoding error:nil];
        mark = [mark stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (!mark.length || [mark caseInsensitiveCompare:sha] == NSOrderedSame) return place;
    }
    return nil;
}

/* No folder holds the image (the app copied to another Mac, or its disk not connected): ask where it is, and keep the
   answer for the next starts (defaults MNGGameDataFolder.<version>). The image is read where it is, never moved or
   copied. Returns its folder, or nil to quit. */
static NSString *game_data_ask(NSArray<NSString *> *looked, unsigned long long size, NSString *sha) {
    [NSApplication sharedApplication]; [NSApp activateIgnoringOtherApps:YES];
    for (;;) {
        NSAlert *alert = [NSAlert new];
        alert.messageText = @"Where is the game data?";
        alert.informativeText = [NSString stringWithFormat:@"This app keeps its game data (romfs.bin, %.1f GB) outside the app, and it is not in %@. If it is on a disk that is not connected, connect it and choose Try Again; otherwise choose the folder it is in. The game reads it where it is, without moving or copying it.",
                                     size / 1e9, [looked componentsJoinedByString:@", "]];
        [alert addButtonWithTitle:@"Choose Folder…"];
        [alert addButtonWithTitle:@"Try Again"];
        [alert addButtonWithTitle:@"Quit"];
        NSModalResponse choice = [alert runModal];
        if (choice == NSAlertThirdButtonReturn) return nil;
        if (choice == NSAlertSecondButtonReturn) {
            for (NSString *folder in looked) { NSString *found = game_data_in(folder, size, sha); if (found) return found; }
            continue;
        }
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = NO; panel.canChooseDirectories = YES; panel.allowsMultipleSelection = NO;
        panel.message = @("Choose the folder that holds romfs.bin (or the folder that holds its " TOTK_GAME_DIR " folder).");
        panel.prompt = @"Choose";
        if ([panel runModal] != NSModalResponseOK || !panel.URL) continue;
        NSString *found = game_data_in(panel.URL.path, size, sha);
        if (found) {
            [NSUserDefaults.standardUserDefaults setObject:found forKey:@"MNGGameDataFolder." TOTK_GAME_DIR];
            return found;
        }
        note([NSString stringWithFormat:@"%@ holds no romfs.bin of this version (%llu bytes).",
              panel.URL.path, size]);
    }
}

int main(int argc, char **argv) {
    @autoreleasepool {
        NSString *root = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"totk"];
        NSString *binary = [root stringByAppendingPathComponent:@"build/totk-native"];
        /* --check (build_app.py, once after packaging): only look that the game is there and exit. The first run of a new
           app is when macOS scans the whole bundle (minutes for 16 GB, with nothing on screen); doing it during the build
           makes the player's first start immediate. */
        if (argc > 1 && !strcmp(argv[1], "--check")) {
            int ok = access(binary.fileSystemRepresentation, X_OK) == 0;
            NSString *declared = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
            ok = ok && [declared isEqualToString:@TOTK_GAME_VERSION];
            NSString *data = ok ? game_data(root, NO) : nil;
            printf("%s: game version %s\n", !ok ? "missing binary or version mismatch" : data ? data.UTF8String : "ok", TOTK_GAME_VERSION);
            return ok && !data ? 0 : 1;
        }
        if (access(binary.fileSystemRepresentation, X_OK) != 0) {
            show_error(@"The game files are missing from this app. Build it again with build_app.py.");
            return 1;
        }
        NSString *missing = game_data(root, NO);
        if (missing) { show_error(missing); return 1; }
        NSString *home = NSHomeDirectory();
        NSString *data = [home stringByAppendingPathComponent:@"Library/Application Support/MacNativeGames/" GAME_FOLDER];   /* game_version.h: TOTK, BotW */
        NSString *logs = [home stringByAppendingPathComponent:@"Library/Logs/MacNativeGames"];
        { const char *v = getenv("TOTK_LOG_DIR"); if (v && *v) logs = @(v); }   // a test's own log folder
        /* Folders the environment already names are kept (open --env TOTK_PROFILE_DIR=... runs a test on a save copy,
           TOTK_GAME_SLOT_DIR shares one game slot with another build of the game: one game at a time on the Mac). */
        NSString *(^env_or)(const char *, NSString *) = ^NSString *(const char *name, NSString *fallback) {
            const char *v = getenv(name); return v && *v ? @(v) : fallback; };
        BOOL test_profile = getenv("TOTK_PROFILE_DIR") && *getenv("TOTK_PROFILE_DIR");   // a test on a save copy may autoload it
        NSString *profile = env_or("TOTK_PROFILE_DIR", [data stringByAppendingPathComponent:@"profile"]);
        NSString *slot = env_or("TOTK_GAME_SLOT_DIR", [data stringByAppendingPathComponent:@"game-slot"]);
        NSString *cache = env_or("TOTK_SHADER_CACHE", [data stringByAppendingPathComponent:[NSString stringWithFormat:@"shader-cache/rt-%s", DMSH_CHAIN]]);
        if (!make_dir(profile) || !make_dir(slot) || !make_dir(cache) || !make_dir(logs)) {
            show_error([NSString stringWithFormat:@"Cannot create %@.", data]);
            return 1;
        }
        NSArray<NSString *> *packages = opened_packages(argc, argv);
        pid_t running = running_game(slot);
        if (running) {
            if (packages.count) {   /* the running game holds its save in memory and would save over an import */
                [NSApp activateIgnoringOtherApps:YES];
                NSAlert *alert = [NSAlert new];
                alert.messageText = @"Quit the game to use these saves";
                alert.informativeText = [NSString stringWithFormat:@"The game is running, and it would save over saves brought in now. "
                                         @"Save in the game if you need to, quit it (⌘Q), then open %@ again.", packages.firstObject.lastPathComponent];
                [alert runModal];
            }
            [[NSRunningApplication runningApplicationWithProcessIdentifier:running] activateWithOptions:NSApplicationActivateAllWindows];
            return 0;
        }
        for (NSString *package in packages)
            if (!import_package(package, profile, [data stringByAppendingPathComponent:@"save-backups"])) return 0;
#if defined(TOTK_GAME_V112) || defined(TOTK_GAME_V121) || defined(TOTK_GAME_V143)
        NSString *backup = [profile stringByAppendingFormat:@"-before-%s", TOTK_GAME_VERSION];
        NSFileManager *fm = NSFileManager.defaultManager;
        if (![fm fileExistsAtPath:backup]) {
            NSString *temporary = [backup stringByAppendingFormat:@".copy-%d", getpid()];
            NSError *error = nil;
            if (![fm copyItemAtPath:profile toPath:temporary error:&error] ||
                ![fm moveItemAtPath:temporary toPath:backup error:&error]) {
                [fm removeItemAtPath:temporary error:nil];
                show_error([@"Cannot back up saves before upgrading: " stringByAppendingString:error.localizedDescription ?: @"unknown error"]);
                return 1;
            }
        }
#endif
        if (chdir(root.fileSystemRepresentation) != 0) {   // the game reads its data relative to this folder
            show_error(@"Cannot open the game folder inside the app.");
            return 1;
        }
        /* The player's controller layout (runtime/controller_map.h), checked here: a bad line shows this alert instead of
           the game closing at start. */
        NSString *controls = env_or("TOTK_CONTROLS", [data stringByAppendingPathComponent:@"controls.txt"]);
        NSData *layout = [NSData dataWithContentsOfFile:controls];
        if (layout) {
            NSString *text = layout.length < 16384 ? [[NSString alloc] initWithData:layout encoding:NSUTF8StringEncoding] : nil;
            uint16_t map[CONTROLLER_BUTTONS]; uint8_t modes[CONTROLLER_BUTTONS]; controller_map_default(map, modes);
            const char *problem = layout.length < 16384 ? "not UTF-8 text" : "larger than 16 KB";
            int line = text ? controller_map_parse(text.UTF8String, map, modes, &problem) : 1;
            if (line) { show_error([NSString stringWithFormat:@"%@, line %d: %s", controls, line, problem]); return 1; }
        }
        /* The player's settings (runtime/settings_schema.h): what the game reads at start goes to it as its variables; the
           game reloads the file for the settings menu (TOTK_SETTINGS). A variable already set wins. */
        { NSString *settings = env_or("TOTK_SETTINGS", [data stringByAppendingPathComponent:@"settings.txt"]);
          int values[SETTINGS_COUNT]; settings_defaults(values, 0);
          NSData *text = [NSData dataWithContentsOfFile:settings];
          if (text.length && text.length < 16384) settings_parse_text([[NSString alloc] initWithData:text encoding:NSUTF8StringEncoding].UTF8String, values);
          settings_take_env(values); settings_export_env(values);
          setenv("TOTK_SETTINGS", settings.fileSystemRepresentation, 1); }
        setenv("TOTK_CONTROLS", controls.fileSystemRepresentation, 1);
        /* key 3's picture mode, kept between runs (runtime/game_patches_v143.c picture_mode_save / _restore) */
        setenv("TOTK_PICTURE_MODE", env_or("TOTK_PICTURE_MODE", [data stringByAppendingPathComponent:@"picture-mode.txt"]).fileSystemRepresentation, 1);
        setenv("TOTK_PROFILE_DIR", profile.fileSystemRepresentation, 1);
        setenv("TOTK_SHADER_CACHE", cache.fileSystemRepresentation, 1);
        setenv("TOTK_GAME_SLOT_DIR", slot.fileSystemRepresentation, 1);
        setenv("TOTK_ROMFS_PROOF_DIR", data.fileSystemRepresentation, 1);   // the app bundle stays read-only
        BOOL stored = [NSFileManager.defaultManager fileExistsAtPath:[root stringByAppendingPathComponent:@"game-data.json"]];
        game_data(root, YES);   // the game data outside the app: its folders (and the RomFS record next to the image)
        /* build_app.py --mac-game-data library: the app carries no RomFS image, so a new build is a few hundred MB and
           copies to another Mac without it. It is looked for in the folder chosen at an earlier start (game_data_ask), the
           folder the build put it in (Info.plist MNGGameDataFolder, e.g. on another disk), and <data>/GameData; with none
           holding it, the player is asked. An app with its own image uses that. */
        NSFileManager *files = NSFileManager.defaultManager;
        if (!stored && ![files fileExistsAtPath:@"extracted/" TOTK_GAME_DIR "/romfs/romfs.bin"]) {
            NSString *pins = [NSString stringWithContentsOfFile:@"build/game-data/" TOTK_GAME_DIR "/romfs-config.txt" encoding:NSUTF8StringEncoding error:nil];
            unsigned long long size = 0; NSString *sha = @"";
            for (NSString *line in [pins componentsSeparatedByString:@"\n"]) {
                NSArray<NSString *> *kv = [line componentsSeparatedByString:@" "];
                if (kv.count != 2) continue;
                if ([kv[0] isEqualToString:@"ROMFS_IMAGE_SIZE"]) size = strtoull(kv[1].UTF8String, NULL, 10);
                if ([kv[0] isEqualToString:@"ROMFS_IMAGE_SHA256"]) sha = kv[1];
            }
            if (!size) { show_error(@"The game files are missing from this app. Build it again with build_app.py."); return 1; }
            NSMutableArray<NSString *> *places = [NSMutableArray array];
            for (id place in @[[NSUserDefaults.standardUserDefaults stringForKey:@"MNGGameDataFolder." TOTK_GAME_DIR] ?: @"",
                               [NSBundle.mainBundle objectForInfoDictionaryKey:@"MNGGameDataFolder"] ?: @"",
                               [data stringByAppendingPathComponent:@"GameData"]])
                if ([place isKindOfClass:NSString.class] && [place length] && ![places containsObject:place]) [places addObject:place];
            NSString *library = nil;
            for (NSString *place in places) if ((library = game_data_in(place, size, sha))) break;
            if (!library && !(library = game_data_ask(places, size, sha))) return 0;
            setenv("TOTK_ROMFS_DATA", library.fileSystemRepresentation, 1);   // runtime/romfs.c
        }
        setenv("TOTK_SHARED_FONTS", "extracted/firmware/shared-fonts", 1);
        /* Player mods (runtime/mods.h): one folder per mod in <data>/mods, mods.txt for the order; a TOTK_MODS_DIR already
           set wins (a test's own folder). The older single folder <data>/mods/<version> with its manifest.tsv still loads. */
        setenv("TOTK_MODS_DIR", [data stringByAppendingPathComponent:@"mods"].fileSystemRepresentation, 0);
        /* With no controller connected at start, <data>/mods/<version>-keyboard takes the older folder's place when it holds a
           manifest (keyboard key icons in the prompts: scripts/make_button_icons.py --style keyboard or azerty). The fonts
           are read once, so it holds for the session. */
        NSString *mods = [data stringByAppendingPathComponent:@"mods/" TOTK_GAME_DIR];
        NSString *keyboardMods = [mods stringByAppendingString:@"-keyboard"];
        if (!getenv("TOTK_ROMFS_MODS") && [NSFileManager.defaultManager fileExistsAtPath:[keyboardMods stringByAppendingPathComponent:@"manifest.tsv"]] &&
            !controller_connected())
            mods = keyboardMods;
        setenv("TOTK_ROMFS_MODS", mods.fileSystemRepresentation, 0);
        /* Cheats (runtime/cheat_files.h): <build ID>.txt files in <data>/cheats, read at every start; a test's own folder wins. */
        setenv("TOTK_CHEATS_DIR", [data stringByAppendingPathComponent:@"cheats"].fileSystemRepresentation, 0);
        /* The community cheat list the game downloads (runtime/cheat_db_fetch.h): <data>/cheats-db; a test's own folder wins. */
        setenv("TOTK_CHEATS_DB_DIR", [data stringByAppendingPathComponent:@"cheats-db"].fileSystemRepresentation, 0);
        /* The game's language (runtime/services.c): the app's first preferred language, which also follows a per-app
           language (defaults write org.macnativegames.totk AppleLanguages -array pt-BR). TOTK_LANG still overrides it. */
        NSString *preferred = NSLocale.preferredLanguages.firstObject;
        if (preferred) setenv("TOTK_LOCALE", preferred.UTF8String, 1);
        setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1);
        setenv("TOTK_GPU_WIDEN", "1", 1);
        setenv("TOTK_GPU_BLOCKRT", "1", 1);
        setenv("TOTK_GPU_MIDCLEAR", "1", 1);
        /* 5000 MB, more with the 4K maximum output (settings: display.headroom 4K, or Auto on 16 GB): a 4K canvas alone runs
           at 4.5-4.9 GB and a size switch holds old and new targets for a moment (stopped at 5023 MB, 2026-10-20). Never
           above the runtime's own default, half of the memory (40% until 2026-10-28). */
        { uint64_t mem = 0; size_t len = sizeof mem; sysctlbyname("hw.memsize", &mem, &len, NULL, 0);
          const char *out = getenv("TOTK_OUTPUT_RES");
          int big = out ? !strncmp(out, "3840", 4) : mem >= (16ULL << 30);
          unsigned budget = big ? 6500 : 5000, cap = (unsigned)((mem / 2) >> 20);
          /* A 3D world above 1440p (settings: Maximum render resolution): its texture heap passes 1 GiB (one zero-copy GPU
             buffer, which has no size limit since upstream b3bb2c08), and its targets need more room: the cap itself, at most 12000 MB (40% and 8000 MB
             until 2026-10-28: 7372 MB stopped a 2560x1600 world on the 18 GB M3 Pro twice, at 8.1 GB, after the game's
             own resolution change; the kernel panics happened at about 15 GB). */
          const char *world = getenv("TOTK_RENDER_RESERVE"); unsigned ww = 0, wh = 0;
          if (world && sscanf(world, "%ux%u", &ww, &wh) == 2 && (uint64_t)ww * wh > 2560u * 1440u) {
              budget = cap && cap < 12000 ? cap : 12000;
          }
          if (cap && budget > cap) budget = cap;
          setenv("TOTK_GPU_BUDGET_MB", [NSString stringWithFormat:@"%u", budget].UTF8String, 1); }
        setenv("TOTK_SLOT_OWNER", "user", 0);
        setenv("TOTK_SLOT_PURPOSE", "player", 0);
        /* The commit the app was built from (build_app.py MNGSourceCommit): the HUD box shows it (runtime/frontend.m). */
        NSString *commit = [NSBundle.mainBundle objectForInfoDictionaryKey:@"MNGSourceCommit"];
        if ([commit isKindOfClass:NSString.class] && commit.length) setenv("TOTK_SOURCE_COMMIT", commit.UTF8String, 1);
        // contrib_file only without a test's own file: it moves the player's file into the project folder
        const char *contribEnv = getenv("TOTK_SHADER_CONTRIB");
        NSString *contrib = contribEnv && *contribEnv ? env_or("TOTK_SHADER_CONTRIB", nil) : contrib_file(data);
        setenv("TOTK_SHADER_CONTRIB", contrib.fileSystemRepresentation, 1);
        /* The shared shader list the game downloads once a day and prepares in the background (third_party/d2r_gfx/gfx/nvn/
           totk_shader_list.m; Interface > Download new shaders). Download only; the game never uploads anything. */
        setenv("TOTK_SHADER_LIST_DIR", env_or("TOTK_SHADER_LIST_DIR", [data stringByAppendingPathComponent:@"shader-list"]).fileSystemRepresentation, 1);
        contrib_reminder(contrib);
        if (!test_profile) unsetenv("TOTK_AUTOLOAD");   // the game starts at its title screen, as on the console (a test on a save copy may skip it)
        setenv("TOTK_APP_BUNDLE", NSBundle.mainBundle.bundlePath.fileSystemRepresentation, 1);   // the menu's Restart now opens it again
        /* runtime command channel (runtime/input.c): echo "shot ~/Desktop/shot.png" >> "<data>/input.txt" */
        { NSString *cmds = env_or("TOTK_INPUT_FILE", [data stringByAppendingPathComponent:@"input.txt"]);
          FILE *cf = fopen(cmds.fileSystemRepresentation, "w"); if (cf) fclose(cf);
          setenv("TOTK_INPUT_FILE", cmds.fileSystemRepresentation, 1); }

        /* The log is written only with debug on, as on the iPhone: defaults write org.macnativegames.totk TOTK_DEBUG_LOG
           -bool YES (org.macnativegames.botw for Breath of the Wild), or TOTK_DEBUG_LOG=1 in the environment, which wins
           (the test tools pass it). Off, the game's output goes to /dev/null and the logs of earlier runs are removed. */
        const char *debugEnv = getenv("TOTK_DEBUG_LOG");
        BOOL debugLog = debugEnv ? (*debugEnv && strcmp(debugEnv, "0")) : [NSUserDefaults.standardUserDefaults boolForKey:@"TOTK_DEBUG_LOG"];
        NSString *log = [logs stringByAppendingPathComponent:@GAME_LOG ".log"];
        NSString *previous = [logs stringByAppendingPathComponent:@GAME_LOG ".previous.log"];
        [NSFileManager.defaultManager removeItemAtPath:previous error:nil];
        if (debugLog) [NSFileManager.defaultManager moveItemAtPath:log toPath:previous error:nil];
        else [NSFileManager.defaultManager removeItemAtPath:log error:nil];
        FILE *lf = fopen(debugLog ? log.fileSystemRepresentation : "/dev/null", "w");
        if (lf) {
            dup2(fileno(lf), STDOUT_FILENO);
            dup2(fileno(lf), STDERR_FILENO);
            fclose(lf);
        }
        char *const arguments[] = {(char *)binary.fileSystemRepresentation, "rewritten/" TOTK_GAME_DIR, "boot", NULL};
        execv(arguments[0], arguments);
        int failed = errno;
        fprintf(stderr, "[launcher] execv failed: %s\n", strerror(failed));
        show_error([NSString stringWithFormat:@"Starting the game failed: %s.", strerror(failed)]);
        return 1;
    }
}
