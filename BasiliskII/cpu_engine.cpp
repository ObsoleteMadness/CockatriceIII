/*
 *  cpu_engine.cpp - CPU Engine Registry and Global Dispatcher
 *
 *  CockatriceIII Multi-Engine Architecture
 *  (C) 2026 CockatriceIII Project
 *
 *  This file provides the central engine registry and global dispatchers
 *  routing Basilisk II CPU lifecycle calls, interrupts, and nested subroutine
 *  execution to the currently active CPU engine (Musashi, Amiberry/UAE, or m68k-rs).
 */

#include <stdio.h>
#include <string.h>
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "prefs.h"
#include "rom_patches.h"
#include "emul_op.h"
#include "main.h"
#include "scc.h"
#include "menu_bar.h"
#include "timer.h"
#include "ether.h"
#include "scsi.h"
#include "sony.h"
#include "disk.h"
#include "audio.h"

// Last Macintosh PC observed by the active engine (VideoInterrupt heartbeat)
extern "C" uint32 cpu_engine_last_pc = 0;

// Maximum supported concurrently registered CPU engines
#define MAX_CPU_ENGINES 8

// Forward declarations for built-in CPU engines
extern const CPUEngine musashi_cpu_engine;
extern const CPUEngine amiberry_cpu_engine;
extern const CPUEngine m68k_rs_cpu_engine;

// Engine registry state
static const CPUEngine *s_engines[MAX_CPU_ENGINES] = {
	&musashi_cpu_engine,
	&amiberry_cpu_engine,
	&m68k_rs_cpu_engine
};
static int s_engine_count = 3;
static const CPUEngine *s_active_engine = &musashi_cpu_engine;

// Global JIT preference flags
bool UseJIT = false;
bool UseJITFPU = false;
uint32 JITCacheSize = 2048;

// Execution return flag stack for supporting nested Execute68k / Execute68kTrap calls
#define MAX_NESTED_EXEC 32
static bool *s_return_stack[MAX_NESTED_EXEC];
static int s_return_stack_top = 0;

/*
 * Pushes a boolean return indicator pointer onto the nested execution stack.
 *
 * Arguments:
 *   flag: Pointer to boolean flag set when M68K_EXEC_RETURN is hit.
 */
void PushReturnStack(bool *flag)
{
	// Push flag pointer if return stack is not full
	if (s_return_stack_top < MAX_NESTED_EXEC) {
		s_return_stack[s_return_stack_top++] = flag;
	}
}

/*
 * Pops the top boolean return indicator from the nested execution stack.
 */
void PopReturnStack(void)
{
	// Decrement stack pointer if return stack is non-empty
	if (s_return_stack_top > 0) {
		s_return_stack_top--;
	}
}

/*
 * Marks the active execution level as returned by setting its return flag to true.
 * Called when an M68K_EXEC_RETURN (0x7100) opcode is encountered by the CPU core.
 */
void TriggerExecutionReturn(void)
{
	// Signal the top-most active execution slice to return
	if (s_return_stack_top > 0) {
		*s_return_stack[s_return_stack_top - 1] = true;
	}
}

/*
 * Checks if the currently active execution slice has been triggered to return.
 *
 * Returns:
 *   true if the top return stack flag is true, false otherwise.
 */
bool IsExecutionReturnTriggered(void)
{
	// Check if top flag is set
	if (s_return_stack_top > 0) {
		return *s_return_stack[s_return_stack_top - 1];
	}
	return false;
}

/*
 * Records the Macintosh PC for heartbeat and crash diagnostics.
 *
 * Arguments:
 *   pc: 32-bit guest program counter.
 */
void cpu_engine_note_pc(uint32 pc)
{
	cpu_engine_last_pc = pc;
}

/*
 * Maps ROMVersion onto the Macintosh ROM base used by every CPU engine.
 *
 * RAMBaseMac is always 0 in this tree. Unknown ROM words are fatal so a
 * glue file cannot silently boot a Classic image at 0x40800000.
 *
 * Returns:
 *   true on a known ROM family, false otherwise.
 */
bool cpu_engine_map_rom_base(void)
{
	RAMBaseMac = 0;
	switch (ROMVersion) {
		case ROM_VERSION_64K:
		case ROM_VERSION_PLUS:
		case ROM_VERSION_CLASSIC:
			ROMBaseMac = 0x00400000;
			return true;
		case ROM_VERSION_II:
			ROMBaseMac = 0x00a00000;
			return true;
		case ROM_VERSION_32:
			ROMBaseMac = 0x40800000;
			return true;
		default:
			return false;
	}
}

/*
 * Computes the Basilisk interrupt level from SCC then InterruptFlags.
 *
 * UAE queries this every slice (intlev). Musashi latches it via m68k_set_irq
 * and clears on ack. m68k-rs must not keep a stale IPL after the 60 Hz
 * handler has already cleared InterruptFlags.
 *
 * Returns:
 *   0, 1, 2, or 4 as documented in cpu_engine.h.
 */
int cpu_engine_intlev(void)
{
	if (SCCInterruptRequest)
		return TwentyFourBitAddressing ? 2 : 4;
	return InterruptFlags ? 1 : 0;
}

/*
 * Resets host-side Mac peripherals after a 68k warm reset.
 *
 * Called from each engine's longjmp Reset680x0 path. ROM patches stay in
 * place; only RAM and device state are wiped.
 */
void cpu_engine_reset_peripherals(void)
{
	MenuQueue_Reset();
	InterruptFlags = 0;
	TimerReset();
	EtherReset();
	SCC_Reset();
	SCSIReset();
	SonyReset();
	DiskReset();
	AudioReset();
	Mac_memset(RAMBaseMac, 0, RAMSize);
}

/*
 * Invalidates any compiled basic blocks or JIT code translation cache
 * in the specified address range [addr .. addr + size).
 *
 * Arguments:
 *   addr: Starting Macintosh guest address.
 *   size: Size in bytes of the modified range.
 */
void cpu_engine_invalidate_code(uint32 addr, uint32 size)
{
	// Forward invalidation request to active engine if it supports code invalidation
	if (s_active_engine && s_active_engine->invalidate_code)
		s_active_engine->invalidate_code(addr, size);
}

/*
 * Gates EmulOp A7 writeback the way Musashi's illg callback does.
 *
 * RESET must be able to move A7 to 0x10000. Random EmulOp results must not
 * replace a live ISP with a non-RAM pointer.
 *
 * Arguments:
 *   old_a7: Live stack before writeback.
 *   new_a7: Candidate from M68kRegisters.
 *
 * Returns:
 *   true if the engine should store new_a7 as A7 (and supervisor ISP).
 */
bool cpu_engine_should_commit_a7(uint32 old_a7, uint32 new_a7)
{
	return new_a7 != old_a7 && new_a7 >= 0x1000 && new_a7 < RAMSize;
}

/*
 * Substitutes the 64 KB boot stack when A7 is below Low Mem or past RAM.
 *
 * Arguments:
 *   sp: Candidate stack pointer.
 *
 * Returns:
 *   sp if it looks like a RAM stack, else CPU_ENGINE_INIT_SP.
 */
uint32 cpu_engine_clamp_sp(uint32 sp)
{
	if (sp < 0x1000 || sp >= RAMSize)
		return CPU_ENGINE_INIT_SP;
	return sp;
}

/*
 * Plants a Line-A opcode and 0x7100 return hook for Execute68kTrap.
 *
 * Arguments:
 *   sp: Current A7 (already clamped).
 *   trap: 16-bit trap word.
 *
 * Returns:
 *   Address of the 4-byte stub.
 */
uint32 cpu_engine_write_trap_stub(uint32 sp, uint16 trap)
{
	uint32 stub = sp - 4;
	WriteMacInt16(stub + 0, trap);
	WriteMacInt16(stub + 2, (uint16)M68K_EXEC_RETURN);
	return stub;
}

/*
 * Plants an RTS return frame that lands on M68K_EXEC_RETURN for Execute68k.
 *
 * Arguments:
 *   sp: Current A7 (already clamped).
 *   ret_addr_out: Receives the address of the 0x7100 word.
 *
 * Returns:
 *   New A7 (return address then 0x7100 below the old SP).
 */
uint32 cpu_engine_write_exec_return_frame(uint32 sp, uint32 *ret_addr_out)
{
	sp -= 2;
	WriteMacInt16(sp, (uint16)M68K_EXEC_RETURN);
	*ret_addr_out = sp;
	sp -= 4;
	WriteMacInt32(sp, *ret_addr_out);
	return sp;
}

/*
 * Registers a CPU engine into the global registry table.
 * If an engine with the same ID already exists, it is replaced.
 *
 * Arguments:
 *   engine: Pointer to the CPUEngine structure to register.
 */
void RegisterCPUEngine(const CPUEngine *engine)
{
	// Validate input pointer and engine identifier
	if (!engine || !engine->id)
		return;

	// Check if engine is already registered and update existing entry
	for (int i = 0; i < s_engine_count; i++) {
		if (strcmp(s_engines[i]->id, engine->id) == 0) {
			s_engines[i] = engine;
			return;
		}
	}

	// Append new engine if capacity permits
	if (s_engine_count < MAX_CPU_ENGINES) {
		s_engines[s_engine_count++] = engine;
	}
}

/*
 * Looks up a registered CPU engine by its unique string identifier.
 *
 * Arguments:
 *   id: Engine identifier (e.g. "musashi", "uae", "m68k_rs").
 *
 * Returns:
 *   Pointer to the matching CPUEngine, or NULL if not found.
 */
const CPUEngine *GetCPUEngine(const char *id)
{
	// Validate input identifier
	if (!id)
		return NULL;

	// Search registered engines for matching ID
	for (int i = 0; i < s_engine_count; i++) {
		if (strcmp(s_engines[i]->id, id) == 0)
			return s_engines[i];
	}
	return NULL;
}

/*
 * Sets the currently active CPU engine by its identifier.
 *
 * Arguments:
 *   id: Engine identifier ("musashi", "uae", "m68k_rs").
 *
 * Returns:
 *   true if the engine was found and activated, false otherwise.
 */
bool SetActiveCPUEngine(const char *id)
{
	// Look up engine by identifier
	const CPUEngine *engine = GetCPUEngine(id);
	if (engine) {
		// Set active engine and announce selection
		s_active_engine = engine;
		printf("[CPU-ENGINE] Active 680x0 CPU Engine: %s (%s)\n", engine->name, engine->id);
		fflush(stdout);
		return true;
	}
	return false;
}

/*
 * Returns a pointer to the currently active CPU engine.
 */
const CPUEngine *GetActiveCPUEngine(void)
{
	// Return active engine pointer
	return s_active_engine;
}

/*
 * Returns the total number of registered CPU engines.
 */
int GetRegisteredCPUEngineCount(void)
{
	// Return engine count
	return s_engine_count;
}

/*
 * Retrieves a registered CPU engine by its zero-based registry index.
 *
 * Arguments:
 *   index: Zero-based engine index (0 .. GetRegisteredCPUEngineCount() - 1).
 *
 * Returns:
 *   Pointer to CPUEngine, or NULL if index is out of bounds.
 */
const CPUEngine *GetRegisteredCPUEngineByIndex(int index)
{
	// Bounds check index
	if (index >= 0 && index < s_engine_count)
		return s_engines[index];
	return NULL;
}

/*
 * Helper ensuring all default CPU engines are registered in the global table.
 */
static void EnsureEnginesRegistered(void)
{
	// Re-register defaults if table is empty
	if (s_engine_count == 0) {
		RegisterCPUEngine(&musashi_cpu_engine);
		RegisterCPUEngine(&amiberry_cpu_engine);
		RegisterCPUEngine(&m68k_rs_cpu_engine);
	}
}

/*
 * Global Basilisk II CPU Emulation Interface Dispatchers
 */

/*
 * Initializes 680x0 emulation according to user configuration preferences.
 *
 * Returns true if initialization succeeded, false otherwise.
 */
bool Init680x0(void)
{
	// Ensure built-in CPU engines are registered
	EnsureEnginesRegistered();

	// Read requested CPU engine from preferences, defaulting to "musashi"
	const char *requested = PrefsFindString("cpu_emulator");
	if (!requested || !requested[0]) {
		requested = "musashi";
	}

	// uae and m68k_rs must not silently fall back to Musashi
	if (!SetActiveCPUEngine(requested)) {
		if (strcmp(requested, "uae") == 0 || strcmp(requested, "m68k_rs") == 0) {
			printf("[CPU-ENGINE] FATAL: Requested engine '%s' is not available\n", requested);
			return false;
		}
		printf("[CPU-ENGINE] Warning: Requested engine '%s' not available, falling back to 'musashi'\n", requested);
		if (!SetActiveCPUEngine("musashi")) {
			printf("[CPU-ENGINE] FATAL: No CPU engines registered!\n");
			return false;
		}
	}

	// Read JIT configuration flags
	UseJIT = PrefsFindBool("jit");
	UseJITFPU = PrefsFindBool("jitfpu");
	int32 cachesize = PrefsFindInt32("jitcachesize");
	if (cachesize > 0)
		JITCacheSize = (uint32)cachesize;

	// Musashi has no translator at all and ignores the flag. m68k_rs honours it:
	// UseJIT selects its batch executor (decoded-op cache, direct-RAM window,
	// trace JIT) over the cycle-accurate interpreter.
	if (s_active_engine && strcmp(s_active_engine->id, "musashi") == 0) {
		UseJIT = false;
	}

	// JIT FPU compilation requires general JIT to be enabled
	if (!UseJIT) {
		UseJITFPU = false;
	}

	// Initialize the active CPU engine
	if (s_active_engine && s_active_engine->init) {
		return s_active_engine->init();
	}
	return false;
}

/*
 * Signals the active CPU engine to terminate.
 */
void Exit680x0(void)
{
	// Forward exit call to active engine
	if (s_active_engine && s_active_engine->exit)
		s_active_engine->exit();
}

/*
 * Enters the main execution loop of the active CPU engine.
 */
void Start680x0(void)
{
	// Forward start call to active engine
	if (s_active_engine && s_active_engine->start)
		s_active_engine->start();
}

/*
 * Triggers a warm software reset on the active CPU engine.
 */
void Reset680x0(void)
{
	// Forward reset call to active engine
	if (s_active_engine && s_active_engine->reset)
		s_active_engine->reset();
}

/*
 * Executes a 680x0 subroutine at `addr` and returns control to C++.
 *
 * Arguments:
 *   addr: 32-bit Mac address of the subroutine entry point.
 *   r: Pointer to M68kRegisters struct with input/output registers.
 */
extern "C" void Execute68k(uint32 addr, M68kRegisters *r)
{
	// Forward subroutine execution to active engine
	if (s_active_engine && s_active_engine->execute_68k)
		s_active_engine->execute_68k(addr, r);
}

/*
 * Executes a Mac OS Line-A or Toolbox trap subroutine and returns control to C++.
 *
 * Arguments:
 *   trap: 16-bit Line-A trap opcode.
 *   r: Pointer to M68kRegisters struct with input/output registers.
 */
extern "C" void Execute68kTrap(uint16 trap, M68kRegisters *r)
{
	// Forward trap execution to active engine
	if (s_active_engine && s_active_engine->execute_68k_trap)
		s_active_engine->execute_68k_trap(trap, r);
}

/*
 * Asserts a standard interrupt on the active CPU engine.
 */
void TriggerInterrupt(void)
{
	// Forward interrupt trigger to active engine
	if (s_active_engine && s_active_engine->trigger_interrupt)
		s_active_engine->trigger_interrupt();
}

/*
 * Triggers a non-maskable interrupt (NMI, Level 7) on the active CPU engine.
 */
void TriggerNMI(void)
{
	// Forward NMI trigger to active engine
	if (s_active_engine && s_active_engine->trigger_nmi)
		s_active_engine->trigger_nmi();
}

/*
 * Returns the current interrupt request level from the active CPU engine.
 */
extern "C" int intlev(void)
{
	// Query interrupt level from active engine
	if (s_active_engine && s_active_engine->intlev)
		return s_active_engine->intlev();
	return 0;
}
