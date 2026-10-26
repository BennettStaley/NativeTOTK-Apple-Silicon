#import "spawn_menu.h"
#include "actor_spawn.h"
#include "spawn_list.h"

/* settings_menu.m's controller bits */
enum { SP_UP = 1, SP_DOWN = 2, SP_LEFT = 4, SP_RIGHT = 8, SP_OK = 16, SP_PAGE_UP = 64, SP_PAGE_DOWN = 128 };

static SpawnList spawnList; static SpawnPicks spawnPicks; static BOOL spawnLoaded;
static void spawn_load(void) {
    if (spawnLoaded) return;
    spawnLoaded = YES;
    NSString *path = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"GameData/actor_list.tsv"];
    int n = spawn_list_load(&spawnList, path.fileSystemRepresentation);
    fprintf(stderr, "[spawn] actor list: %d actors\n", n);
    spawn_picks_load(&spawnPicks);
}
static NSString *spawn_display(NSString *name) {
    int i = spawn_list_find(&spawnList, name.UTF8String);
    return i >= 0 ? @(spawnList.actor[i].display) : name;
}

/* The toast: top-right of the game's window, 2.5 s. */
static void spawn_toast(NSString *text, BOOL failed) {
    static UILabel *toast; static NSUInteger generation;
    UIWindow *window = nil;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes)
        if ([scene isKindOfClass:UIWindowScene.class]) for (UIWindow *w in ((UIWindowScene *)scene).windows) if (w.isKeyWindow) window = w;
    if (!window) return;
    if (!toast) {
        toast = [UILabel new]; toast.font = [UIFont systemFontOfSize:14 weight:UIFontWeightSemibold]; toast.textAlignment = NSTextAlignmentCenter;
        toast.layer.cornerRadius = 8; toast.layer.masksToBounds = YES; toast.numberOfLines = 2;
    }
    toast.text = text; toast.textColor = failed ? UIColor.blackColor : UIColor.whiteColor;
    toast.backgroundColor = failed ? UIColor.systemOrangeColor : [UIColor colorWithWhite:0.08 alpha:0.88];
    CGSize size = [toast sizeThatFits:CGSizeMake(window.bounds.size.width * 0.5, 200)];
    size.width += 24; size.height += 12;
    UIEdgeInsets safe = window.safeAreaInsets;
    toast.frame = CGRectMake(window.bounds.size.width - safe.right - size.width - 12, safe.top + 12, size.width, size.height);
    toast.alpha = 1; [window addSubview:toast];
    NSUInteger mine = ++generation;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (mine != generation) return;
        [UIView animateWithDuration:0.4 animations:^{ toast.alpha = 0; } completion:^(BOOL done) { (void)done; if (mine == generation) [toast removeFromSuperview]; }];
    });
    fprintf(stderr, "[spawn] toast: %s\n", text.UTF8String);
}
static void spawn_observed(const char *name, int requested, int created, const char *problem) {
    NSString *n = @(name), *why = problem ? @(problem) : @"?";
    dispatch_async(dispatch_get_main_queue(), ^{
        NSString *display = spawn_display(n), *text =
            created == requested ? [NSString stringWithFormat:@"Spawned %@ ×%d", display, created]
          : created ? [NSString stringWithFormat:@"Spawned %@ ×%d of %d: %@", display, created, requested, why]
                    : [NSString stringWithFormat:@"Could not spawn %@: %@", display, why];
        spawn_toast(text, created < requested);
    });
}

NSString *TOTKSpawnSummary(void) {
    int a = runtime_spawn_available();
    if (a == SPAWN_ERR_UNSUPPORTED) return @"Spawning needs game version 1.0.0 or 1.4.3 for now.";
    if (a != SPAWN_OK) return @"The spawner is off on this game (the log says why).";
    return @"Items, weapons, creatures and Zonai devices next to you.";
}

@interface TOTKSpawnViewController () <UISearchResultsUpdating> @end
@implementation TOTKSpawnViewController {
    UISearchController *searchController;
    int category, amount, *shown, nshown; BOOL unnamed;
    NSString *query;
    NSInteger cursor;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    spawn_load(); runtime_spawn_observe(spawn_observed); runtime_spawn_set_placement(spawnPicks.place);
    category = SPAWN_CAT_ITEMS; amount = 1; query = @""; cursor = -1;
    shown = malloc(sizeof *shown * (size_t)(spawnList.count ? spawnList.count : 1));
    self.title = @"Spawn";
    searchController = [[UISearchController alloc] initWithSearchResultsController:nil];
    searchController.searchResultsUpdater = self; searchController.obscuresBackgroundDuringPresentation = NO;
    searchController.searchBar.placeholder = @"Search every category";
    self.navigationItem.searchController = searchController; self.navigationItem.hidesSearchBarWhenScrolling = NO;
    [self rebuildMenus]; [self filter];
}
- (void)dealloc { free(shown); }

- (void)rebuildMenus {
    __weak TOTKSpawnViewController *weak = self;
    NSMutableArray *cats = [NSMutableArray array];
    for (int c = 0; c < SPAWN_SHOWN_CATS; c++) {
        if (c == SPAWN_CAT_DISHES) continue;   /* giving dishes is Mac-only for now */
        UIAction *a = [UIAction actionWithTitle:@(SPAWN_CAT_NAMES[c]) image:nil identifier:nil handler:^(UIAction *x) { (void)x; [weak choose:c]; }];
        a.state = c == category ? UIMenuElementStateOn : UIMenuElementStateOff; [cats addObject:a];
    }
    UIAction *show = [UIAction actionWithTitle:@"Show unnamed" image:nil identifier:nil handler:^(UIAction *x) { (void)x; [weak toggleUnnamed]; }];
    show.state = unnamed ? UIMenuElementStateOn : UIMenuElementStateOff;
    UIMenu *menu = [UIMenu menuWithChildren:@[[UIMenu menuWithTitle:@"" image:nil identifier:nil options:UIMenuOptionsDisplayInline children:cats], show]];
    UIBarButtonItem *catItem = [[UIBarButtonItem alloc] initWithTitle:@(SPAWN_CAT_NAMES[category]) menu:menu];
    NSMutableArray *counts = [NSMutableArray array];
    for (int n = 1; n <= SPAWN_COUNT_MAX; n++) {
        UIAction *a = [UIAction actionWithTitle:[NSString stringWithFormat:@"× %d", n] image:nil identifier:nil handler:^(UIAction *x) { (void)x; [weak setAmount:n]; }];
        a.state = n == amount ? UIMenuElementStateOn : UIMenuElementStateOff; [counts addObject:a];
    }
    UIBarButtonItem *amountItem = [[UIBarButtonItem alloc] initWithTitle:[NSString stringWithFormat:@"× %d", amount] menu:[UIMenu menuWithChildren:counts]];
    NSMutableArray *places = [NSMutableArray array];
    for (int i = 0; i < SPAWN_PLACE_COUNT; i++) {
        UIAction *a = [UIAction actionWithTitle:@(SPAWN_PLACE_NAMES[i]) image:nil identifier:nil handler:^(UIAction *x) { (void)x; [weak setPlace:i]; }];
        a.state = i == spawnPicks.place ? UIMenuElementStateOn : UIMenuElementStateOff; [places addObject:a];
    }
    UIBarButtonItem *placeItem = [[UIBarButtonItem alloc] initWithTitle:@(SPAWN_PLACE_NAMES[spawnPicks.place]) menu:[UIMenu menuWithChildren:places]];
    self.navigationItem.rightBarButtonItems = @[amountItem, placeItem, catItem];
}
- (void)choose:(int)c { category = c; cursor = -1; searchController.searchBar.text = @""; query = @""; [self rebuildMenus]; [self filter]; }
- (void)toggleUnnamed { unnamed = !unnamed; [self rebuildMenus]; [self filter]; }
- (void)setAmount:(int)n { amount = MAX(1, MIN(SPAWN_COUNT_MAX, n)); [self rebuildMenus]; }
/* Where spawns go (spawn-picks.txt "place"), like the Mac tab's Place popup. */
- (void)setPlace:(int)p {
    spawnPicks.place = ((p % SPAWN_PLACE_COUNT) + SPAWN_PLACE_COUNT) % SPAWN_PLACE_COUNT;
    runtime_spawn_set_placement(spawnPicks.place); spawn_picks_save(&spawnPicks); [self rebuildMenus];
}
- (void)updateSearchResultsForSearchController:(UISearchController *)s { query = s.searchBar.text ?: @""; cursor = -1; [self filter]; }

- (void)filter {
    nshown = spawnList.count ? spawn_list_filter_dishes(&spawnList, &spawnPicks, category, query.UTF8String, unnamed, 0, shown, spawnList.count) : 0;
    [self.tableView reloadData];
}
- (NSString *)emptyText {
    int a = runtime_spawn_available();
    if (a != SPAWN_OK) return TOTKSpawnSummary();
    if (!spawnList.count) return @"This app has no actor list. Rebuild it (build_app.py makes it from your game files).";
    if (query.length) return @"Nothing matches.";
    if (category == SPAWN_CAT_FAVORITES) return @"No favorites yet: the star marks one.";
    if (category == SPAWN_CAT_RECENTS) return @"Nothing spawned yet.";
    return @"Nothing here.";
}
- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { (void)t; (void)s; return runtime_spawn_available() == SPAWN_OK ? nshown : 0; }
- (NSString *)tableView:(UITableView *)t titleForFooterInSection:(NSInteger)s { (void)t; (void)s; return nshown && runtime_spawn_available() == SPAWN_OK ? nil : [self emptyText]; }
- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)path {
    UITableViewCell *cell = [t dequeueReusableCellWithIdentifier:@"actor"];
    if (!cell) cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"actor"];
    const SpawnActor *a = &spawnList.actor[shown[path.row]];
    cell.textLabel.text = @(a->display);
    cell.detailTextLabel.text = query.length ? [NSString stringWithFormat:@"%s · %s", a->name, SPAWN_CAT_NAMES[a->category]] : @(a->name);
    cell.detailTextLabel.textColor = UIColor.tertiaryLabelColor;
    cell.detailTextLabel.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
    UIButton *star = [UIButton buttonWithType:UIButtonTypeSystem];
    [star setImage:[UIImage systemImageNamed:spawn_picks_is_favorite(&spawnPicks, a->name) ? @"star.fill" : @"star"] forState:UIControlStateNormal];
    star.tintColor = UIColor.systemYellowColor; star.frame = CGRectMake(0, 0, 36, 36); star.tag = path.row;
    [star addTarget:self action:@selector(starTapped:) forControlEvents:UIControlEventTouchUpInside];
    cell.accessoryView = star;
    return cell;
}
- (void)starTapped:(UIButton *)b { [self favorite:b.tag]; }
- (UISwipeActionsConfiguration *)tableView:(UITableView *)t leadingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)path {
    (void)t; __weak TOTKSpawnViewController *weak = self; NSInteger row = path.row;
    UIContextualAction *a = [UIContextualAction contextualActionWithStyle:UIContextualActionStyleNormal title:@"Favorite"
        handler:^(UIContextualAction *x, UIView *v, void (^done)(BOOL)) { (void)x; (void)v; [weak favorite:row]; done(YES); }];
    a.backgroundColor = UIColor.systemYellowColor;
    return [UISwipeActionsConfiguration configurationWithActions:@[a]];
}
- (void)tableView:(UITableView *)t didSelectRowAtIndexPath:(NSIndexPath *)path { cursor = path.row; [self spawn:path.row]; (void)t; }

- (void)favorite:(NSInteger)row {
    if (row < 0 || row >= nshown) return;
    if (spawn_picks_toggle_favorite(&spawnPicks, spawnList.actor[shown[row]].name) < 0) return;
    spawn_picks_save(&spawnPicks);
    if (category == SPAWN_CAT_FAVORITES && !query.length) [self filter];
    else [self.tableView reloadRowsAtIndexPaths:@[[NSIndexPath indexPathForRow:row inSection:0]] withRowAnimation:UITableViewRowAnimationNone];
    [self showCursor];
}
- (void)spawn:(NSInteger)row {
    if (row < 0 || row >= nshown) return;
    const SpawnActor *a = &spawnList.actor[shown[row]];
    int r = runtime_spawn_actor(a->name, amount);
    if (r != SPAWN_OK) {
        spawn_toast([NSString stringWithFormat:@"Could not spawn %s: %@", a->display,
                     r == SPAWN_ERR_UNSUPPORTED ? @"spawning needs game version 1.0.0 or 1.4.3" : @(runtime_spawn_error(r))], YES);
        return;
    }
    spawn_picks_add_recent(&spawnPicks, a->name); spawn_picks_save(&spawnPicks);
    if (category == SPAWN_CAT_RECENTS && !query.length) { [self filter]; cursor = 0; }
    [self showCursor];
}

- (void)showCursor {
    if (cursor < 0 || cursor >= nshown) return;
    [self.tableView selectRowAtIndexPath:[NSIndexPath indexPathForRow:cursor inSection:0] animated:NO scrollPosition:UITableViewScrollPositionMiddle];
}
- (BOOL)press:(unsigned)b {
    if (b & (SP_UP | SP_DOWN | SP_PAGE_UP | SP_PAGE_DOWN)) {
        int by = b & SP_UP ? -1 : b & SP_DOWN ? 1 : b & SP_PAGE_UP ? -8 : 8;
        cursor = nshown ? MAX(0, MIN((NSInteger)nshown - 1, cursor < 0 ? 0 : cursor + by)) : -1;   /* no wrapping round */
        [self showCursor]; return YES;
    }
    if (b & (SP_LEFT | SP_RIGHT)) { [self setAmount:amount + (b & SP_LEFT ? -1 : 1)]; return YES; }
    if (b & SP_OK) { [self spawn:cursor]; return YES; }
    if (b & SPAWN_PAD_FAVORITE) { [self favorite:cursor]; return YES; }
    return NO;
}
@end
