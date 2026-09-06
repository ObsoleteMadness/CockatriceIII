/*
 *  toolbox_menu.h - Menu Manager client of the Toolbox trap registry
 *
 *  Cockatrice III
 *  (C) 2026 Cockatrice III Project
 *
 *  High-Level Concept Block:
 *  =========================
 *  toolbox_traps.h declares the generic mechanism -- register a trap number and
 *  a C++ handler, get a RAM trampoline installed in the Mac OS trap table. This
 *  header declares the first subsystem built on it, and is the template for the
 *  next one: a handler plus a registration call in the .cpp, and here only the
 *  types the host UI needs plus a Register function it calls once.
 *
 *  Division of labour with platform code:
 *
 *    toolbox_menu.cpp  Menu Manager trap hooks, MenuList/MenuInfo decoding,
 *                      _MenuKey / _SystemMenu dispatch back into the guest.
 *                      Portable: no Cocoa, SDL or Win32.
 *    platform bridge   Turns a MacMenuBarSnapshot into host menu widgets and
 *                      posts user selections back. Registers its renderer with
 *                      Toolbox_SetMenuBarSyncCallback() and nothing else.
 *                      See BasiliskII/bridge/darwin/macos_menu_bridge.mm.
 *
 *  Sync is deferred rather than immediate: a hook runs *before* the ROM routine
 *  it stands in front of, so the guest menu state it would read is the state
 *  from before the call. Hooks therefore only raise a flag, and
 *  Toolbox_ProcessPendingMenuBarSync() -- driven from the IRQ path -- does the
 *  reading afterwards, on the CPU thread, where touching guest memory is safe.
 */

#ifndef TOOLBOX_MENU_H
#define TOOLBOX_MENU_H

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include <string>
#include <vector>

/*
 * Inside Macintosh Menu Manager constants (Toolbox Essentials, Chapter 3).
 */
enum {
	kMenuNoMark     = 0,    // Item has no marking character
	kMenuHierCmd    = 27,   // hMenuCmd ($1B): keyboard equiv marks a submenu
	kMenuDrawMsg    = 0,    // Menu def proc: draw items
	kMenuChooseMsg  = 1,    // Menu def proc: highlight item under cursor
	kMenuSizeMsg    = 2,    // Menu def proc: calculate dimensions
	kMenuPopUpMsg   = 3,    // Menu def proc: pop-up box rectangle
};

/*
 * Classic Menu Manager A-line trap opcodes used by the guest dispatch helpers.
 */
enum {
	kTrap_GetItemCmd    = 0xa815, // PROCEDURE GetItemCmd(theMenu, item, VAR cmdChar)
	kTrap_GetMenuHandle = 0xa939, // FUNCTION GetMenuHandle(menuID): MenuHandle
	kTrap_HiliteMenu    = 0xa938, // PROCEDURE HiliteMenu(menuID)
	kTrap_MenuSelect    = 0xa93d, // FUNCTION MenuSelect(startPt): LongInt
	kTrap_MenuKey       = 0xa93e, // FUNCTION MenuKey(ch): LongInt
	kTrap_SystemMenu    = 0xa9b5, // PROCEDURE SystemMenu(menuResult)
};

/*
 * Data structures representing decoded Macintosh Menu Manager state.
 * Layout follows the MenuInfo record in Inside Macintosh (menuID at +0,
 * enableFlags at +10, menuData Str255 at +14, then item definition bytes).
 */
struct MacMenuItemSnapshot {
	std::string text;             // UTF-8 item title (or "-" for separator)
	char cmdChar;                 // Keyboard shortcut (GetItemCmd); hMenuCmd if submenu
	uint8 markChar;               // Marking character (noMark=0; checkmark often $12)
	bool isSeparator;             // True if this item is a menu separator line
	bool isSubmenu;               // True when cmdChar == hMenuCmd ($1B)
	bool isEnabled;               // True if item is enabled (enableFlags bit)
	int16 menuID;                 // Parent Menu ID
	int16 itemIndex;              // 1-based item index within menu
};

struct MacMenuSnapshot {
	int16 menuID;                 // Menu ID (e.g. 128 for Apple menu, 129 for File...)
	std::string title;            // UTF-8 menu title
	bool isEnabled;               // True if entire menu is enabled
	std::vector<MacMenuItemSnapshot> items; // Child menu items
};

struct MacMenuBarSnapshot {
	std::vector<MacMenuSnapshot> menus; // List of menus currently installed in MenuList
};

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Registers the Menu Manager trap hooks with the Toolbox trap registry.
 * Call once during host UI setup, after Toolbox_SetMenuBarSyncCallback().
 * No-op while the toolbox_hooks pref is off.
 */
void ToolboxMenu_RegisterTraps(void);

/*
 * Decodes the guest Mac OS MenuList global (0x0A1C) and all MenuInfo records from guest RAM.
 *
 * Arguments:
 *   snapshot_out: Reference to snapshot structure to populate.
 *
 * Returns:
 *   true if MenuList was valid and decoded, false otherwise.
 */
bool Toolbox_SnapshotMenuBar(MacMenuBarSnapshot &snapshot_out);

/*
 * Schedules a deferred guest-to-host menu bar sync (processed on the next IRQ).
 * Use from trap pre-hooks so ROM Menu Manager updates complete before snapshotting.
 */
void Toolbox_RequestMenuBarSync(void);

/*
 * Installs the platform callback that renders a snapshot into the host menu bar.
 */
void Toolbox_SetMenuBarSyncCallback(void (*callback)(void));

/*
 * Runs a pending menu bar sync if one was requested. Call from the CPU thread (IRQ).
 */
void Toolbox_ProcessPendingMenuBarSync(void);

/*
 * Places the menu-bar mouseDown that makes the front application call
 * _MenuSelect, so that a menu item chosen on the host is dispatched by the
 * application itself. Called only from the jGNEFilter safe point.
 *
 * Arguments:
 *   event_record: guest EventRecord the Event Manager is about to return.
 *   result_addr: guest address of the Boolean word GetNextEvent returns.
 *
 * Returns:
 *   true if an event was placed.
 */
bool Toolbox_MenuSafePoint(uint32 event_record, uint32 result_addr);

/*
 * Activates a guest menu item by menu ID and 1-based item index.
 * Uses MenuKey when the item has a command-key shortcut; safe to call from CPU thread.
 *
 * Returns:
 *   true if the selection was dispatched, false if the item was not found or has no shortcut.
 */
bool Toolbox_DispatchGuestMenuSelect(int16 menuID, int16 itemIndex);

#ifdef __cplusplus
}
#endif

#endif /* TOOLBOX_MENU_H */
