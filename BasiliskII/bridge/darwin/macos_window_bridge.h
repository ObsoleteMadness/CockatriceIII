/*
 *  macos_window_bridge.h - Host macOS Cocoa window mirroring bridge
 *
 *  Cockatrice III
 *  (C) 2026 Cockatrice III Project
 *
 *  Architectural Context:
 *  ======================
 *  The macOS half of the guest window mirroring experiment. toolbox_window.cpp
 *  watches the guest Window Manager and decides what exists; this file owns one
 *  NSWindow per mirrored guest window and draws each window's pixels into it.
 *
 *  Like the menu bridge, this file registers itself with the portable module and
 *  contributes no trap knowledge of its own.
 */

#ifndef MACOS_WINDOW_BRIDGE_H
#define MACOS_WINDOW_BRIDGE_H

#include "sysdeps.h"

#ifdef __cplusplus
#include "toolbox_window.h"
extern "C" {
#endif

/*
 * Installs this bridge as the platform back end for window mirroring.
 * Called during host application startup.
 */
void MacWindowBridge_Init(void);

/*
 * Wires the bridge to the Window Manager trap hooks: installs the callbacks and
 * asks toolbox_window.cpp to register the traps. Called once at startup.
 */
void MacWindowBridge_RegisterWindowTraps(void);

#ifdef __cplusplus
}
#endif

#endif /* MACOS_WINDOW_BRIDGE_H */
