/*
 *  toolbox_menu.cpp - Menu Manager client of the Toolbox trap registry
 *
 *  Cockatrice III
 *  (C) 2026 Cockatrice III Project
 *
 *  High-Level Architectural Context:
 *  ==================================
 *  toolbox_traps.cpp owns the *mechanism*: a registry of hooked A-line traps,
 *  one RAM trampoline per hook installed through _SetToolTrap /
 *  _SetOSTrapAddress, and the dispatcher that EmulOp() enters. It knows nothing
 *  about any particular trap.
 *
 *  This file is the first *client* of that mechanism, and is meant to be read
 *  as the worked example of one. Everything a subsystem needs to bring is here
 *  and nowhere else:
 *
 *    1. A handler with the ToolboxTrapHandler signature -- Handle_MenuStateChange()
 *       below, which only flags work and returns TOOLBOX_ACTION_PASSTHROUGH so the
 *       ROM Menu Manager still runs.
 *    2. A registration entry point -- ToolboxMenu_RegisterTraps() -- that names the
 *       traps it cares about. Registration is portable; nothing here is Cocoa,
 *       SDL or Win32.
 *    3. Whatever guest-state decoding the subsystem needs, kept on this side of
 *       the boundary: Toolbox_SnapshotMenuBar() walks MenuList and the MenuInfo
 *       records, and the guest helper stubs drive _MenuKey / _SystemMenu.
 *
 *  Host UI code supplies exactly one thing: a sync callback, via
 *  Toolbox_SetMenuBarSyncCallback(). BasiliskII/bridge/darwin/macos_menu_bridge.mm
 *  is the macOS implementation of that callback and now contains no trap
 *  registration of its own.
 *
 *  Threading:
 *  ----------
 *  Handlers run on the CPU thread inside the trap, where the ROM's own Menu
 *  Manager state is mid-update. So a hook never snapshots directly: it sets the
 *  deferred flag and Toolbox_ProcessPendingMenuBarSync() -- called from the IRQ
 *  path in SDL/menu_bar.cpp -- does the reading once the trap has completed.
 */

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "emul_op.h"
#include "main.h"
#include "macos_util.h"
#include "prefs.h"
#include "toolbox_traps.h"
#include "toolbox_menu.h"

#define DEBUG 0
#include "debug.h"

/*
 * Low Memory Global Offsets for Menu Manager
 */
enum {
	LM_MenuList    = 0x0a1c, // Handle to current MenuList record
	LM_MBarEnable  = 0x0a20, // Menu bar enable flags (0 = app owns menu bar)
	LM_TheMenu     = 0x0a26, // Menu ID of highlighted menu in menu bar
	LM_TopMenuItem = 0x0a0a, // Pixel value at top of scrollable menu
	LM_MBarHook    = 0x0a3c, // Menu bar drawing hook procedure
	LM_MenuHook    = 0x0a30, // Menu selection hook (MenuSelect while button down)
	LM_MBarHeight  = 0x0baa  // Current menu bar height in pixels
};

// Deferred menu bar sync flag (processed on CPU thread during IRQ)
static volatile bool s_menu_bar_sync_pending = false;

/*
 * Verifies that a menu ID and 1-based item index exist in the guest MenuList.
 * Optionally returns the item command-key character and submenu flag.
 *
 * Returns:
 *   true if the menu/item exists in guest memory.
 */
static bool lookup_menu_item(int16 menuID, int16 itemIndex, char *cmd_char_out, bool *is_submenu_out)
{
	if (menuID <= 0 || itemIndex <= 0)
		return false;

	if (cmd_char_out)
		*cmd_char_out = '\0';
	if (is_submenu_out)
		*is_submenu_out = false;

	MacMenuBarSnapshot snapshot;
	if (!Toolbox_SnapshotMenuBar(snapshot))
		return false;

	for (size_t m = 0; m < snapshot.menus.size(); m++) {
		const MacMenuSnapshot &menu = snapshot.menus[m];
		if (menu.menuID != menuID)
			continue;

		for (size_t i = 0; i < menu.items.size(); i++) {
			const MacMenuItemSnapshot &item = menu.items[i];
			if (item.itemIndex == itemIndex) {
				if (cmd_char_out)
					*cmd_char_out = item.cmdChar;
				if (is_submenu_out)
					*is_submenu_out = item.isSubmenu;
				return true;
			}
		}
		// Menu exists but item index was not found
		return false;
	}

	return false;
}

/*
 * Making the guest act on a menu choice made on the host.
 *
 * There is no Toolbox call that performs a menu command. _MenuSelect and
 * _MenuKey only *report* which item the user picked; it is the application that
 * acts on the answer, in its own event loop. So a host menu choice takes effect
 * by getting the application to ask the question, and then answering it.
 *
 * Answering is a REPLACE hook on _MenuSelect, which hands back the menuResult
 * chosen on the host instead of tracking the mouse. Asking is a mouseDown in
 * the menu bar, which is what every Mac event loop responds to by calling
 * _MenuSelect -- and that event is written straight into the event record the
 * Event Manager is about to return, from the jGNEFilter safe point. The
 * application then dispatches the choice exactly as it would a real selection,
 * its own _HiliteMenu(0) included.
 *
 * Nothing about the mouse is touched. An earlier version injected a real click
 * through ADB, which moved the guest pointer to the menu bar and left it there.
 * Before that it called _MenuKey or _SystemMenu directly, which could not work
 * at all: those ran from the 60 Hz interrupt, where Execute68k must not be
 * used, and their return value went nowhere -- nothing in the guest was waiting
 * for it. (_SystemMenu is for desk accessory menus in any case.)
 */

// menuResult, (menuID << 16) | item, waiting for the next _MenuSelect; -1 if none
static int32 s_pending_menu_result = -1;

// Ticks left before the pending result is abandoned. An application that never
// asks must not leave a stale answer for a later real selection.
static int s_pending_menu_timeout = 0;

// Whether the mouseDown that prompts the question has been handed over yet
static bool s_menu_event_posted = false;

enum {
	kMenuEventH = 20,        // inside the Apple menu's title, safely on the bar
	kMenuEventV = 8,
	kMenuResultTimeout = 300 // five seconds at 60 Hz
};

// EventRecord: what, message, when, where (v then h), modifiers
enum {
	kEvt_What = 0, kEvt_Message = 2, kEvt_When = 6,
	kEvt_WhereV = 10, kEvt_WhereH = 12, kEvt_Modifiers = 14
};
enum { kEvent_Null = 0, kEvent_MouseDown = 1 };
enum { LM_Ticks = 0x016a };

/*
 * Answers a _MenuSelect with the choice made on the host.
 *
 * FUNCTION MenuSelect(startPt: Point): LongInt -- four bytes of argument, a
 * four byte result, Pascal convention.
 */
static ToolboxAction Handle_MenuSelect(uint16 trap_num, M68kRegisters *r,
                                       uint32 original_addr, void *user_data)
{
	(void)trap_num; (void)original_addr; (void)user_data;

	// No host selection outstanding: this is the user working the guest's own
	// menu bar, which must behave exactly as it always did
	if (s_pending_menu_result < 0)
		return TOOLBOX_ACTION_PASSTHROUGH;

	int32 result = s_pending_menu_result;
	s_pending_menu_result = -1;
	s_pending_menu_timeout = 0;
	s_menu_event_posted = false;

	printf("[TOOLBOX-MENU] MenuSelect answered with 0x%08X (menu %d, item %d)\n",
	       (unsigned)result, (int)(int16)(result >> 16), (int)(int16)(result & 0xffff));
	fflush(stdout);

	ToolboxArgs args(r);
	args.SetResultInt32(4, (uint32)result);
	return args.Return(4);
}

/*
 * Hands the front application the menu-bar mouseDown that makes it ask.
 *
 * Called from the jGNEFilter safe point with the event record GetNextEvent is
 * about to return and the address of the Boolean result it will hand back --
 * ToolboxEventMgr.a:265-280 documents both as part of the filter's entry
 * conditions. Only a null event is overwritten, so no real event is ever lost;
 * null events are frequent enough that the wait is a frame or two.
 *
 * Args:
 *   event_record: guest EventRecord the Event Manager is about to return.
 *   result_addr: guest address of the Boolean word GetNextEvent returns.
 *
 * Returns:
 *   true if an event was placed.
 */
bool Toolbox_MenuSafePoint(uint32 event_record, uint32 result_addr)
{
	if (s_pending_menu_result < 0 || s_menu_event_posted)
		return false;
	if (event_record < 0x1000 || event_record >= RAMSize)
		return false;
	if (result_addr < 0x1000 || result_addr >= RAMSize)
		return false;

	// Never displace a real event
	if (ReadMacInt16(event_record + kEvt_What) != kEvent_Null)
		return false;

	/*
	 * Only the application that owns this menu may be asked about it. Every
	 * process calls GetNextEvent, background ones included, and the Process
	 * Manager swaps MenuList per process, so the list visible here identifies
	 * whose event loop we are standing in. Feeding the Finder a menuResult for
	 * an ID it does not have would have it dispatch some unrelated item of its
	 * own -- or none, and swallow the selection either way.
	 */
	int16 menu_id = (int16)(s_pending_menu_result >> 16);
	int16 item_index = (int16)(s_pending_menu_result & 0xffff);
	if (!lookup_menu_item(menu_id, item_index, NULL, NULL))
		return false;

	WriteMacInt16(event_record + kEvt_What, kEvent_MouseDown);
	WriteMacInt32(event_record + kEvt_Message, 0);
	WriteMacInt32(event_record + kEvt_When, ReadMacInt32(LM_Ticks));
	WriteMacInt16(event_record + kEvt_WhereV, kMenuEventV);
	WriteMacInt16(event_record + kEvt_WhereH, kMenuEventH);
	WriteMacInt16(event_record + kEvt_Modifiers, 0);

	/*
	 * GetNextEvent's Boolean result occupies a word on the stack with the value
	 * in its *high* byte: GNECommon builds it with CLR.W followed by
	 * ADDQ.B #1 at the same address (ToolboxEventMgr.a), and WaitNextEvent
	 * reads it back with MOVE.B (SP)+ (WaitNextEvent.a:56). Writing a word of 1
	 * sets the low byte, which every caller reads as false -- the event was
	 * being filled in and then thrown away.
	 */
	WriteMacInt8(result_addr, 1); // GetNextEvent returns true

	s_menu_event_posted = true;
	printf("[TOOLBOX-MENU] handed the front application a menu bar mouseDown for "
	       "menuResult 0x%08X\n", (unsigned)s_pending_menu_result);
	fflush(stdout);
	return true;
}

/*
 * Expires a selection nothing asked about. Called once per 60 Hz interrupt.
 */
static void tick_pending_menu(void)
{
	if (s_pending_menu_timeout > 0 && --s_pending_menu_timeout == 0 &&
	    s_pending_menu_result >= 0) {
		printf("[TOOLBOX-MENU] no application asked for menuResult 0x%08X; discarded "
		       "(mouseDown %s)\n", (unsigned)s_pending_menu_result,
		       s_menu_event_posted ? "was handed over" : "never placed");
		fflush(stdout);
		s_pending_menu_result = -1;
		s_menu_event_posted = false;
	}
}

/*
 * Activates a guest menu item by menu ID and 1-based item index.
 *
 * Arranges for the next _MenuSelect to return this item, and for the front
 * application to make that call.
 */
bool Toolbox_DispatchGuestMenuSelect(int16 menuID, int16 itemIndex)
{
	if (!ToolboxTrap_HooksEnabled())
		return false;

	char cmd_char = '\0';
	bool is_submenu = false;
	if (!lookup_menu_item(menuID, itemIndex, &cmd_char, &is_submenu))
		return false;

	// A submenu title is not a command; picking it should open the submenu,
	// which on the host has already happened
	if (is_submenu) {
		printf("[TOOLBOX-MENU] MenuID=%d ItemIndex=%d is a submenu; nothing to dispatch\n",
		       (int)menuID, (int)itemIndex);
		fflush(stdout);
		return false;
	}

	s_pending_menu_result = ((int32)(uint16)menuID << 16) | (uint16)itemIndex;
	s_pending_menu_timeout = kMenuResultTimeout;
	s_menu_event_posted = false;

	printf("[TOOLBOX-MENU] queued menuResult 0x%08X (menu %d, item %d)\n",
	       (unsigned)s_pending_menu_result, (int)menuID, (int)itemIndex);
	fflush(stdout);
	return true;
}

/*
 * Schedules a deferred guest-to-host menu bar sync (processed on the next IRQ).
 */
void Toolbox_RequestMenuBarSync(void)
{
	s_menu_bar_sync_pending = true;
}

/*
 * Runs a pending menu bar sync if one was requested. Platform code registers the callback.
 */
static void (*s_menu_bar_sync_callback)(void) = NULL;

void Toolbox_SetMenuBarSyncCallback(void (*callback)(void))
{
	s_menu_bar_sync_callback = callback;
}

/*
 * Notices the menu bar being replaced without a trap being called.
 *
 * The hooked Menu Manager traps catch an application building its own menu bar,
 * but not the Process Manager handing the menu bar over when the front
 * application changes: a context switch swaps the low-memory globals directly,
 * MenuList among them (ProcessMgr/LomemTab.Color.a). Without this the host menu
 * bar keeps showing whichever application last called _SetMenuBar -- in
 * practice the Finder, for the whole session.
 *
 * So MenuList is fingerprinted on every pass and a sync requested when it moves.
 * That is a handful of reads at 60 Hz, and it makes the switch automatic rather
 * than dependent on the incoming application happening to rebuild its menus.
 */
static void poll_menu_list(void)
{
	uint32 handle = ReadMacInt32(LM_MenuList);
	uint32 fingerprint = handle;

	if (handle >= 0x1000 && handle < RAMSize) {
		uint32 list_ptr = ReadMacInt32(handle);
		if (list_ptr >= 0x1000 && list_ptr < RAMSize) {
			int16 total_bytes = (int16)ReadMacInt16(list_ptr);
			fingerprint = fingerprint * 31 + list_ptr;
			fingerprint = fingerprint * 31 + (uint16)total_bytes;

			// The menu handles themselves: two applications can be handed menu
			// lists of the same length at the same address
			if (total_bytes > 6 && total_bytes <= 4096) {
				for (int16 offset = 6; offset < total_bytes; offset += 6)
					fingerprint = fingerprint * 31 + ReadMacInt32(list_ptr + offset);
			}
		}
	}

	static uint32 last_fingerprint = 0;
	static bool have_last = false;
	if (have_last && fingerprint == last_fingerprint)
		return;

	// The first observation is a baseline, not a change. Asking for a sync from
	// it would run the whole decode on the first interrupt after boot, when
	// MenuList still holds whatever was in that longword beforehand.
	bool first = !have_last;
	have_last = true;
	last_fingerprint = fingerprint;
	if (!first)
		s_menu_bar_sync_pending = true;
}

void Toolbox_ProcessPendingMenuBarSync(void)
{
	// Polling MenuList is not a trap hook, so it needs the master switch checked
	// explicitly; with hooking off the host menu bar stays the emulator's own
	if (!ToolboxTrap_HooksEnabled())
		return;

	tick_pending_menu();
	poll_menu_list();

	if (!s_menu_bar_sync_pending)
		return;
	s_menu_bar_sync_pending = false;

	/*
	 * Report once that the whole path works: a hooked trap was entered, the
	 * dispatcher identified it, the deferred flag came back here, and the guest
	 * menu bar decodes. Every stage of a *working* hook is silent by design, so
	 * without this the log can show ten traps hooked and never say whether any
	 * of them fires.
	 *
	 * Only a decode that produced menus retires the announcement: the early
	 * syncs legitimately arrive before the Menu Manager has any menus, and that
	 * case is worth one line (with the list it found) rather than silence.
	 */
	static bool announced = false;
	static bool announced_empty = false;
	if (!announced) {
		MacMenuBarSnapshot snapshot;
		if (Toolbox_SnapshotMenuBar(snapshot)) {
			announced = true;
			printf("[TOOLBOX-MENU] hooks are live; guest menu bar:");
			for (size_t i = 0; i < snapshot.menus.size(); i++)
				printf(" %s", snapshot.menus[i].title.c_str());
			printf("\n");
			fflush(stdout);
		} else if (!announced_empty) {
			announced_empty = true;
			uint32 handle = ReadMacInt32(LM_MenuList);
			// Bounds-check before dereferencing: a sync can be requested before
			// the Menu Manager has ever run, and this longword is then whatever
			// the ROM left in it
			uint32 list_ptr = (handle >= 0x1000 && handle < RAMSize)
				? ReadMacInt32(handle) : 0;
			printf("[TOOLBOX-MENU] hooks are live; MenuList ($0A1C) = 0x%08X -> 0x%08X holds no menus yet\n",
			       (unsigned)handle, (unsigned)list_ptr);
			fflush(stdout);
		}
	}

	if (s_menu_bar_sync_callback)
		s_menu_bar_sync_callback();
}

/*
 * Decodes the guest Mac OS MenuList global (0x0A1C) and all MenuInfo records from guest RAM.
 *
 * Arguments:
 *   snapshot_out: Reference to snapshot structure to populate.
 *
 * Returns:
 *   true if MenuList was valid and decoded, false otherwise.
 */
bool Toolbox_SnapshotMenuBar(MacMenuBarSnapshot &snapshot_out)
{
	snapshot_out.menus.clear();

	if (!ToolboxTrap_HooksEnabled())
		return false;

	// Read MenuList handle from Low Memory global (0x0A1C)
	uint32 menu_list_handle = ReadMacInt32(LM_MenuList);
	if (!menu_list_handle)
		return false;

	// Validate handle against guest RAM boundaries
	if (menu_list_handle < 0x1000 || menu_list_handle >= RAMSize)
		return false;

	// Dereference MenuList handle to get master pointer
	uint32 list_ptr = ReadMacInt32(menu_list_handle);
	if (!list_ptr || list_ptr < 0x1000 || list_ptr >= RAMSize)
		return false;

	// Read total length in bytes of menu list table
	int16 total_bytes = (int16)ReadMacInt16(list_ptr);
	if (total_bytes <= 6 || total_bytes > 4096)
		return false;

	// Iterate over 6-byte menu list entries starting at offset 6
	// Format: [0..3]: MenuHandle, [4..5]: leftEdge coordinate
	for (int16 offset = 6; offset < total_bytes; offset += 6) {
		uint32 menu_handle = ReadMacInt32(list_ptr + offset);
		if (!menu_handle || menu_handle < 0x1000 || menu_handle >= RAMSize)
			continue;

		// Dereference MenuHandle to get MenuInfo record
		uint32 menu_info_ptr = ReadMacInt32(menu_handle);
		if (!menu_info_ptr || menu_info_ptr < 0x1000 || menu_info_ptr >= RAMSize)
			continue;

		MacMenuSnapshot menu;
		menu.menuID = (int16)ReadMacInt16(menu_info_ptr + 0);
		uint32 enable_flags = ReadMacInt32(menu_info_ptr + 10);
		// Bit 0 of enableFlags indicates whether the entire menu is enabled
		menu.isEnabled = (enable_flags & 1) != 0;

		// Read Menu Title Pascal string at menu_info_ptr + 14
		uint32 title_addr = menu_info_ptr + 14;
		std::string raw_title = ToolboxArgs::ReadPascalString(title_addr);
		uint8 title_len = (uint8)ReadMacInt8(title_addr);

		// Handle classic Apple symbol (char code 0x14)
		if (raw_title.length() == 1 && (uint8)raw_title[0] == 0x14) {
			menu.title = "\xEF\xA3\xBF"; // UTF-8 Apple Logo  (U+F8FF)
		} else {
			menu.title = raw_title;
		}

		// Item definitions start immediately after the menu title Pascal string
		uint32 item_cursor = title_addr + 1 + title_len;
		int16 item_index = 1;

		// Parse variable-length item records until terminating null length byte
		while (item_cursor < menu_info_ptr + 4096) {
			uint8 item_text_len = (uint8)ReadMacInt8(item_cursor);
			// Length byte of 0 indicates the end of the item definition list
			if (item_text_len == 0)
				break;

			std::string item_text;
			item_text.reserve(item_text_len);
			for (uint32 k = 0; k < item_text_len; k++) {
				item_text.push_back((char)ReadMacInt8(item_cursor + 1 + k));
			}

			// Advance cursor past text string
			uint32 meta_cursor = item_cursor + 1 + item_text_len;
			// uint8 icon_num = (uint8)ReadMacInt8(meta_cursor + 0);
			char cmd_char = (char)ReadMacInt8(meta_cursor + 1);
			uint8 mark_char = (uint8)ReadMacInt8(meta_cursor + 2);
			// uint8 item_style = (uint8)ReadMacInt8(meta_cursor + 3);

			MacMenuItemSnapshot item;
			item.text = item_text;
			item.cmdChar = cmd_char;
			item.markChar = mark_char;
			item.isSeparator = (item_text == "-");
			item.isSubmenu = ((uint8)cmd_char == (uint8)kMenuHierCmd);
			item.menuID = menu.menuID;
			item.itemIndex = item_index;

			// Check if this item is enabled: bit (itemIndex) in enableFlags
			if (item_index <= 31) {
				item.isEnabled = menu.isEnabled && ((enable_flags & (1 << item_index)) != 0);
			} else {
				item.isEnabled = menu.isEnabled;
			}

			menu.items.push_back(item);

			// Each item metadata block is 4 bytes (icon, cmd, mark, style)
			item_cursor = meta_cursor + 4;
			item_index++;
		}

		snapshot_out.menus.push_back(menu);
	}

	return !snapshot_out.menus.empty();
}

/*
 * Trap hook for every Menu Manager call that can change what the menu bar
 * shows.
 *
 * Runs on the CPU thread with the ROM routine not yet executed, so the guest
 * MenuList still holds the *old* state: reading it here would snapshot the menu
 * bar as it was before the call. The hook therefore only raises the deferred
 * flag and passes through, leaving the ROM to do its work and
 * Toolbox_ProcessPendingMenuBarSync() to read the result on the next interrupt.
 *
 * Arguments and return value: see ToolboxTrapHandler in toolbox_traps.h.
 */
static TOOLBOX_TRAP_HANDLER(Handle_MenuStateChange)
{
	// Defer the sync until after the ROM Menu Manager trap completes (next IRQ)
	Toolbox_RequestMenuBarSync();

	// Passthrough: allow the original Mac OS ROM code to complete internal updates
	return TOOLBOX_ACTION_PASSTHROUGH;
}

/*
 * Registers the Menu Manager trap hooks with the Toolbox trap registry.
 *
 * Call once during host UI setup, after the platform has installed its sync
 * callback with Toolbox_SetMenuBarSyncCallback(). Registration only records the
 * hooks; ToolboxTrap_InstallAll() is what writes the trampolines into guest RAM
 * later, once there is a Mac heap to allocate them from.
 *
 * ToolboxTrap_Register() itself refuses while the toolbox_hooks pref is off;
 * the same check is made here so the reason is visible in the log rather than
 * appearing as ten silent failures.
 */
void ToolboxMenu_RegisterTraps(void)
{
	if (!ToolboxTrap_HooksEnabled()) {
		printf("[TOOLBOX-MENU] toolbox_hooks disabled; skipping Menu Manager trap registration.\n");
		fflush(stdout);
		return;
	}

	// Hook the core Menu Manager traps that alter menu bar content or display
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

	// Not a state-change hook: this one answers, so that a menu item chosen
	// on the host is dispatched by the guest application itself
	ToolboxTrap_Register(0xa93d, "_MenuSelect",     Handle_MenuSelect, NULL);

	printf("[TOOLBOX-MENU] Registered Menu Manager trap hooks for host menu bar sync.\n");
	fflush(stdout);
}
