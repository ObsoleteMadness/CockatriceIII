/*
 *  macos_menu_bridge.h - Host macOS Cocoa Menu Bar Integration Bridge
 *
 *  Cockatrice III
 *  (C) 2026 Cockatrice III Project
 *
 *  Architectural Context:
 *  ======================
 *  This header provides the host macOS integration bridge that connects the
 *  Macintosh Toolbox Menu Manager traps to native macOS Cocoa menus (NSMenu / NSMenuItem).
 *  When guest applications update their menu bar, the bridge extracts the menu tree
 *  from guest memory and dynamically reconstructs the macOS host application menu bar.
 */

#ifndef MACOS_MENU_BRIDGE_H
#define MACOS_MENU_BRIDGE_H

#include "sysdeps.h"

#ifdef __cplusplus
#include "toolbox_traps.h"
extern "C" {
#endif

/*
 * Initializes the macOS native menu bar bridge.
 * Called during host application startup.
 */
void MacMenuBridge_Init(void);

/*
 * Captures the current guest Mac OS MenuList and updates the macOS native Cocoa menu bar.
 * Safe to call from the CPU/emulation thread.
 */
void MacMenuBridge_SyncFromGuest(void);

/*
 * Dispatches a menu item selection from the macOS UI thread to the emulation engine.
 *
 * Arguments:
 *   menuID: Macintosh Menu ID (e.g. 128, 129...).
 *   itemIndex: 1-based item index within the menu.
 */
void MacMenuBridge_SelectMenuItem(int16 menuID, int16 itemIndex);

/*
 * Registers all Menu Manager trap hooks with the modular Toolbox Traps subsystem.
 */
void MacMenuBridge_RegisterMenuTraps(void);

#ifdef __cplusplus
}
#endif

#endif /* MACOS_MENU_BRIDGE_H */
