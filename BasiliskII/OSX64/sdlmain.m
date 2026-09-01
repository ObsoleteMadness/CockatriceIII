/*
 *  SDLMain.m - main entry point for Cocoa SDL app on macOS
 */

#import <SDL/SDL.h>
#import "sdlmain.h"
#import "menu_bar.h"
#import "macos_menu_bridge.h"
#import "scsi.h"
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

@interface CocoaMenuHandler : NSObject
- (void)menuSaveConfig:(id)sender;
- (void)menuZapPRAM:(id)sender;
- (void)menuResetMachine:(id)sender;
- (void)menuShutdown:(id)sender;
- (void)menuForcePoweroff:(id)sender;
- (void)menuAddFloppy:(id)sender;
- (void)menuAttachSCSI:(id)sender;
- (void)menuDetachSCSI:(id)sender;
@end

@implementation CocoaMenuHandler
- (void)menuSaveConfig:(id)sender
{
    MenuAction_SaveConfig();
}

- (void)menuZapPRAM:(id)sender
{
    MenuAction_ZapPRAM();
}

- (void)menuResetMachine:(id)sender
{
    MenuAction_ResetMachine();
}

- (void)menuShutdown:(id)sender
{
    MenuAction_Shutdown();
}

- (void)menuForcePoweroff:(id)sender
{
    MenuAction_ForcePoweroff();
}

- (void)menuAddFloppy:(id)sender
{
    MenuAction_AddFloppy();
}

- (void)menuAttachSCSI:(id)sender
{
    int scsiId = (int)[sender tag];
    MenuAction_AttachSCSI(scsiId);
}

- (void)menuDetachSCSI:(id)sender
{
    int scsiId = (int)[sender tag];
    MenuAction_DetachSCSI(scsiId);
}
@end

static CocoaMenuHandler *g_menuHandler = nil;
static NSMenuItem *g_scsiMenuItems[7] = {nil};
static NSMenuItem *g_scsiDetachItems[7] = {nil};

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

static void setupFileMenu(void)
{
    NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    NSMenuItem *item;

    item = [[NSMenuItem alloc] initWithTitle:@"Save Configuration" action:@selector(menuSaveConfig:) keyEquivalent:@"s"];
    [item setTarget:g_menuHandler];
    [fileMenu addItem:item];
    [item release];

    [fileMenu addItem:[NSMenuItem separatorItem]];

    item = [[NSMenuItem alloc] initWithTitle:@"Zap PRAM" action:@selector(menuZapPRAM:) keyEquivalent:@""];
    [item setTarget:g_menuHandler];
    [fileMenu addItem:item];
    [item release];

    [fileMenu addItem:[NSMenuItem separatorItem]];

    item = [[NSMenuItem alloc] initWithTitle:@"Reset Machine" action:@selector(menuResetMachine:) keyEquivalent:@"r"];
    [item setTarget:g_menuHandler];
    [fileMenu addItem:item];
    [item release];

    item = [[NSMenuItem alloc] initWithTitle:@"Shutdown" action:@selector(menuShutdown:) keyEquivalent:@""];
    [item setTarget:g_menuHandler];
    [fileMenu addItem:item];
    [item release];

    item = [[NSMenuItem alloc] initWithTitle:@"Forced Poweroff and Exit" action:@selector(menuForcePoweroff:) keyEquivalent:@""];
    [item setTarget:g_menuHandler];
    [fileMenu addItem:item];
    [item release];

    NSMenuItem *fileMenuItem = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
    [fileMenuItem setSubmenu:fileMenu];
    [[NSApp mainMenu] addItem:fileMenuItem];

    [fileMenu release];
    [fileMenuItem release];
}

static void setupDiskMenu(void)
{
    NSMenu *diskMenu = [[NSMenu alloc] initWithTitle:@"Disk"];
    NSMenuItem *item;

    item = [[NSMenuItem alloc] initWithTitle:@"Add Floppy..." action:@selector(menuAddFloppy:) keyEquivalent:@""];
    [item setTarget:g_menuHandler];
    [diskMenu addItem:item];
    [item release];

    [diskMenu addItem:[NSMenuItem separatorItem]];

    for (int i = 0; i < 7; i++) {
        NSString *scsiTitle = [NSString stringWithFormat:@"SCSI %d", i];
        NSMenu *scsiSubmenu = [[NSMenu alloc] initWithTitle:scsiTitle];

        NSMenuItem *attachItem = [[NSMenuItem alloc] initWithTitle:@"Attach..." action:@selector(menuAttachSCSI:) keyEquivalent:@""];
        [attachItem setTarget:g_menuHandler];
        [attachItem setTag:i];
        [scsiSubmenu addItem:attachItem];
        [attachItem release];

        NSMenuItem *detachItem = [[NSMenuItem alloc] initWithTitle:@"Detach" action:@selector(menuDetachSCSI:) keyEquivalent:@""];
        [detachItem setTarget:g_menuHandler];
        [detachItem setTag:i];
        [scsiSubmenu addItem:detachItem];
        g_scsiDetachItems[i] = detachItem;

        NSMenuItem *scsiMenuItem = [[NSMenuItem alloc] initWithTitle:scsiTitle action:nil keyEquivalent:@""];
        [scsiMenuItem setSubmenu:scsiSubmenu];
        [diskMenu addItem:scsiMenuItem];
        g_scsiMenuItems[i] = scsiMenuItem;

        [scsiSubmenu release];
        [scsiMenuItem release];
    }

    NSMenuItem *diskMenuItem = [[NSMenuItem alloc] initWithTitle:@"Disk" action:nil keyEquivalent:@""];
    [diskMenuItem setSubmenu:diskMenu];
    [[NSApp mainMenu] addItem:diskMenuItem];

    [diskMenu release];
    [diskMenuItem release];
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

void MenuBar_UpdateAll(void)
{
    void (^updateBlock)(void) = ^{
        for (int i = 0; i < 7; i++) {
            if (g_scsiMenuItems[i]) {
                bool present = false, cdrom = false;
                char path[1024] = {0};
                SCSI_GetDeviceInfo(i, &present, &cdrom, path, sizeof(path));
                if (present && path[0] != '\0') {
                    const char *filename = strrchr(path, '/');
                    filename = filename ? filename + 1 : path;
                    NSString *title = [NSString stringWithFormat:@"SCSI %d: %s (%s)", i, filename, cdrom ? "CD-ROM" : "HDD"];
                    [g_scsiMenuItems[i] setTitle:title];
                    if (g_scsiDetachItems[i]) {
                        [g_scsiDetachItems[i] setEnabled:YES];
                    }
                } else {
                    NSString *title = [NSString stringWithFormat:@"SCSI %d (Empty)", i];
                    [g_scsiMenuItems[i] setTitle:title];
                    if (g_scsiDetachItems[i]) {
                        [g_scsiDetachItems[i] setEnabled:NO];
                    }
                }
            }
        }
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
    setupFileMenu();
    setupDiskMenu();
    setupWindowMenu();
    MenuBar_UpdateAll();

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

    if ( argc >= 2 && strncmp (argv[1], "-psn", 4) == 0 ) {
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
