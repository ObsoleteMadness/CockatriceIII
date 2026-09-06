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
 *  This file is the mechanism only -- it knows no trap by name. A subsystem that
 *  wants a trap brings a handler and a registration call of its own;
 *  toolbox_menu.cpp is the worked example of one, and the Menu Manager code that
 *  used to live here is now in it.
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

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "emul_op.h"
#include "main.h"
#include "macos_util.h"
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
 *
 * This is the master switch for every client of the registry, not just for
 * ToolboxTrap_Register(): with it off, nothing here or in any client may touch
 * guest state -- no trampolines, no jGNEFilter stub, no MenuList polling, no
 * window walk. The result must be a boot that differs from an unpatched one
 * only in what it prints.
 *
 * Asked once per 60 Hz interrupt by the periodic clients, which is cheap enough
 * not to be worth caching -- and caching would quietly ignore a pref set after
 * the first call, which the tests do.
 */
bool ToolboxTrap_HooksEnabled(void)
{
	return PrefsFindBool("toolbox_hooks");
}

static bool toolbox_hooks_enabled(void)
{
	return ToolboxTrap_HooksEnabled();
}
static bool s_traps_installed = false;

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
 * Points the registry at a caller-supplied trampoline pool.
 *
 * ToolboxTrap_InstallAll() normally gets the pool from NewPtrSysClear, which
 * needs a live Mac heap. Separating "where the pool is" from "how it was
 * obtained" lets the dispatcher be exercised offline -- see
 * BasiliskII/tests/basilisk/basilisk_toolbox_test.cpp, which is the only thing
 * that checks stub identification on all three CPU engines.
 *
 * Arguments:
 *   base: Macintosh address of at least MAX_TOOLBOX_TRAPS * TRAMPOLINE_SIZE
 *         bytes of guest RAM, or 0 to forget the current pool.
 */
void ToolboxTrap_SetStubPool(uint32 base)
{
	s_stub_pool_base = base;
	s_stub_pool_cursor = base;
}

/*
 * Writes one 12-byte trampoline into guest RAM and invalidates the JIT cache
 * across it.
 *
 * Layout, which stub_addr_from_pc() and ToolboxTrap_Dispatch() both depend on:
 *   [0..1]  M68K_EMUL_OP_TOOLBOX_DISPATCH -- must be first, so any PC the
 *           engine reports still falls inside this slot
 *   [2..3]  move.l a1,a7   (stack the handler chose)
 *   [4..5]  jmp (a0)       (ROM routine, or the caller for a replacement)
 *   [6..7]  trap number
 *   [8..11] original trap address
 *
 * Arguments:
 *   addr:          slot address inside the pool.
 *   trap_num:      A-line trap this trampoline stands in for.
 *   original_addr: address the trap resolved to before hooking.
 */
void ToolboxTrap_WriteTrampoline(uint32 addr, uint16 trap_num, uint32 original_addr)
{
	WriteMacInt16(addr + 0, (uint16)M68K_EMUL_OP_TOOLBOX_DISPATCH);
	WriteMacInt16(addr + 2, (uint16)OP_MOVE_L_A1_A7);
	WriteMacInt16(addr + 4, (uint16)OP_JMP_A0);
	WriteMacInt16(addr + 6, trap_num);
	WriteMacInt32(addr + 8, original_addr);
	cpu_engine_invalidate_code(addr, TRAMPOLINE_SIZE);

	/*
	 * Keep the invariant stub_addr_from_pc() relies on: the cursor is one past
	 * the highest slot ever written. alloc_stub_slot() has usually moved it
	 * there already, so this only matters when a caller places a trampoline
	 * itself.
	 */
	if (addr + TRAMPOLINE_SIZE > s_stub_pool_cursor)
		s_stub_pool_cursor = addr + TRAMPOLINE_SIZE;
}

/*
 * Hands out the next free slot in the trampoline pool.
 *
 * Returns:
 *   Slot address, or 0 if the pool is exhausted. The pool is sized for
 *   MAX_TOOLBOX_TRAPS and ToolboxTrap_Register() refuses beyond that, so
 *   exhaustion means the pool was set smaller than the module expects.
 */
static uint32 alloc_stub_slot(void)
{
	uint32 limit = s_stub_pool_base + MAX_TOOLBOX_TRAPS * TRAMPOLINE_SIZE;
	if (!s_stub_pool_base || s_stub_pool_cursor + TRAMPOLINE_SIZE > limit)
		return 0;
	uint32 slot = s_stub_pool_cursor;
	s_stub_pool_cursor += TRAMPOLINE_SIZE;
	return slot;
}

/*
 * Reads a trap's current entry out of the Mac OS trap table.
 *
 * Arguments:
 *   trap_num: A-line trap number; bit 11 selects the Toolbox table.
 *
 * Returns:
 *   Address the trap currently dispatches to.
 */
static uint32 get_trap_address(uint16 trap_num)
{
	M68kRegisters r;
	memset(&r, 0, sizeof(r));
	r.d[0] = trap_num;
	// _GetToolTrapAddress (0xA746) for Toolbox traps, _GetOSTrapAddress (0xA346) for OS traps
	Execute68kTrap((trap_num & 0x0800) ? 0xa746 : 0xa346, &r);
	return r.a[0];
}

/*
 * Reports whether the trap table still dispatches through the trampolines this
 * module installed.
 *
 * Used to tell a second boot from a second call inside one boot; see the
 * comment in ToolboxTrap_InstallAll().
 *
 * Returns:
 *   true if a trap we installed still points at our own stub.
 */
static bool trap_table_still_ours(void)
{
	for (int i = 0; i < s_trap_count; i++) {
		const ToolboxTrapDesc &desc = s_trap_table[i];
		if (!desc.is_installed || !desc.stub_addr)
			continue;
		if (get_trap_address(desc.trap_num) != desc.stub_addr)
			continue;
		/*
		 * The address matching is not enough on its own: a rebuilt System heap
		 * can hand the same address to something else. The trampoline's own
		 * contents settle it -- if the EmulOp word and the trap number are
		 * still there, this really is our stub.
		 */
		if (ReadMacInt16(desc.stub_addr) == (uint16)M68K_EMUL_OP_TOOLBOX_DISPATCH &&
		    ReadMacInt16(desc.stub_addr + 6) == desc.trap_num)
			return true;
	}
	return false;
}

/*
 * Drops every trace of a previous boot's installation.
 *
 * The pool pointer and the stub addresses describe a System heap that no longer
 * exists, so they are cleared rather than reused; the registrations themselves
 * (trap number, name, handler, user_data) survive, because those came from the
 * host side and are not affected by the guest resetting.
 */
static void forget_previous_installation(void)
{
	ToolboxTrap_SetStubPool(0);
	for (int i = 0; i < s_trap_count; i++) {
		s_trap_table[i].stub_addr = 0;
		s_trap_table[i].original_addr = 0;
		s_trap_table[i].is_installed = false;
	}
	s_traps_installed = false;
}

/*
 * Allocates guest system memory for the trap trampoline pool and installs all
 * registered hooks.
 *
 * Called once per boot from PatchAfterStartup(), which is the first point where
 * both preconditions hold: there is a Mac heap for NewPtrSysClear to allocate
 * the pool from, and -- the reason it is not InstallDrivers() -- the System file
 * has finished installing its own trap patches. Installing after those leaves
 * our trampoline at the head of the chain, so a hook still runs and a
 * TOOLBOX_ACTION_PASSTHROUGH still reaches whatever the System installed. Going
 * first would mean the System overwrote our entry (hook silently dead), or
 * chained onto it and left us passing through to whatever the trap resolved to
 * mid-boot -- for a trap the System itself implements, that is the ROM's
 * Unimplemented handler.
 *
 * Resetting the machine re-runs all of this, so the function has to distinguish
 * the two ways it can be entered a second time:
 *
 *   - after a reset, where the System heap was rebuilt and the trap table
 *     refilled from ROM: last boot's pool and stubs are gone, and everything
 *     must be installed again.
 *   - twice inside one boot, where re-installing would read our own stub as the
 *     "original" address and chain a second trampoline onto the first, running
 *     every handler twice.
 *
 * The trap table itself tells them apart: if it still dispatches through a stub
 * we installed, we are inside the same boot.
 */
void ToolboxTrap_InstallAll(void)
{
	if (!toolbox_hooks_enabled())
		return;

	// Exit early if no traps registered
	if (s_trap_count == 0)
		return;

	if (s_traps_installed) {
		if (trap_table_still_ours()) {
			printf("[TOOLBOX-TRAP] Hooks are already installed in this boot; nothing to do\n");
			fflush(stdout);
			return;
		}
		// The guest reset: the heap and trap table are new, so install again.
		printf("[TOOLBOX-TRAP] Trap table no longer holds our stubs (machine reset); reinstalling\n");
		forget_previous_installation();
	}

	printf("[TOOLBOX-TRAP] Installing %d registered trap hooks...\n", s_trap_count);
	fflush(stdout);

	// Allocate non-relocatable buffer in System Heap using NewPtrSysClear (0xA71E) if not yet allocated
	if (!s_stub_pool_base) {
		uint32 pool_size = MAX_TOOLBOX_TRAPS * TRAMPOLINE_SIZE;
		M68kRegisters r;
		r.d[0] = pool_size;
		Execute68kTrap(0xa71e, &r); // NewPtrSysClear()
		ToolboxTrap_SetStubPool(r.a[0]);

		if (!s_stub_pool_base) {
			printf("[TOOLBOX-TRAP] FATAL: Failed to allocate guest stub pool memory!\n");
			return;
		}
		printf("[TOOLBOX-TRAP] Allocated guest trampoline pool at 0x%08X (size %u bytes)\n",
		       s_stub_pool_base, pool_size);
	}

	/*
	 * _Unimplemented ($A89F) is where the trap table sends every trap this
	 * System does not implement. Hooking such a trap is legal -- a handler
	 * returning TOOLBOX_ACTION_REPLACE is how a subsystem would implement one --
	 * but a passthrough hook on it is a hook that goes nowhere, which is worth
	 * saying out loud rather than leaving to be discovered as a bomb.
	 */
	uint32 unimplemented = get_trap_address(0xa89f);

	// Install trampolines for each registered trap hook
	for (int i = 0; i < s_trap_count; i++) {
		ToolboxTrapDesc &desc = s_trap_table[i];
		if (desc.is_installed)
			continue;

		// 1. Fetch current/original trap address from Mac OS trap table
		desc.original_addr = get_trap_address(desc.trap_num);

		// 2. Assign trampoline memory location
		desc.stub_addr = alloc_stub_slot();
		if (!desc.stub_addr) {
			printf("[TOOLBOX-TRAP] FATAL: trampoline pool exhausted at trap 0x%04X\n",
			       desc.trap_num);
			break;
		}

		// 3. Write the trampoline into guest RAM
		ToolboxTrap_WriteTrampoline(desc.stub_addr, desc.trap_num, desc.original_addr);

		// 4. Update Mac OS trap table entry to point to our trampoline stub
		M68kRegisters r_set;
		r_set.d[0] = desc.trap_num;
		r_set.a[0] = desc.stub_addr;
		// Use _SetToolTrap (0xA647) for Toolbox traps, _SetOSTrapAddress (0xA247) for OS traps
		Execute68kTrap((desc.trap_num & 0x0800) ? 0xa647 : 0xa247, &r_set);

		desc.is_installed = true;
		printf("[TOOLBOX-TRAP] Hooked 0x%04X (%s) -> stub 0x%08X (orig 0x%08X)%s\n",
		       desc.trap_num, desc.name, desc.stub_addr, desc.original_addr,
		       desc.original_addr == unimplemented ? "  [trap is unimplemented]" : "");
	}

	s_traps_installed = true;
	fflush(stdout);
}

/*
 * Identifies the trampoline that a guest PC is executing inside.
 *
 * The dispatcher has to know which of the hooked traps it was entered for, and
 * the only thing distinguishing them at that moment is where the EmulOp word
 * sits in memory. Recovering that as "PC - 2" is wrong on two of the three
 * engines: it read Musashi's PC register directly, so under UAE or m68k-rs it
 * saw a stale value, decoded a bogus trap number and then jumped through a
 * garbage A0.
 *
 * Slot arithmetic over the trampoline pool fixes both halves. The engine is
 * asked for its own PC through cpu_engine_get_pc(), and the pool layout makes
 * the answer insensitive to how far that PC has advanced: every trampoline
 * occupies its own TRAMPOLINE_SIZE-byte slot, and the EmulOp word is in the
 * first two bytes of it, so PC-at-the-opcode and PC-past-the-opcode land in
 * the same slot either way.
 *
 * Arguments:
 *   pc: Guest PC reported by the active engine.
 *
 * Returns:
 *   Trampoline slot address, or 0 if the PC is not inside the pool.
 */
static uint32 stub_addr_from_pc(uint32 pc)
{
	/*
	 * Bound against the cursor, not s_trap_count: unregistering a trap shrinks
	 * the table without reclaiming its slot, so slots handed out is the only
	 * safe upper limit.
	 */
	if (!s_stub_pool_base || pc < s_stub_pool_base || pc >= s_stub_pool_cursor)
		return 0;
	uint32 index = (pc - s_stub_pool_base) / TRAMPOLINE_SIZE;
	return s_stub_pool_base + index * TRAMPOLINE_SIZE;
}

/*
 * Central dispatcher invoked from EmulOp() when M68K_EMUL_OP_TOOLBOX_DISPATCH (0x7130) executes.
 *
 * Arguments:
 *   r: Pointer to active 68k register state.
 */
void ToolboxTrap_Dispatch(struct M68kRegisters *r)
{
	uint32 stub_addr = stub_addr_from_pc(cpu_engine_get_pc());
	if (!stub_addr) {
		/*
		 * Nothing sane to pass through to: A0 would be garbage and the stub
		 * ends in "jmp (a0)". Send control to the stub's own rts-equivalent by
		 * leaving A0 alone is not safe either, so refuse loudly instead.
		 */
		printf("[TOOLBOX-TRAP] FATAL: dispatch from PC 0x%08X, outside the stub pool\n",
		       cpu_engine_get_pc());
		fflush(stdout);
		return;
	}

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
