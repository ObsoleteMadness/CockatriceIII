/*
 *  toolbox_traps.cpp - Modular Macintosh Toolbox and OS Trap Dispatcher & Acceleration Architecture
 *
 *  Cockatrice III
 *  (C) 2026 Cockatrice III Project
 *
 *  High-Level Architectural Context:
 *  ==================================
 *  Macintosh software invokes system services via A-line opcodes (0xA000..0xAFFF).
 *  Rather than hardcoding ROM byte patches for every desired feature, this module provides
 *  a modular registry where any subsystem can intercept, monitor, or replace any Toolbox
 *  or OS trap at runtime.
 *
 *  Trampoline Structure (12 bytes per hooked trap):
 *    Offset +0: 0x7130 (M68K_EMUL_OP_TOOLBOX_DISPATCH)
 *    Offset +2: 0x2E49 (move.l a1, a7)  -> Restores caller stack or sets new stack
 *    Offset +4: 0x4ED0 (jmp (a0))       -> Jumps to target (ROM address or caller PC)
 *    Offset +6: 0xXXXX (16-bit Trap Number)
 *    Offset +8: 0xXXXXXXXX (32-bit Original Trap Address)
 */

#include <stdio.h>
#include <string.h>
#include <vector>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "emul_op.h"
#include "main.h"
#include "macos_util.h"
#include "m68k.h"
#include "prefs.h"
#include "toolbox_traps.h"

#define DEBUG 0
#include "debug.h"

// Maximum number of concurrently hooked traps supported in registry
#define MAX_TOOLBOX_TRAPS 128

// Trampoline binary layout constants
#define TRAMPOLINE_SIZE 12
#define OP_MOVE_L_A1_A7 0x2e49
#define OP_JMP_A0       0x4ed0

// Global registry of hooked traps
static ToolboxTrapDesc s_trap_table[MAX_TOOLBOX_TRAPS];
static int s_trap_count = 0;

// Guest memory base address of the allocated trampoline stub pool
static uint32 s_stub_pool_base = 0;
static uint32 s_stub_pool_cursor = 0;

/*
 * Returns true when CockatriceIII_Prefs enables guest trap hooking (toolbox_hooks true).
 */
static bool toolbox_hooks_enabled(void)
{
	return PrefsFindBool("toolbox_hooks");
}
static bool s_traps_installed = false;

// Deferred menu bar sync flag (processed on CPU thread during IRQ)
static volatile bool s_menu_bar_sync_pending = false;

// Guest helper stubs allocated once from the System heap
static uint32 s_menu_key_stub = 0;
static uint32 s_system_menu_stub = 0;

/*
 * Allocates guest RAM for Menu Manager helper stubs if not yet present.
 */
static void ensure_menu_helper_stubs(void)
{
	if (s_menu_key_stub && s_system_menu_stub)
		return;

	M68kRegisters r;
	r.d[0] = 32;
	Execute68kTrap(0xa71e, &r); // NewPtrSysClear()
	uint32 base = r.a[0];
	if (!base)
		return;

	// MenuKey stub: MOVE.W D0,-(A7); _MenuKey; ADDQ.L #4,A7; RTS
	s_menu_key_stub = base;
	WriteMacInt16(s_menu_key_stub + 0, 0x3f00);
	WriteMacInt16(s_menu_key_stub + 2, kTrap_MenuKey);
	WriteMacInt16(s_menu_key_stub + 4, 0x588f);
	WriteMacInt16(s_menu_key_stub + 6, 0x4e75);

	// SystemMenu stub: MOVE.L D0,-(A7); _SystemMenu; ADDQ.L #4,A7; RTS
	s_system_menu_stub = base + 8;
	WriteMacInt16(s_system_menu_stub + 0, 0x2f00);
	WriteMacInt16(s_system_menu_stub + 2, kTrap_SystemMenu);
	WriteMacInt16(s_system_menu_stub + 4, 0x588f);
	WriteMacInt16(s_system_menu_stub + 6, 0x4e75);

	cpu_engine_invalidate_code(base, 16);
}

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
 * Invokes guest _MenuKey with the given command-key character in D0.
 */
static void invoke_menu_key(char cmd_char)
{
	ensure_menu_helper_stubs();
	if (!s_menu_key_stub)
		return;

	M68kRegisters r;
	memset(&r, 0, sizeof(r));
	r.d[0] = (uint32)(uint8)cmd_char;
	Execute68k(s_menu_key_stub, &r);
}

/*
 * Invokes guest _SystemMenu with menuResult = (menuID << 16) | itemIndex.
 * Inside Mac: call after the user (or host) chooses a menu command.
 */
static void invoke_system_menu(int16 menuID, int16 itemIndex)
{
	ensure_menu_helper_stubs();
	if (!s_system_menu_stub)
		return;

	M68kRegisters r;
	memset(&r, 0, sizeof(r));
	r.d[0] = ((uint32)(uint16)menuID << 16) | (uint32)(uint16)itemIndex;
	Execute68k(s_system_menu_stub, &r);
}

/*
 * Activates a guest menu item by menu ID and 1-based item index.
 * Prefers _SystemMenu with the standard menuResult encoding; falls back to
 * _MenuKey when the item has a command-key equivalent and no submenu arrow.
 */
bool Toolbox_DispatchGuestMenuSelect(int16 menuID, int16 itemIndex)
{
	char cmd_char = '\0';
	bool is_submenu = false;
	if (!lookup_menu_item(menuID, itemIndex, &cmd_char, &is_submenu))
		return false;

	if (is_submenu) {
		printf("[TOOLBOX-TRAP] MenuID=%d ItemIndex=%d is a submenu (hMenuCmd); not dispatching\n",
		       (int)menuID, (int)itemIndex);
		fflush(stdout);
		return false;
	}

	// Items with a real command-key can also be driven through MenuKey
	if (cmd_char != '\0' && cmd_char != (char)kMenuNoMark && cmd_char != (char)kMenuHierCmd) {
		printf("[TOOLBOX-TRAP] MenuKey('%c') for MenuID=%d ItemIndex=%d\n",
		       cmd_char, (int)menuID, (int)itemIndex);
		fflush(stdout);
		invoke_menu_key(cmd_char);
		return true;
	}

	printf("[TOOLBOX-TRAP] SystemMenu(0x%08X) MenuID=%d ItemIndex=%d\n",
	       (unsigned)(((uint16)menuID << 16) | (uint16)itemIndex), (int)menuID, (int)itemIndex);
	fflush(stdout);
	invoke_system_menu(menuID, itemIndex);
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

void Toolbox_ProcessPendingMenuBarSync(void)
{
	if (!s_menu_bar_sync_pending)
		return;
	s_menu_bar_sync_pending = false;
	if (s_menu_bar_sync_callback)
		s_menu_bar_sync_callback();
}

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

/*
 * Finds a registered trap entry by its trap number.
 *
 * Arguments:
 *   trap_num: 16-bit A-line trap number.
 *
 * Returns:
 *   Pointer to ToolboxTrapDesc if found, NULL otherwise.
 */
static ToolboxTrapDesc *find_trap_desc(uint16 trap_num)
{
	// Search registry table for matching trap opcode
	for (int i = 0; i < s_trap_count; i++) {
		if (s_trap_table[i].trap_num == trap_num) {
			return &s_trap_table[i];
		}
	}
	return NULL;
}

/*
 * Registers a Toolbox or OS trap hook with the modular dispatcher.
 *
 * Arguments:
 *   trap_num: 16-bit A-Line trap opcode (e.g. 0xA937 for _DrawMenuBar).
 *   name: Human-readable name for logging and diagnostics.
 *   handler: C++ handler callback.
 *   user_data: Optional user context pointer.
 *
 * Returns:
 *   true if registered successfully, false if table is full or invalid arguments.
 */
bool ToolboxTrap_Register(uint16 trap_num, const char *name, ToolboxTrapHandler handler, void *user_data)
{
	if (!toolbox_hooks_enabled())
		return false;

	// Validate inputs
	if (!handler || (trap_num & 0xf000) != 0xa000) {
		printf("[TOOLBOX-TRAP] Error: Invalid trap registration for 0x%04X\n", trap_num);
		return false;
	}

	// Update existing registration if already registered
	ToolboxTrapDesc *desc = find_trap_desc(trap_num);
	if (desc) {
		desc->name = name ? name : "UnknownTrap";
		desc->handler = handler;
		desc->user_data = user_data;
		printf("[TOOLBOX-TRAP] Updated registration for trap 0x%04X (%s)\n", trap_num, desc->name);
		return true;
	}

	// Reject if registry table capacity is reached
	if (s_trap_count >= MAX_TOOLBOX_TRAPS) {
		printf("[TOOLBOX-TRAP] Error: Trap registry table full (max %d)\n", MAX_TOOLBOX_TRAPS);
		return false;
	}

	// Append new registration entry
	desc = &s_trap_table[s_trap_count++];
	desc->trap_num = trap_num;
	desc->name = name ? name : "UnknownTrap";
	desc->handler = handler;
	desc->user_data = user_data;
	desc->stub_addr = 0;
	desc->original_addr = 0;
	desc->is_installed = false;

	printf("[TOOLBOX-TRAP] Registered trap 0x%04X (%s)\n", trap_num, desc->name);
	fflush(stdout);

	return true;
}

/*
 * Unregisters a previously registered trap hook.
 *
 * Arguments:
 *   trap_num: 16-bit trap opcode.
 *
 * Returns:
 *   true if unregistered, false if not found.
 */
bool ToolboxTrap_Unregister(uint16 trap_num)
{
	// Search for matching trap index
	for (int i = 0; i < s_trap_count; i++) {
		if (s_trap_table[i].trap_num == trap_num) {
			// Restore original trap address if currently installed
			if (s_trap_table[i].is_installed && s_trap_table[i].original_addr) {
				M68kRegisters r;
				r.d[0] = s_trap_table[i].trap_num;
				r.a[0] = s_trap_table[i].original_addr;
				Execute68kTrap((s_trap_table[i].trap_num & 0x0800) ? 0xa647 : 0xa247, &r);
			}
			// Shift remaining entries forward
			for (int j = i; j < s_trap_count - 1; j++) {
				s_trap_table[j] = s_trap_table[j + 1];
			}
			s_trap_count--;
			printf("[TOOLBOX-TRAP] Unregistered trap 0x%04X\n", trap_num);
			return true;
		}
	}
	return false;
}

/*
 * Allocates guest system memory for the trap trampoline pool and installs all registered hooks.
 * Called during boot from InstallDrivers() or PatchAfterStartup().
 */
void ToolboxTrap_InstallAll(void)
{
	if (!toolbox_hooks_enabled())
		return;

	// Exit early if no traps registered or already installed
	if (s_trap_count == 0)
		return;

	printf("[TOOLBOX-TRAP] Installing %d registered trap hooks...\n", s_trap_count);
	fflush(stdout);

	// Allocate non-relocatable buffer in System Heap using NewPtrSysClear (0xA71E) if not yet allocated
	if (!s_stub_pool_base) {
		uint32 pool_size = MAX_TOOLBOX_TRAPS * TRAMPOLINE_SIZE;
		M68kRegisters r;
		r.d[0] = pool_size;
		Execute68kTrap(0xa71e, &r); // NewPtrSysClear()
		s_stub_pool_base = r.a[0];
		s_stub_pool_cursor = s_stub_pool_base;

		if (!s_stub_pool_base) {
			printf("[TOOLBOX-TRAP] FATAL: Failed to allocate guest stub pool memory!\n");
			return;
		}
		printf("[TOOLBOX-TRAP] Allocated guest trampoline pool at 0x%08X (size %u bytes)\n",
		       s_stub_pool_base, pool_size);
	}

	// Install trampolines for each registered trap hook
	for (int i = 0; i < s_trap_count; i++) {
		ToolboxTrapDesc &desc = s_trap_table[i];
		if (desc.is_installed)
			continue;

		// 1. Fetch current/original trap address from Mac OS trap table
		M68kRegisters r_get;
		r_get.d[0] = desc.trap_num;
		// Use _GetToolTrapAddress (0xA746) for Toolbox traps, _GetOSTrapAddress (0xA346) for OS traps
		Execute68kTrap((desc.trap_num & 0x0800) ? 0xa746 : 0xa346, &r_get);
		desc.original_addr = r_get.a[0];

		// 2. Assign trampoline memory location
		desc.stub_addr = s_stub_pool_cursor;
		s_stub_pool_cursor += TRAMPOLINE_SIZE;

		// 3. Write 12-byte trampoline binary stub into guest RAM:
		//    [0..1]: M68K_EMUL_OP_TOOLBOX_DISPATCH (0x7130)
		//    [2..3]: move.l a1, a7 (0x2E49)
		//    [4..5]: jmp (a0) (0x4ED0)
		//    [6..7]: 16-bit Trap Opcode
		//    [8..11]: 32-bit Original Trap Address
		WriteMacInt16(desc.stub_addr + 0, (uint16)M68K_EMUL_OP_TOOLBOX_DISPATCH);
		WriteMacInt16(desc.stub_addr + 2, (uint16)OP_MOVE_L_A1_A7);
		WriteMacInt16(desc.stub_addr + 4, (uint16)OP_JMP_A0);
		WriteMacInt16(desc.stub_addr + 6, desc.trap_num);
		WriteMacInt32(desc.stub_addr + 8, desc.original_addr);

		// Invalidate JIT translation cache across the written stub
		cpu_engine_invalidate_code(desc.stub_addr, TRAMPOLINE_SIZE);

		// 4. Update Mac OS trap table entry to point to our trampoline stub
		M68kRegisters r_set;
		r_set.d[0] = desc.trap_num;
		r_set.a[0] = desc.stub_addr;
		// Use _SetToolTrap (0xA647) for Toolbox traps, _SetOSTrapAddress (0xA247) for OS traps
		Execute68kTrap((desc.trap_num & 0x0800) ? 0xa647 : 0xa247, &r_set);

		desc.is_installed = true;
		printf("[TOOLBOX-TRAP] Hooked 0x%04X (%s) -> stub 0x%08X (orig 0x%08X)\n",
		       desc.trap_num, desc.name, desc.stub_addr, desc.original_addr);
	}

	s_traps_installed = true;
	fflush(stdout);
}

/*
 * Central dispatcher invoked from EmulOp() when M68K_EMUL_OP_TOOLBOX_DISPATCH (0x7130) executes.
 *
 * Arguments:
 *   r: Pointer to active 68k register state.
 */
void ToolboxTrap_Dispatch(struct M68kRegisters *r)
{
	// In the CPU core, PC has advanced 2 bytes past the 0x7130 opcode, pointing to stub + 2
	uint32 pc = (uint32)m68k_get_reg(NULL, M68K_REG_PC);
	uint32 stub_addr = pc - 2;

	// Extract metadata stored in trampoline footer
	uint16 trap_num = (uint16)ReadMacInt16(stub_addr + 6);
	uint32 original_addr = ReadMacInt32(stub_addr + 8);

	// Initialize default passthrough registers: A0 = original ROM address, A1 = current SP
	r->a[0] = original_addr;
	r->a[1] = r->a[7];

	// Lookup registered trap descriptor
	ToolboxTrapDesc *desc = find_trap_desc(trap_num);
	if (!desc || !desc->handler) {
		// No custom handler registered: default to executing original ROM implementation
		return;
	}

	// Invoke registered C++ handler callback
	ToolboxAction action = desc->handler(trap_num, r, original_addr, desc->user_data);

	if (action == TOOLBOX_ACTION_PASSTHROUGH) {
		// Passthrough mode: ensure A0 points to original ROM routine and A1 points to caller stack
		r->a[0] = original_addr;
		r->a[1] = r->a[7];
	} else if (action == TOOLBOX_ACTION_REPLACE) {
		// Replace mode: handler has already set A0 to caller return PC and A1 to new stack pointer
	}
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
