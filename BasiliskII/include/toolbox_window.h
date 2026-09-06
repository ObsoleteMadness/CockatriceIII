/*
 *  toolbox_window.h - Window Manager client of the Toolbox trap registry
 *
 *  Cockatrice III
 *  (C) 2026 Cockatrice III Project
 *
 *  High-Level Concept Block:
 *  =========================
 *  The second client of the trap registry, built to the same shape as
 *  toolbox_menu.h: the .cpp brings a handler plus a registration call, and this
 *  header declares only the types host UI code needs plus the entry points it
 *  drives.
 *
 *  What it does: mirrors each Mac OS window into a host window. The guest keeps
 *  running an ordinary Window Manager; we watch its window list, and give the
 *  host enough to put a native frame around each window's content.
 *
 *  Division of labour with platform code:
 *
 *    toolbox_window.cpp  WindowList decoding, the adopt/retire lifecycle, and
 *                        (later) redirecting each window's port at a private
 *                        pixel buffer. Portable: no Cocoa, SDL or Win32.
 *    platform bridge     Turns MacWindowSnapshot into host windows and presents
 *                        MacWindowBuffer pixels. Registers itself with
 *                        ToolboxWindow_SetCallbacks() and nothing else.
 *                        See BasiliskII/bridge/darwin/macos_window_bridge.mm.
 *
 *  Why hooks are only hints:
 *  -------------------------
 *  A trap hook runs *before* the ROM routine it stands in front of, so a hook on
 *  _NewWindow would see the state from before the window existed. Rather than
 *  fight that, window discovery is done by walking WindowList on the IRQ path
 *  and diffing against the previous walk -- creation, move, resize, retitle,
 *  show/hide and z-order all fall out of the diff for free. Only teardown needs
 *  a real hook, because a disposed window must be un-redirected while its port
 *  is still intact.
 */

#ifndef TOOLBOX_WINDOW_H
#define TOOLBOX_WINDOW_H

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include <string>
#include <vector>

/*
 * Classic Window Manager A-line trap opcodes.
 *
 * Numbers are the Toolbox trap table offsets from OS/DispTable.a in the Apple
 * SuperMario ROM sources, biased by the $A800 Toolbox base: DispTable's
 * "ToolBox $113,NewWindow" is trap $A913.
 */
enum {
	kTrap_InitWindows   = 0xa912, // PROCEDURE InitWindows
	kTrap_NewWindow     = 0xa913, // FUNCTION  NewWindow(...): WindowPtr
	kTrap_DisposeWindow = 0xa914, // PROCEDURE DisposeWindow(theWindow)
	kTrap_ShowWindow    = 0xa915, // PROCEDURE ShowWindow(theWindow)
	kTrap_HideWindow    = 0xa916, // PROCEDURE HideWindow(theWindow)
	kTrap_SetWTitle     = 0xa91a, // PROCEDURE SetWTitle(theWindow, title)
	kTrap_MoveWindow    = 0xa91b, // PROCEDURE MoveWindow(theWindow, h, v, front)
	kTrap_SizeWindow    = 0xa91d, // PROCEDURE SizeWindow(theWindow, w, h, update)
	kTrap_SelectWindow  = 0xa91f, // PROCEDURE SelectWindow(theWindow)
	kTrap_HiliteWindow  = 0xa91c, // PROCEDURE HiliteWindow(theWindow, fHilite)
	kTrap_BringToFront  = 0xa920, // PROCEDURE BringToFront(theWindow)
	kTrap_BeginUpdate   = 0xa922, // PROCEDURE BeginUpdate(theWindow)
	kTrap_EndUpdate     = 0xa923, // PROCEDURE EndUpdate(theWindow)
	kTrap_InvalRect     = 0xa928, // PROCEDURE InvalRect(badRect)
	kTrap_CloseWindow   = 0xa92d, // PROCEDURE CloseWindow(theWindow)
	kTrap_GetNewWindow  = 0xa9bd, // FUNCTION  GetNewWindow(...): WindowPtr
	kTrap_RectRgn       = 0xa8df, // PROCEDURE RectRgn(rgn, r)
	kTrap_SetPort       = 0xa873, // PROCEDURE SetPort(port)
	kTrap_GetPort       = 0xa874, // PROCEDURE GetPort(VAR port)
	kTrap_WaitNextEvent = 0xa860, // FUNCTION  WaitNextEvent(...): Boolean
	kTrap_EventAvail    = 0xa971, // FUNCTION  EventAvail(mask, VAR evt): Boolean
	kTrap_AllocCursor      = 0xaa1d, // PROCEDURE AllocCursor; rebuilds CrsrRow / cursor data
	kTrap_DisplayDispatch  = 0xabeb  // Display Manager; D0 = (paramWords<<8)|selector
};

/*
 * Host input being forwarded to the guest.
 */
enum {
	kGuestInput_MouseMove = 0,
	kGuestInput_MouseDown = 1,
	kGuestInput_MouseUp   = 2,
	kGuestInput_KeyDown   = 3,
	kGuestInput_KeyUp     = 4
};

/*
 * WindowRecord.windowKind values. Dialogs and alerts identify themselves here,
 * which is what lets them be mapped onto native host alerts instead of being
 * mirrored as anonymous rectangles of pixels.
 */
enum {
	kWindowKind_Dialog = 2, // dialogKind: a DialogRecord, so it has an item list
	kWindowKind_User   = 8  // userKind: an ordinary application window
};

/*
 * Window definition variation codes: the low four bits of a window's procID.
 *
 * The procID an application passes to _NewWindow is (WDEF resource ID << 4) plus
 * a variation code, and the Window Manager keeps the variation code in the high
 * byte of the windowDefProc handle field, which is where these are read from.
 * The WDEF resource ID is not recoverable from the record, so rDocProc -- procID
 * 16, meaning WDEF 1 variant 0 -- is indistinguishable here from documentProc.
 */
enum {
	kWindowVariant_Document    = 0,  // movable, sizable, no zoom box
	kWindowVariant_DBox        = 1,  // alert box or modal dialog box
	kWindowVariant_PlainDBox   = 2,  // plain box
	kWindowVariant_AltDBox     = 3,  // plain box with a shadow
	kWindowVariant_NoGrowDoc   = 4,  // movable, no size box or zoom box
	kWindowVariant_MovableDBox = 5,  // movable modal dialog box
	kWindowVariant_ZoomDoc     = 8,  // standard document window
	kWindowVariant_ZoomNoGrow  = 12  // zoomable, not resizable
};

/*
 * Item types in a dialog item list ('DITL'). The high bit is a disable flag
 * rather than part of the type, hence the mask.
 */
enum {
	kDialogItem_UserItem    = 0,
	kDialogItem_Button      = 4,
	kDialogItem_CheckBox    = 5,
	kDialogItem_RadioButton = 6,
	kDialogItem_Control     = 7,
	kDialogItem_StaticText  = 8,
	kDialogItem_EditText    = 16,
	kDialogItem_Icon        = 32,
	kDialogItem_Picture     = 64,
	kDialogItem_TypeMask    = 0x7f,
	kDialogItem_DisableBit  = 0x80
};

/*
 * One decoded entry from a dialog's item list. Rects are global, so a host
 * click can be mapped straight back onto the item the user aimed at.
 */
struct MacDialogItem {
	int index;                    // 1-based item number, as ModalDialog reports it
	int type;                     // kDialogItem_* after masking off the disable bit
	bool enabled;
	std::string text;             // Button title or static text; empty for icons
	int16 top;                    // Global coordinates
	int16 left;
	int16 bottom;
	int16 right;
};

/*
 * A window as the host needs to see it. Coordinates are global (screen) pixels,
 * taken from the region bounding boxes rather than computed from the port, so
 * no local-to-global arithmetic is involved.
 */
struct MacWindowSnapshot {
	uint32 window_ptr;            // WindowPtr in guest RAM; the window's identity
	uint32 layer_ptr;             // Owning layer record, or 0 -- groups windows by application
	std::string title;            // UTF-8 window title
	int16 content_top;            // contRgn bounding box, global coordinates
	int16 content_left;
	int16 content_bottom;
	int16 content_right;
	int16 struc_top;              // strucRgn bounding box (content plus WDEF frame)
	int16 struc_left;
	int16 struc_bottom;
	int16 struc_right;
	int16 window_kind;            // >= 0 application window, < 0 desk accessory
	int16 variant;                // WDEF variation code: documentProc, dBoxProc, ...
	bool go_away;                 // WindowRecord.goAwayFlag: the window has a close box
	bool is_dialog;               // windowKind is dialogKind, so it has an item list
	bool visible;                 // WindowRecord.visible
	bool hilited;                 // WindowRecord.hilited (front window of its layer)
	int z_order;                  // 0 is frontmost; position in the WindowList walk
};

/*
 * Where a redirected window's pixels live. Valid only for adopted windows whose
 * port has been spliced; base_addr is a guest address, so the host reaches the
 * pixels with Mac2HostAddr().
 */
struct MacWindowBuffer {
	uint32 base_addr;             // Guest address of the top-left pixel
	int32 row_bytes;              // Bytes per row (already masked of the PixMap flag bits)
	int32 width;                  // Pixels
	int32 height;
	int16 pixel_size;             // Bits per pixel; matches the screen depth
};

/*
 * Platform bridge entry points. All are called on the CPU thread: window_added,
 * window_changed and window_removed from the IRQ path, window_present from the
 * video refresh. None may call back into the guest.
 */
struct ToolboxWindowCallbacks {
	void (*window_added)(const MacWindowSnapshot *w);
	void (*window_changed)(const MacWindowSnapshot *w);
	void (*window_removed)(uint32 window_ptr);
	void (*window_present)(uint32 window_ptr, const MacWindowBuffer *buf);
};

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Registers the Window Manager trap hooks with the Toolbox trap registry.
 * Call once during host UI setup, after ToolboxWindow_SetCallbacks().
 * No-op while either the toolbox_hooks or mdi_windows pref is off.
 */
void ToolboxWindow_RegisterTraps(void);

/*
 * Installs the platform bridge callbacks. Pass NULL to detach.
 */
void ToolboxWindow_SetCallbacks(const struct ToolboxWindowCallbacks *callbacks);

/*
 * Walks the guest WindowList global (0x09D6) and decodes every application
 * window into snapshot_out, frontmost first.
 *
 * Arguments:
 *   snapshot_out: Vector to fill; cleared first.
 *
 * Returns:
 *   true if WindowList was valid and at least one window decoded.
 */
bool Toolbox_SnapshotWindowList(std::vector<MacWindowSnapshot> &snapshot_out);

/*
 * Re-walks the window list and reconciles it against the previous walk, driving
 * the added/changed/removed callbacks. Called from the IRQ path in
 * SDL/menu_bar.cpp, on the CPU thread, where touching guest memory is safe.
 */
void Toolbox_ProcessPendingWindowSync(void);

/*
 * Hands each adopted window's current pixels to the platform bridge. Called
 * from the video refresh, after the main screen blit.
 */
void Toolbox_PresentWindows(void);

/*
 * Decodes a dialog window's item list, so the host can present it as a native
 * alert rather than as a bitmap.
 *
 * Called once when a dialog is adopted rather than on every walk: item lists do
 * not change while a dialog is up, and decoding one is far more work than the
 * geometry diff.
 *
 * Arguments:
 *   window_ptr:  WindowPtr of a window whose is_dialog flag is set.
 *   items_out:   Vector to fill; cleared first.
 *   default_item_out: Receives the 1-based default item, or 0 if none.
 *
 * Returns:
 *   true if the item list was valid and decoded.
 */
bool Toolbox_SnapshotDialogItems(uint32 window_ptr,
                                 std::vector<MacDialogItem> &items_out,
                                 int *default_item_out);

/*
 * Clicks a dialog item on the user's behalf, by steering the emulated mouse to
 * the item and pressing it. Called from the IRQ path after the host alert's
 * button was clicked.
 *
 * Arguments:
 *   window_ptr: WindowPtr of the dialog.
 *   item_index: 1-based item number to click.
 */
void Toolbox_ClickDialogItem(uint32 window_ptr, int item_index);

/*
 * Brings a guest window to the front and activates it, matching a host window
 * having been focused.
 *
 * The call is queued rather than made immediately: see the note on safe points
 * in toolbox_window.cpp. Window Manager traps must not be entered from the
 * interrupt.
 *
 * Arguments:
 *   window_ptr: WindowPtr to select.
 */
void Toolbox_SelectGuestWindow(uint32 window_ptr);

/*
 * Resizes a guest window to match its host window's content size. Queued, as
 * above.
 *
 * Arguments:
 *   window_ptr: WindowPtr to resize.
 *   width, height: New content size in guest pixels.
 */
void Toolbox_ResizeGuestWindow(uint32 window_ptr, int16 width, int16 height);

/*
 * Forwards one host input event to the guest.
 *
 * Mouse coordinates are local to the window's content area, with the origin at
 * its top-left corner and y increasing downwards; they are translated to guest
 * global coordinates here. For key events, code is the ADB key code -- which is
 * also what macOS reports as an NSEvent keyCode.
 *
 * Arguments:
 *   window_ptr: WindowPtr the event is aimed at.
 *   kind:       One of the kGuestInput_* values.
 *   x, y:       Content-local coordinates, for mouse events.
 *   code:       Key code, for key events.
 */
void Toolbox_ForwardGuestInput(uint32 window_ptr, int kind, int16 x, int16 y, int code);

/*
 * Moves a guest window so its content lands at the given global position.
 * Queued, as above.
 *
 * Arguments:
 *   window_ptr: WindowPtr to move.
 *   x, y: New global position of the content region's top-left corner.
 */
void Toolbox_MoveGuestWindow(uint32 window_ptr, int16 x, int16 y);

/*
 * Runs the queued window operations.
 *
 * Called from the jGNEFilter stub by way of M68K_EMUL_OP_WINDOW_SAFEPOINT, which
 * puts it in an application's own context with no Toolbox call in progress.
 * Never call it from anywhere else -- calling the Window Manager from the
 * interrupt crashes the guest.
 */
void Toolbox_WindowSafePoint(struct M68kRegisters *r);

/*
 * Requests the jGNEFilter safe point and installs it when it can be. Called
 * from the interrupt drain by whichever clients need a safe context to work in.
 */
void Toolbox_EnableSafePoint(void);

/*
 * Returns true when a screen-resize notification is waiting for the
 * jGNEFilter stub. MenuQueue_Drain uses this to arm the safe point even
 * when toolbox_hooks is off.
 */
bool Toolbox_ScreenResizePending(void);

/*
 * Queues a guest-screen switch for the next jGNEFilter safe point.
 *
 * The safe point prefers Display Manager (DMBeginConfigureDisplays /
 * DMSetDisplayMode / DMEndConfigureDisplays) so InitGDevice, ports,
 * cursor and the desktop are rebuilt the way Monitors does. Without
 * DM it falls back to cscSwitchMode + geometry + _AllocCursor.
 * Safe to call from VideoInterrupt / doevents.
 *
 * Arguments:
 *   width, height: New screen size in pixels.
 */
void Toolbox_NotifyScreenResized(int16 width, int16 height);

/*
 * Presses the frontmost dialog's default button, by way of the Return key.
 */
void Toolbox_PressDialogDefault(void);

/*
 * Asks the guest to close one mirrored window, as if the application had done
 * it itself. Called from the IRQ path after the host close button was clicked.
 *
 * Arguments:
 *   window_ptr: WindowPtr of the window to close.
 */
void Toolbox_DispatchGuestWindowClose(uint32 window_ptr);

/*
 * Records the guest colour table, so mirrored windows are drawn with the same
 * palette as the main screen.
 *
 * Arguments:
 *   entries: 256 RGB triples, one byte per component.
 */
void ToolboxWindow_SetPalette(const uint8 *entries);

/*
 * Returns the recorded colour table, or NULL if none has been seen yet.
 */
const uint8 *ToolboxWindow_GetPalette(void);

/*
 * Drops all mirrored window state. Called on machine reset, where every
 * WindowPtr we are holding has become meaningless.
 */
void ToolboxWindow_Reset(void);

#ifdef __cplusplus
}
#endif

#endif // TOOLBOX_WINDOW_H
