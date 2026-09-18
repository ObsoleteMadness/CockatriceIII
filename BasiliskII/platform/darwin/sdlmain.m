/*
 *  SDLMain.m - main entry point for Cocoa SDL app on macOS
 */

#import <SDL/SDL.h>
#import "sdlmain.h"
#import "menu_bar.h"
#import "menu_model.h"
#import "macos_menu_bridge.h"
#import "macos_window_bridge.h"
#import <sys/param.h>
#import <unistd.h>

static int    gArgc;
static char  **gArgv;
static BOOL   gFinderLaunch;
static BOOL   gCalledAppMainline = FALSE;

static NSString *getApplicationName(void)
{
    NSDictionary *dict;
    NSString *appName = nil;

    dict = (NSDictionary *)CFBundleGetInfoDictionary(CFBundleGetMainBundle());
    if (dict)
        appName = [dict objectForKey: @"CFBundleName"];
    
    if (![appName length])
        appName = [[NSProcessInfo processInfo] processName];

    return appName;
}

@interface SDLApplication : NSApplication
@end

@implementation SDLApplication
- (void)terminate:(id)sender
{
    SDL_Event event;
    event.type = SDL_QUIT;
    SDL_PushEvent(&event);
}
@end

/*
 *  Cocoa target for every row in the shared menu model.
 *
 *  There is deliberately one selector for the whole menu bar: the row's
 *  command id rides along in the NSMenuItem tag, and MenuModel_Invoke() maps it
 *  back to the action. Adding a menu item never means adding a method here.
 */
@interface CocoaMenuHandler : NSObject
- (void)menuModelAction:(id)sender;
@end

@implementation CocoaMenuHandler
- (void)menuModelAction:(id)sender
{
    MenuModel_Invoke((int)[sender tag]);
}
@end

static CocoaMenuHandler *g_menuHandler = nil;

/* NSMenu built from each top-level menu in the model, in model order, so a
   refresh can find the native object that belongs to a MenuNode by index. */
static NSMenu     *g_modelMenus[MENU_MAX_MENUS] = {nil};
static NSMenuItem *g_modelMenuItems[MENU_MAX_MENUS] = {nil};
static int         g_modelMenuCount = 0;
/* Model revision the NSMenus above were built from; a bump means the shape of
   the bar changed (the Video menu gained or lost presets) and they have to be
   repopulated rather than retitled. */
static unsigned    g_modelRevision = 0;

static void setApplicationMenu(void)
{
    NSMenu *appleMenu;
    NSMenuItem *menuItem;
    NSString *title;
    NSString *appName;
    
    appName = getApplicationName();
    appleMenu = [[NSMenu alloc] initWithTitle:@""];
    
    title = [@"About " stringByAppendingString:appName];
    [appleMenu addItemWithTitle:title action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];

    [appleMenu addItem:[NSMenuItem separatorItem]];

    title = [@"Hide " stringByAppendingString:appName];
    [appleMenu addItemWithTitle:title action:@selector(hide:) keyEquivalent:@"h"];

    menuItem = (NSMenuItem *)[appleMenu addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    [menuItem setKeyEquivalentModifierMask:(NSEventModifierFlagOption|NSEventModifierFlagCommand)];

    [appleMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];

    [appleMenu addItem:[NSMenuItem separatorItem]];

    title = [@"Quit " stringByAppendingString:appName];
    [appleMenu addItemWithTitle:title action:@selector(terminate:) keyEquivalent:@"q"];

    menuItem = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    [menuItem setSubmenu:appleMenu];
    [[NSApp mainMenu] addItem:menuItem];

    [appleMenu release];
    [menuItem release];
}

/*
 *  Fill an NSMenu from one node of the shared model, replacing whatever it held.
 *
 *  Arguments:
 *    menu: native menu to populate; its existing items are discarded.
 *    node: the model node describing the rows to create.
 *
 *  Submenus recurse. Automatic item validation is turned off so the model's
 *  own enabled flag is what the user sees -- with validation on, Cocoa would
 *  re-enable every row whose target implements the selector, which is all of
 *  them, and "Detach" on an empty SCSI id would never gray out.
 */
static void populateMenuFromModel(NSMenu *menu, const MenuNode *node)
{
    [menu setAutoenablesItems:NO];
    [menu removeAllItems];

    for (int i = 0; i < node->item_count; i++) {
        const MenuItem *desc = &node->items[i];

        if (desc->kind == MENU_ITEM_SEPARATOR) {
            [menu addItem:[NSMenuItem separatorItem]];
            continue;
        }

        NSString *title = [NSString stringWithUTF8String:desc->label];
        /* A shortcut of 0 means "no key equivalent"; Cocoa spells that as the
           empty string and implies the Command modifier for a single letter. */
        NSString *key = desc->shortcut ? [NSString stringWithFormat:@"%c", desc->shortcut] : @"";

        if (desc->kind == MENU_ITEM_SUBMENU && desc->submenu) {
            NSMenu *submenu = [[NSMenu alloc] initWithTitle:title];
            populateMenuFromModel(submenu, desc->submenu);

            NSMenuItem *parent = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
            [parent setSubmenu:submenu];
            [menu addItem:parent];

            [submenu release];
            [parent release];
            continue;
        }

        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title
                                                      action:@selector(menuModelAction:)
                                               keyEquivalent:key];
        [item setTarget:g_menuHandler];
        /* The command id is the only state the callback needs. */
        [item setTag:desc->command_id];
        [item setEnabled:desc->enabled ? YES : NO];
        [menu addItem:item];
        [item release];
    }
}

/*
 *  Restate titles and enable flags on an already-populated NSMenu.
 *
 *  Used for the common refresh (a disk was attached or detached) where the rows
 *  are unchanged and only their text moves; walking in parallel by index is
 *  safe because the same model node produced the menu. Falls back to a full
 *  repopulate if the two ever disagree on row count.
 *
 *  Arguments:
 *    menu: native menu previously built from node.
 *    node: the model node to read the new titles and flags from.
 */
static void syncMenuFromModel(NSMenu *menu, const MenuNode *node)
{
    if ((int)[menu numberOfItems] != node->item_count) {
        populateMenuFromModel(menu, node);
        return;
    }

    for (int i = 0; i < node->item_count; i++) {
        const MenuItem *desc = &node->items[i];
        if (desc->kind == MENU_ITEM_SEPARATOR)
            continue;

        NSMenuItem *item = [menu itemAtIndex:i];
        NSString *title = [NSString stringWithUTF8String:desc->label];
        if (![[item title] isEqualToString:title])
            [item setTitle:title];

        if (desc->kind == MENU_ITEM_SUBMENU && desc->submenu) {
            /* The submenu carries the same caption as its parent row, so both
               follow a drive change together. */
            NSMenu *submenu = [item submenu];
            if (submenu) {
                [submenu setTitle:title];
                syncMenuFromModel(submenu, desc->submenu);
            }
            continue;
        }

        [item setEnabled:desc->enabled ? YES : NO];
    }
}

/*
 *  Create the menu bar described by the shared model and append it after the
 *  application menu. Called once, before NSApp runs.
 *
 *  The macOS menu bar has to exist before SDL_main() starts, so the video
 *  presets the Video menu is generated from cannot come from SDL's desktop
 *  query yet; NSScreen answers the same question here, and VideoInit() later
 *  refines the list through MenuBar_UpdateAll().
 */
static void setupModelMenus(void)
{
    NSScreen *screen = [NSScreen mainScreen];
    if (screen) {
        NSRect frame = [screen frame];
        MenuModel_SetHostScreenSize((int)frame.size.width, (int)frame.size.height);
    }

    const MenuBarModel *model = MenuModel_Get();
    g_modelMenuCount = model->menu_count;
    if (g_modelMenuCount > MENU_MAX_MENUS)
        g_modelMenuCount = MENU_MAX_MENUS;

    for (int i = 0; i < g_modelMenuCount; i++) {
        const MenuNode *node = &model->menus[i];
        NSString *title = [NSString stringWithUTF8String:node->label];

        NSMenu *menu = [[NSMenu alloc] initWithTitle:title];
        populateMenuFromModel(menu, node);

        NSMenuItem *menuItem = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
        [menuItem setSubmenu:menu];
        [[NSApp mainMenu] addItem:menuItem];

        /* Kept (retained by the menu bar) so MenuBar_UpdateAll() can reach them. */
        g_modelMenus[i] = menu;
        g_modelMenuItems[i] = menuItem;

        [menu release];
        [menuItem release];
    }

    g_modelRevision = MenuModel_Revision();
}

static void setupWindowMenu(void)
{
    NSMenu      *windowMenu;
    NSMenuItem  *windowMenuItem;
    NSMenuItem  *menuItem;

    windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    
    menuItem = [[NSMenuItem alloc] initWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [windowMenu addItem:menuItem];
    [menuItem release];
    
    windowMenuItem = [[NSMenuItem alloc] initWithTitle:@"Window" action:nil keyEquivalent:@""];
    [windowMenuItem setSubmenu:windowMenu];
    [[NSApp mainMenu] addItem:windowMenuItem];
    
    [NSApp setWindowsMenu:windowMenu];

    [windowMenu release];
    [windowMenuItem release];
}

/*
 *  Re-read the shared model and push any change onto the live NSMenus.
 *
 *  Called from the emulation thread after a SCSI attach or detach and from
 *  VideoInit() once SDL knows the real desktop size, so the work is marshalled
 *  onto the main thread where AppKit requires it.
 */
void MenuBar_UpdateAll(void)
{
    void (^updateBlock)(void) = ^{
        /* A true return means rows appeared or vanished, so retitling in place
           is not enough and the native menus must be rebuilt. */
        const bool structural = MenuModel_Refresh() || MenuModel_Revision() != g_modelRevision;
        const MenuBarModel *model = MenuModel_Get();

        for (int i = 0; i < g_modelMenuCount && i < model->menu_count; i++) {
            NSMenu *menu = g_modelMenus[i];
            if (!menu)
                continue;
            const MenuNode *node = &model->menus[i];

            NSString *title = [NSString stringWithUTF8String:node->label];
            [menu setTitle:title];
            if (g_modelMenuItems[i])
                [g_modelMenuItems[i] setTitle:title];

            if (structural)
                populateMenuFromModel(menu, node);
            else
                syncMenuFromModel(menu, node);
        }

        g_modelRevision = MenuModel_Revision();
    };

    if ([NSThread isMainThread]) {
        updateBlock();
    } else {
        dispatch_async(dispatch_get_main_queue(), updateBlock);
    }
}

void MenuBar_Init(void *native_window_handle)
{
    MacMenuBridge_RegisterMenuTraps();
    MacWindowBridge_RegisterWindowTraps();
    MenuBar_UpdateAll();
}

bool MenuBar_ShowOpenFileDialog(const char *title, const char *filter_desc, const char *filter_exts, char *out_path, size_t max_len)
{
    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];
        if (title) {
            [panel setMessage:[NSString stringWithUTF8String:title]];
            [panel setTitle:[NSString stringWithUTF8String:title]];
        }
        if ([panel runModal] == NSModalResponseOK) {
            NSURL *url = [[panel URLs] firstObject];
            if (url && [url path]) {
                const char *cpath = [[url path] UTF8String];
                if (cpath && out_path && max_len > 0) {
                    strncpy(out_path, cpath, max_len - 1);
                    out_path[max_len - 1] = '\0';
                    return true;
                }
            }
        }
    }
    return false;
}

@implementation SDLMain

- (void) setupWorkingDirectory:(BOOL)shouldChdir
{
    if (shouldChdir)
    {
        char appdir[MAXPATHLEN];
        CFURLRef url = CFBundleCopyBundleURL(CFBundleGetMainBundle());
        if (url) {
            if (CFURLGetFileSystemRepresentation(url, true, (UInt8 *)appdir, MAXPATHLEN)) {
                assert(chdir(appdir) == 0);
            }
            CFRelease(url);
        }
    }
}

static void CustomApplicationMain (int argc, char **argv)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    SDLMain *sdlMain;

    [SDLApplication sharedApplication];
    
    if ([NSApp respondsToSelector:@selector(setActivationPolicy:)]) {
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    }

    [NSApp setMainMenu:[[NSMenu alloc] init]];
    g_menuHandler = [[CocoaMenuHandler alloc] init];
    setApplicationMenu();
    setupModelMenus();
    setupWindowMenu();

    sdlMain = [[SDLMain alloc] init];
    [NSApp setDelegate:sdlMain];
    
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];
    
    [sdlMain release];
    [pool release];
}

- (BOOL)application:(NSApplication *)theApplication openFile:(NSString *)filename
{
    const char *temparg;
    size_t arglen;
    char *arg;
    char **newargv;

    if (!gFinderLaunch || gCalledAppMainline)
        return FALSE;

    temparg = [filename UTF8String];
    arglen = strlen(temparg) + 1;
    arg = (char *) malloc(arglen);
    if (arg == NULL)
        return FALSE;

    newargv = (char **) realloc(gArgv, sizeof (char *) * (gArgc + 2));
    if (newargv == NULL)
    {
        free(arg);
        return FALSE;
    }
    gArgv = newargv;

    strlcpy(arg, temparg, arglen);
    gArgv[gArgc++] = arg;
    gArgv[gArgc] = NULL;
    return TRUE;
}

- (void) applicationDidFinishLaunching: (NSNotification *) note
{
    int status;

    [self setupWorkingDirectory:gFinderLaunch];

    gCalledAppMainline = TRUE;
    status = SDL_main (gArgc, gArgv);

    exit(status);
}
@end

#ifdef main
#  undef main
#endif

int main (int argc, char **argv)
{
    /* Earliest possible stderr marker — if this never appears, dyld/kernel killed us before main */
    write(STDERR_FILENO, "[CockatriceIII] entering main\n", 31);

    /* Finder hasn't passed "-psn_..." since ~OS X 10.9, so a double-clicked
       (or otherwise no-arg) launch looks just like `argc == 1` now. Treat
       that the same as the old -psn case: chdir into the bundle directory
       so PREFS_FILE_NAME / the ROM resolve relative to it instead of
       whatever cwd Finder/launchd happened to set. Explicit CLI args (argc
       >= 2, not -psn) keep respecting the caller's cwd. */
    if ( argc < 2 || strncmp (argv[1], "-psn", 4) == 0 ) {
        gArgv = (char **) malloc(sizeof (char *) * 2);
        gArgv[0] = argv[0];
        gArgv[1] = NULL;
        gArgc = 1;
        gFinderLaunch = YES;
    } else {
        int i;
        gArgc = argc;
        gArgv = (char **) malloc(sizeof (char *) * (argc+1));
        for (i = 0; i <= argc; i++)
            gArgv[i] = argv[i];
        gFinderLaunch = NO;
    }

    CustomApplicationMain (argc, argv);
    return 0;
}
