/*
 *  macos_menu_bridge.mm - Host macOS Cocoa Menu Bar Integration Bridge
 *
 *  Cockatrice III
 *  (C) 2026 Cockatrice III Project
 *
 *  High-Level Architectural Context:
 *  ==================================
 *  This file implements the native macOS Cocoa bridge for the Menu Manager.
 *  When Mac OS 68k software alters the menu bar (via _DrawMenuBar, _InsertMenu,
 *  _SetMenuBar, etc.), this module extracts the active MenuList structure from
 *  guest RAM, transforms it into native Cocoa NSMenu and NSMenuItem hierarchies,
 *  and synchronizes the host macOS application menu bar on the main UI thread.
 *
 *  When a user selects a menu item in the native macOS menu bar, the Cocoa action
 *  forwards the (MenuID, ItemIndex) pair back across the thread-safe MenuQueue
 *  so the 68k CPU thread can simulate or invoke the guest selection.
 */

#import <Cocoa/Cocoa.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "menu_bar.h"
#include "prefs.h"
#include "toolbox_traps.h"
#include "macos_menu_bridge.h"

@interface CocoaMacMenuBridgeTarget : NSObject
- (void)menuItemSelected:(id)sender;
@end

static CocoaMacMenuBridgeTarget *g_menuTarget = nil;

@implementation CocoaMacMenuBridgeTarget

/*
 * Action handler triggered when the user clicks a native macOS menu item
 * corresponding to a guest Macintosh menu.
 */
- (void)menuItemSelected:(id)sender
{
	if ([sender isKindOfClass:[NSMenuItem class]]) {
		NSMenuItem *item = (NSMenuItem *)sender;
		int menuID = (int)item.tag;
		int itemIndex = 0;
		if (item.representedObject && [item.representedObject isKindOfClass:[NSNumber class]]) {
			itemIndex = [(NSNumber *)item.representedObject intValue];
		}

		printf("[MENU-BRIDGE] Native menu item clicked: '%s' (MenuID=%d, ItemIndex=%d)\n",
		       [item.title UTF8String], menuID, itemIndex);
		fflush(stdout);

		// Forward selection to the thread-safe menu command queue
		MacMenuBridge_SelectMenuItem((int16)menuID, (int16)itemIndex);
	}
}

@end

/*
 * Initializes the native macOS menu bar bridge.
 */
void MacMenuBridge_Init(void)
{
	if (!g_menuTarget) {
		g_menuTarget = [[CocoaMacMenuBridgeTarget alloc] init];
	}
	// Register platform sync callback so deferred trap hooks can refresh Cocoa menus on IRQ
	Toolbox_SetMenuBarSyncCallback(MacMenuBridge_SyncFromGuest);
	printf("[MENU-BRIDGE] Native macOS Cocoa menu bridge initialized.\n");
	fflush(stdout);
}

/*
 * Dispatches a menu item selection from the macOS UI thread to the emulation engine.
 *
 * Arguments:
 *   menuID: Macintosh Menu ID (e.g. 128, 129...).
 *   itemIndex: 1-based item index within the menu.
 */
void MacMenuBridge_SelectMenuItem(int16 menuID, int16 itemIndex)
{
	MenuAction_GuestMenuSelect((int)menuID, (int)itemIndex);
}

/*
 * Reconstructs the native Cocoa menu bar on the macOS main thread from a guest snapshot.
 *
 * Arguments:
 *   snapshot: Snapshot of guest Macintosh menus and menu items.
 */
static void UpdateCocoaMenuBarOnMainThread(const MacMenuBarSnapshot &snapshot)
{
	NSApplication *app = [NSApplication sharedApplication];
	NSMenu *mainMenu = [app mainMenu];

	if (!mainMenu) {
		mainMenu = [[NSMenu alloc] initWithTitle:@"MainMenu"];
		[app setMainMenu:mainMenu];
	}

	// Retain existing top-level Cockatrice application menu (first item at index 0)
	NSMenuItem *appMenuItem = nil;
	if ([mainMenu numberOfItems] > 0) {
		appMenuItem = [mainMenu itemAtIndex:0];
	}

	// Clear previous guest menus from main menu bar while preserving Cockatrice host menu
	while ([mainMenu numberOfItems] > 1) {
		[mainMenu removeItemAtIndex:1];
	}
	if ([mainMenu numberOfItems] == 0 && appMenuItem) {
		[mainMenu addItem:appMenuItem];
	}

	// Recreate each guest Macintosh menu in the native Cocoa menu bar
	for (size_t m = 0; m < snapshot.menus.size(); m++) {
		const MacMenuSnapshot &guestMenu = snapshot.menus[m];

		// Create native Cocoa menu container
		NSString *menuTitle = [NSString stringWithUTF8String:guestMenu.title.c_str()];
		if (!menuTitle) {
			menuTitle = [NSString stringWithCString:guestMenu.title.c_str() encoding:NSMacOSRomanStringEncoding];
		}
		if (!menuTitle) {
			menuTitle = @"Menu";
		}

		NSMenu *cocoaMenu = [[NSMenu alloc] initWithTitle:menuTitle];
		[cocoaMenu setAutoenablesItems:NO];

		// Populate menu items
		for (size_t i = 0; i < guestMenu.items.size(); i++) {
			const MacMenuItemSnapshot &guestItem = guestMenu.items[i];

			if (guestItem.isSeparator) {
				// Insert standard native menu separator
				[cocoaMenu addItem:[NSMenuItem separatorItem]];
			} else {
				NSString *itemTitle = [NSString stringWithUTF8String:guestItem.text.c_str()];
				if (!itemTitle) {
					itemTitle = [NSString stringWithCString:guestItem.text.c_str() encoding:NSMacOSRomanStringEncoding];
				}
				if (!itemTitle) {
					itemTitle = @"";
				}

				NSString *keyEq = @"";
				if (!guestItem.isSubmenu && guestItem.cmdChar != '\0' &&
				    guestItem.cmdChar != (char)kMenuNoMark && guestItem.cmdChar != (char)kMenuHierCmd) {
					char keyBuf[2] = {(char)tolower((unsigned char)guestItem.cmdChar), '\0'};
					keyEq = [NSString stringWithUTF8String:keyBuf];
				}

				NSMenuItem *cocoaItem = [[NSMenuItem alloc] initWithTitle:itemTitle
				                                                   action:@selector(menuItemSelected:)
				                                            keyEquivalent:keyEq];
				[cocoaItem setTarget:g_menuTarget];
				[cocoaItem setTag:(NSInteger)guestItem.menuID];
				[cocoaItem setRepresentedObject:@((NSInteger)guestItem.itemIndex)];
				[cocoaItem setEnabled:(guestItem.isEnabled && !guestItem.isSubmenu) ? YES : NO];

				// Set checkmark indicator if marked
				if (guestItem.markChar != 0) {
					[cocoaItem setState:NSControlStateValueOn];
				} else {
					[cocoaItem setState:NSControlStateValueOff];
				}

				// Standard Command key modifier mask
				if (keyEq.length > 0) {
					[cocoaItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
				}

				[cocoaMenu addItem:cocoaItem];
			}
		}

		// Create top-level menu bar item containing the submenu
		NSMenuItem *topItem = [[NSMenuItem alloc] initWithTitle:menuTitle action:nil keyEquivalent:@""];
		[topItem setSubmenu:cocoaMenu];
		[mainMenu addItem:topItem];
	}
}

/*
 * Captures the current guest Mac OS MenuList and updates the macOS native Cocoa menu bar.
 * Safe to call from the CPU/emulation thread.
 */
void MacMenuBridge_SyncFromGuest(void)
{
	MacMenuBarSnapshot snapshot;
	if (Toolbox_SnapshotMenuBar(snapshot)) {
		// Asynchronously update Cocoa UI on the main thread
		dispatch_async(dispatch_get_main_queue(), ^{
			UpdateCocoaMenuBarOnMainThread(snapshot);
		});
	}
}

/*
 * Generic hook callback for Menu Manager traps that modify menu bar state.
 */
static TOOLBOX_TRAP_HANDLER(Handle_MenuStateChange)
{
	// Defer sync until after the ROM Menu Manager trap completes (next IRQ)
	Toolbox_RequestMenuBarSync();

	// Passthrough: allow original Mac OS ROM code to complete internal updates
	return TOOLBOX_ACTION_PASSTHROUGH;
}

/*
 * Registers all Menu Manager trap hooks with the modular Toolbox Traps subsystem.
 */
void MacMenuBridge_RegisterMenuTraps(void)
{
	if (!PrefsFindBool("toolbox_hooks")) {
		printf("[MENU-BRIDGE] toolbox_hooks disabled; skipping Menu Manager trap registration.\n");
		fflush(stdout);
		return;
	}

	MacMenuBridge_Init();

	// Hook core Menu Manager traps that alter menu bar content or display
	ToolboxTrap_Register(0xa930, "_InitMenus",      Handle_MenuStateChange, NULL);
	ToolboxTrap_Register(0xa933, "_AppendMenu",     Handle_MenuStateChange, NULL);
	ToolboxTrap_Register(0xa934, "_ClearMenuBar",   Handle_MenuStateChange, NULL);
	ToolboxTrap_Register(0xa935, "_InsertMenu",     Handle_MenuStateChange, NULL);
	ToolboxTrap_Register(0xa936, "_DeleteMenu",     Handle_MenuStateChange, NULL);
	ToolboxTrap_Register(0xa937, "_DrawMenuBar",    Handle_MenuStateChange, NULL);
	ToolboxTrap_Register(0xa93c, "_SetMenuBar",     Handle_MenuStateChange, NULL);
	ToolboxTrap_Register(0xa81d, "_InvalMenuBar",   Handle_MenuStateChange, NULL);
	ToolboxTrap_Register(0xa826, "_InsertMenuItem", Handle_MenuStateChange, NULL);
	ToolboxTrap_Register(0xa827, "_DeleteMenuItem", Handle_MenuStateChange, NULL);

	printf("[MENU-BRIDGE] Registered Menu Manager trap hooks for native macOS menu sync.\n");
	fflush(stdout);
}
