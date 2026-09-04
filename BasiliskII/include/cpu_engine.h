/*
 *  cpu_engine.h - Unified 680x0 CPU Engine Abstraction Layer
 *
 *  CockatriceIII Multi-Engine Architecture
 *  Supports: Musashi (C core), Amiberry/UAE (680x0 + JIT), m68k-rs (Rust)
 *
 *  (C) 2026 CockatriceIII Project
 *
 *  This header defines the abstract CPUEngine dispatch table and global
 *  registration API used by Cockatrice III to support multiple swappable
 *  680x0 CPU emulation cores (Musashi, Amiberry/UAE, m68k-rs).
 */

#ifndef CPU_ENGINE_H
#define CPU_ENGINE_H

#include "sysdeps.h"
#include "cpu_emulation.h"

#ifdef __cplusplus
extern "C" {
#endif

struct M68kRegisters;

/*
 * How an engine reaches guest memory. Declared per-engine so callers (test
 * harness, future prefs UI) can tell without reading glue code whether an
 * engine's accesses are bounds-checked against the region table (Callback)
 * or whether it inlines Host_Mem_Base-relative pointer arithmetic itself
 * (DirectPointer) and relies on guard-page faulting for bad addresses.
 */
typedef enum {
	CPU_MEM_STRATEGY_CALLBACK,       /* Read/WriteMacInt* on every access */
	CPU_MEM_STRATEGY_DIRECT_POINTER  /* Inlines Host_Mem_Base + addr itself */
} CPUMemStrategy;

/*
 * Correctness-vs-speed classification. Musashi is the permanent golden
 * reference every other engine's output is checked against; UAE and m68k-rs
 * are speed/compatibility tradeoff options. This makes that policy explicit
 * in code instead of by convention.
 */
typedef enum {
	CPU_ENGINE_TIER_GOLDEN,      /* Correctness reference */
	CPU_ENGINE_TIER_PERFORMANCE  /* Speed/compatibility tradeoff */
} CPUEngineTier;

/*
 * CPU Engine Definition Interface
 * Defines the complete lifecycle, execution, and interrupt contract for a CPU engine.
 */
typedef struct CPUEngine {
	const char *id;             /* Unique identifier: "musashi", "uae", "m68k_rs" */
	const char *name;           /* Human-readable description */
	bool is_jit;                /* True if dynamic binary translator, false for interpreter */
	CPUMemStrategy mem_strategy; /* How this engine accesses guest memory */
	CPUEngineTier tier;          /* Golden correctness reference vs. performance tradeoff */

	/* Lifecycle functions */
	bool (*init)(void);         /* Initialize engine and allocate code/memory caches */
	void (*exit)(void);         /* Clean up and stop execution */
	void (*start)(void);        /* Enter main continuous execution loop */
	void (*reset)(void);        /* Perform software reset */

	/* Execution functions */
	void (*execute_68k)(uint32 addr, struct M68kRegisters *r);       /* Execute subroutine */
	void (*execute_68k_trap)(uint16 trap, struct M68kRegisters *r);  /* Execute Line-A trap */

	/* Interrupts */
	void (*trigger_interrupt)(void);  /* Assert standard interrupt level */
	void (*trigger_nmi)(void);        /* Assert level 7 NMI */
	int  (*intlev)(void);             /* Query current interrupt request level */

	/* Code Cache / JIT Translation Invalidation */
	void (*invalidate_code)(uint32 addr, uint32 size); /* Invalidate code in address range */
} CPUEngine;

/*
 * Engine Registry & Selection API
 */

// Registers a new or replacement CPU engine in the global dispatch table
void RegisterCPUEngine(const CPUEngine *engine);

// Looks up a registered CPU engine by its identifier string
const CPUEngine *GetCPUEngine(const char *id);

// Sets the active CPU engine used for all emulation calls
bool SetActiveCPUEngine(const char *id);

// Retrieves the currently active CPU engine
const CPUEngine *GetActiveCPUEngine(void);

// Returns the count of currently registered CPU engines
int GetRegisteredCPUEngineCount(void);

// Retrieves a registered CPU engine by its index (0 .. count-1)
const CPUEngine *GetRegisteredCPUEngineByIndex(int index);

/*
 * Execution Return Stack API
 * Used by Execute68k / Execute68kTrap to catch M68K_EXEC_RETURN (0x7100)
 */

// Pushes a return indicator flag pointer for nested subroutine calls
void PushReturnStack(bool *flag);

// Pops the active return indicator flag
void PopReturnStack(void);

// Triggers the active return flag when 0x7100 is executed
void TriggerExecutionReturn(void);

// Checks if the active execution level has been signaled to return
bool IsExecutionReturnTriggered(void);

/*
 * Shared 680x0 boot / interrupt / trap helpers.
 *
 * Every engine used to copy these. Keep the numbers and the A7-writeback
 * test in one place so RESET, Execute68kTrap, and 60 Hz IPL stay aligned.
 */
enum {
	CPU_ENGINE_BOOT_SP = 0x2000,	/* Temporary A7 until RESET EmulOp */
	CPU_ENGINE_INIT_SP = 0x10000,	/* Supervisor stack for Execute68kTrap */
	CPU_ENGINE_BOOT_SR = 0x2700,	/* Supervisor, IPL 7 */
	CPU_ENGINE_BOOT_PC_OFF = 0x2a	/* ROM entry used by Start680x0 */
};

/* Last guest PC sampled by the active engine (heartbeat / diagnostics). */
extern uint32 cpu_engine_last_pc;

/* Records a Macintosh PC so VideoInterrupt heartbeat can print it. */
void cpu_engine_note_pc(uint32 pc);

/*
 * Unconditionally logs a 68k exception that reached the CPU's vector dispatch,
 * for the small set of vectors that indicate a genuine fault (as opposed to
 * ordinary A-line/Toolbox trap dispatch, which also runs through the same
 * mechanism thousands of times per second and would drown this out). Each of
 * these vectors is exactly one Mac OS System Error "bomb" type (bomb type =
 * vector - 1), so this is engine-independent ground truth for "did Mac OS
 * just crash" without needing to read Mac OS's own DSErrCode global.
 *
 * Every CPU core (Musashi, UAE, m68k-rs) funnels every exception through one
 * central dispatch point already; call this from there for vectors 2-8 and
 * 11 (bus error, address error, illegal instruction, zero divide, CHK,
 * TRAPV, privilege violation, line-1111). Skip vector 9 (trace, debugger
 * use) and vector 10 (line-1010, legitimate Toolbox trap dispatch).
 *
 * Arguments:
 *   engine: Short engine id ("musashi", "uae", "m68k_rs").
 *   vector: 68k exception vector number (2-11).
 *   pc: Guest PC of the faulting instruction.
 */
void cockatrice_report_cpu_exception(const char *engine, int vector, uint32 pc);

/*
 * Sets RAMBaseMac = 0 and ROMBaseMac from ROMVersion.
 *
 * Returns:
 *   true if ROMVersion is a known family, false if the engine must refuse init.
 */
bool cpu_engine_map_rom_base(void);

/*
 * Highest pending Basilisk interrupt level (SCC then VIA/InterruptFlags).
 *
 * Returns:
 *   0, 1 (VIA/60 Hz), 2 or 4 (SCC, 24-bit vs 32-bit).
 */
int cpu_engine_intlev(void);

/* Warm-reset peripherals and zero Mac RAM after Reset680x0(). */
void cpu_engine_reset_peripherals(void);

/*
 * Invalidates compiled blocks or JIT code translation cache across [addr .. addr + size).
 *
 * Arguments:
 *   addr: Starting Macintosh guest address.
 *   size: Size in bytes of the modified range.
 */
void cpu_engine_invalidate_code(uint32 addr, uint32 size);

/*
 * True when EmulOp changed A7 to a RAM stack that should replace the live SP.
 *
 * Arguments:
 *   old_a7: Stack pointer before EmulOp.
 *   new_a7: r.a[7] after EmulOp (RESET writes 0x10000).
 */
bool cpu_engine_should_commit_a7(uint32 old_a7, uint32 new_a7);

/*
 * Returns a RAM stack pointer, substituting the 64 KB boot stack if unmapped.
 *
 * Arguments:
 *   sp: Candidate A7 from the CPU core.
 */
uint32 cpu_engine_clamp_sp(uint32 sp);

/*
 * Writes a 4-byte Execute68kTrap stub: [trap][M68K_EXEC_RETURN] at sp-4.
 *
 * Arguments:
 *   sp: Current A7 (already clamped).
 *   trap: Line-A / Toolbox opcode.
 *
 * Returns:
 *   Address of the stub (new A7 / PC).
 */
uint32 cpu_engine_write_trap_stub(uint32 sp, uint16 trap);

/*
 * Pushes M68K_EXEC_RETURN then a 4-byte return address for Execute68k RTS.
 *
 * Arguments:
 *   sp: Current A7 (already clamped).
 *   ret_addr_out: Receives the address of the 0x7100 word.
 *
 * Returns:
 *   New A7 after both pushes.
 */
uint32 cpu_engine_write_exec_return_frame(uint32 sp, uint32 *ret_addr_out);

/* Built-in Engine Instances */
extern const CPUEngine musashi_cpu_engine;
extern const CPUEngine amiberry_cpu_engine;
extern const CPUEngine m68k_rs_cpu_engine;

#ifdef __cplusplus
}
#endif

#endif /* CPU_ENGINE_H */
