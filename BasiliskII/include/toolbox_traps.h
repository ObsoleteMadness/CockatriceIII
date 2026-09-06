/*
 *  toolbox_traps.h - Modular Macintosh Toolbox and OS Trap Dispatcher & Acceleration Architecture
 *
 *  Cockatrice III
 *  (C) 2026 Cockatrice III Project
 *
 *  High-Level Concept Block:
 *  =========================
 *  Classic Macintosh software interfaces with the system through "A-Line" traps
 *  (opcodes in the range 0xA000 to 0xAFFF). These are split into:
 *    - OS Traps (0xA000..0xA7FF): Low-level hardware/kernel routines (Memory Manager,
 *      File Manager, Device Manager). Arguments are passed in 68k registers (D0, A0, etc.)
 *      and return status codes in D0.
 *    - Toolbox Traps (0xA800..0xAFFF): High-level UI and runtime managers (Menu Manager,
 *      Window Manager, Dialog Manager, QuickDraw, Resource Manager). Arguments are passed
 *      on the 68k stack (A7) using Pascal calling conventions, where the caller reserves
 *      stack space for the return value, pushes arguments left-to-right, and the callee
 *      cleans up the argument bytes before returning.
 *
 *  This subsystem allows any developer to hook, monitor, or completely accelerate any
 *  Toolbox or OS trap simply by registering a trap number (e.g. 0xA937 for _DrawMenuBar)
 *  and a C++ handler function.
 *
 *  Execution Mechanics:
 *  -------------------
 *  1. When CockatriceIII_Prefs sets toolbox_hooks true, ToolboxTrap_InstallAll()
 *     (called once per boot from PatchAfterStartup(), after the System file has
 *     installed its own trap patches) queries trap addresses via _GetToolTrapAddress.
 *  2. An 12-byte trampoline stub is installed into guest memory for each hooked trap:
 *         0x7130 (M68K_EMUL_OP_TOOLBOX_DISPATCH)
 *         move.l a1, a7   ; Update stack pointer (if modified by accelerated replacement)
 *         jmp (a0)        ; Jump to destination (original ROM address or caller return address)
 *         dc.w <trap_number>
 *         dc.l <original_trap_addr>
 *  3. The trap table entry is updated via _SetToolTrap / _SetOSTrapAddress to point to the stub.
 *  4. When guest 68k code executes the trap, it enters the stub and hits 0x7130.
 *  5. The CPU engine invokes EmulOp(), which forwards to ToolboxTrap_Dispatch().
 *  6. The C++ handler receives the 68k register state (r) and original address:
 *     - TOOLBOX_ACTION_PASSTHROUGH: Sets r->a[0] = original_addr, r->a[1] = r->a[7].
 *       Control continues into the ROM/System implementation.
 *     - TOOLBOX_ACTION_REPLACE: The handler uses ToolboxArgs to pop arguments, write the
 *       return value, and pop the return address into r->a[0] and new stack into r->a[1].
 *       Control returns directly to the 68k caller!
 */

#ifndef TOOLBOX_TRAPS_H
#define TOOLBOX_TRAPS_H

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include <string>

/*
 * Action returned by a Toolbox trap handler function.
 */
typedef enum {
	TOOLBOX_ACTION_PASSTHROUGH = 0, // Fall through to original ROM/System trap implementation
	TOOLBOX_ACTION_REPLACE     = 1, // Native C++ replacement; return directly to caller
	TOOLBOX_ACTION_POST_HOOK   = 2  // Run original ROM trap first; handler runs again after return (reserved)
} ToolboxAction;

/*
 * Function pointer type for Toolbox and OS trap handlers.
 *
 * Arguments:
 *   trap_num: 16-bit A-line trap number (e.g. 0xA937).
 *   r: Pointer to live 68k registers (D0..D7, A0..A7, SR).
 *   original_addr: 32-bit Macintosh address of the original ROM/System trap handler.
 *   user_data: Optional user pointer passed during registration.
 *
 * Returns:
 *   ToolboxAction indicating whether to pass through to ROM or return to caller.
 */
typedef ToolboxAction (*ToolboxTrapHandler)(uint16 trap_num, struct M68kRegisters *r, uint32 original_addr, void *user_data);

/*
 * Description of a registered trap hook.
 */
struct ToolboxTrapDesc {
	uint16 trap_num;              // 16-bit trap number (0xA000..0xAFFF)
	const char *name;             // Human-readable symbol name (e.g. "_DrawMenuBar")
	ToolboxTrapHandler handler;   // C++ handler callback
	void *user_data;              // Context pointer
	uint32 stub_addr;             // Guest address of allocated trampoline stub
	uint32 original_addr;         // Original Mac OS / ROM trap address
	bool is_installed;            // True if patched into Mac OS trap table
};

/*
 * Helper class for parsing and constructing Macintosh Toolbox stack frames (Pascal convention).
 *
 * Standard Pascal Toolbox Stack Layout on Trap Entry:
 *   [A7 + 0]            : 32-bit Return Address (pushed by trap dispatcher)
 *   [A7 + 4]            : Last pushed argument
 *   ...
 *   [A7 + 4 + N - 4]    : First pushed argument
 *   [A7 + 4 + N]        : Return value slot (if function returns a value)
 */
class ToolboxArgs {
public:
	/*
	 * Constructs a ToolboxArgs helper bound to the active 68k register context.
	 *
	 * Arguments:
	 *   regs: Pointer to M68kRegisters structure.
	 */
	explicit ToolboxArgs(struct M68kRegisters *regs)
		: m_regs(regs), m_sp(regs->a[7]), m_ret_addr(0), m_arg_cursor(0)
	{
		// Fetch the 32-bit return address residing at top of stack
		m_ret_addr = ReadMacInt32(m_sp);
		// Argument cursor begins immediately after the 4-byte return address
		m_arg_cursor = m_sp + 4;
	}

	/*
	 * Returns the 32-bit return address of the caller.
	 */
	uint32 GetReturnAddress(void) const { return m_ret_addr; }

	/*
	 * Reads an 8-bit integer argument from the current stack cursor.
	 * Note: On 680x0 stacks, 8-bit Pascal arguments are word-aligned (2 bytes on stack, value in low byte).
	 */
	uint8 PopInt8(void)
	{
		uint8 val = (uint8)ReadMacInt8(m_arg_cursor + 1);
		m_arg_cursor += 2;
		return val;
	}

	/*
	 * Reads a 16-bit integer argument from the current stack cursor.
	 */
	int16 PopInt16(void)
	{
		int16 val = (int16)ReadMacInt16(m_arg_cursor);
		m_arg_cursor += 2;
		return val;
	}

	/*
	 * Reads a 32-bit integer, pointer, or Handle from the current stack cursor.
	 */
	uint32 PopInt32(void)
	{
		uint32 val = ReadMacInt32(m_arg_cursor);
		m_arg_cursor += 4;
		return val;
	}

	/*
	 * Reads a 32-bit pointer (alias for PopInt32).
	 */
	uint32 PopPtr(void) { return PopInt32(); }

	/*
	 * Reads a 32-bit handle (alias for PopInt32).
	 */
	uint32 PopHandle(void) { return PopInt32(); }

	/*
	 * Reads a Pascal string (Str255) from the specified Macintosh guest address.
	 *
	 * Arguments:
	 *   mac_addr: 32-bit guest address pointing to length byte followed by string data.
	 *
	 * Returns:
	 *   std::string containing the decoded ASCII/Mac Roman characters.
	 */
	static std::string ReadPascalString(uint32 mac_addr)
	{
		if (!mac_addr)
			return "";
		uint8 len = (uint8)ReadMacInt8(mac_addr);
		std::string s;
		s.reserve(len);
		for (uint32 i = 0; i < len; i++) {
			s.push_back((char)ReadMacInt8(mac_addr + 1 + i));
		}
		return s;
	}

	/*
	 * Sets a 16-bit integer function return value at the reserved stack slot.
	 *
	 * Arguments:
	 *   total_arg_bytes: Total number of argument bytes pushed on the stack.
	 *   val: 16-bit integer return value.
	 */
	void SetResultInt16(int total_arg_bytes, int16 val)
	{
		uint32 res_addr = m_sp + 4 + total_arg_bytes;
		WriteMacInt16(res_addr, (uint16)val);
	}

	/*
	 * Sets a 32-bit integer/pointer function return value at the reserved stack slot.
	 *
	 * Arguments:
	 *   total_arg_bytes: Total number of argument bytes pushed on the stack.
	 *   val: 32-bit return value.
	 */
	void SetResultInt32(int total_arg_bytes, uint32 val)
	{
		uint32 res_addr = m_sp + 4 + total_arg_bytes;
		WriteMacInt32(res_addr, val);
	}

	/*
	 * Sets up 68k registers for clean Pascal return to caller, popping arguments from stack.
	 *
	 * Arguments:
	 *   param_bytes: Number of bytes of parameters to pop. If the routine is a function
	 *                returning a value on stack, param_bytes should NOT include the result slot
	 *                (the result stays on stack for caller).
	 *
	 * Returns:
	 *   TOOLBOX_ACTION_REPLACE to signal the dispatcher that native replacement is complete.
	 */
	ToolboxAction Return(int param_bytes)
	{
		// Direct execution to return address
		m_regs->a[0] = m_ret_addr;
		// Advance stack pointer past return address and parameter bytes
		m_regs->a[1] = m_sp + 4 + param_bytes;
		return TOOLBOX_ACTION_REPLACE;
	}

private:
	struct M68kRegisters *m_regs;
	uint32 m_sp;
	uint32 m_ret_addr;
	uint32 m_arg_cursor;
};

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Registers a Toolbox or OS trap hook with the modular dispatcher.
 *
 * Arguments:
 *   trap_num: 16-bit A-Line trap opcode (e.g. 0xA937 for _DrawMenuBar).
 *   name: Human-readable name for logging.
 *   handler: C++ handler callback.
 *   user_data: Optional user context pointer.
 *
 * Returns:
 *   true if registered successfully, false if table is full or invalid args.
 */
bool ToolboxTrap_Register(uint16 trap_num, const char *name, ToolboxTrapHandler handler, void *user_data);

/*
 * Returns true when the toolbox_hooks pref enables the registry.
 *
 * The master switch for the whole subsystem. Registration refuses while it is
 * false, and every client must check it before touching guest state from a
 * periodic path -- the menu bar poll and the window walk are not trap hooks and
 * would otherwise keep running with hooking off.
 */
bool ToolboxTrap_HooksEnabled(void);

/*
 * Unregisters a previously registered trap hook.
 *
 * Arguments:
 *   trap_num: 16-bit trap opcode.
 *
 * Returns:
 *   true if unregistered, false if not found.
 */
bool ToolboxTrap_Unregister(uint16 trap_num);

/*
 * Initializes and patches all registered trap trampolines into the Macintosh trap table.
 *
 * Called once per boot from PatchAfterStartup() when toolbox_hooks is true in prefs.
 * Safe to call again: a call inside the same boot does nothing, and a call after the
 * guest has reset reinstalls everything (see the .cpp for how the two are told apart).
 */
void ToolboxTrap_InstallAll(void);

/*
 * Points the registry at a caller-supplied trampoline pool.
 *
 * ToolboxTrap_InstallAll() gets the pool from NewPtrSysClear, which needs a
 * live Mac heap. This separates "where the pool is" from "how it was obtained"
 * so the dispatcher can be exercised offline.
 *
 * Arguments:
 *   base: Guest address of at least 128 * 12 bytes of RAM, or 0 to forget the
 *         current pool.
 */
void ToolboxTrap_SetStubPool(uint32 base);

/*
 * Writes one 12-byte trampoline into guest RAM (see the layout in the .cpp).
 *
 * Arguments:
 *   addr:          slot address inside the pool.
 *   trap_num:      A-line trap this trampoline stands in for.
 *   original_addr: address the trap resolved to before hooking.
 */
void ToolboxTrap_WriteTrampoline(uint32 addr, uint16 trap_num, uint32 original_addr);

/*
 * Central dispatcher invoked from EmulOp() when M68K_EMUL_OP_TOOLBOX_DISPATCH (0x7130) executes.
 *
 * Arguments:
 *   r: Pointer to active 68k register state.
 */
void ToolboxTrap_Dispatch(struct M68kRegisters *r);

/*
 * Convenience macros for trap handler definitions and registration.
 */
#define TOOLBOX_TRAP_HANDLER(func_name) \
	ToolboxAction func_name(uint16 trap_num, struct M68kRegisters *r, uint32 original_addr, void *user_data)

#define REGISTER_TOOLBOX_TRAP(trap_num, name, handler) \
	ToolboxTrap_Register((trap_num), (name), (handler), NULL)

#ifdef __cplusplus
}
#endif

#endif /* TOOLBOX_TRAPS_H */
