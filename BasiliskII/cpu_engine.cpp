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
#include <time.h>
#include <atomic>
#include <errno.h>
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
 * Ring buffer of recently-executed guest PCs, written from a per-instruction
 * hook. Dumped once (see cockatrice_report_cpu_exception()) so the very first
 * fault of a run can be traced back to how the CPU got there -- later faults
 * in a crash loop reuse the same buffer contents and are not re-dumped, to
 * keep log volume bounded.
 */
#define PC_TRACE_SIZE 4096
static uint32 s_pc_trace[PC_TRACE_SIZE];
static uint32 s_pc_trace_index = 0;
static uint32 s_pc_trace_count = 0;
static std::atomic<bool> s_pc_trace_dumped{false};

void cpu_engine_note_pc_trace(uint32 pc)
{
	s_pc_trace[s_pc_trace_index] = pc;
	s_pc_trace_index = (s_pc_trace_index + 1) % PC_TRACE_SIZE;
	if (s_pc_trace_count < PC_TRACE_SIZE)
		s_pc_trace_count++;
}

/*
 * Prints the PC ring buffer's contents in execution order (oldest first), the
 * first time this is called. No-op on every subsequent call so a crash loop
 * doesn't repeat the same trace millions of times.
 */
static void cpu_engine_dump_pc_trace_once(void)
{
	if (s_pc_trace_dumped.exchange(true, std::memory_order_relaxed))
		return;

	uint32 count = s_pc_trace_count;
	uint32 start = (count < PC_TRACE_SIZE) ? 0 : s_pc_trace_index;
	printf("[SYSTEM-ERROR-PCTRACE] last %u guest PCs executed before this fault (oldest first):\n", count);
	for (uint32 i = 0; i < count; i++) {
		uint32 idx = (start + i) % PC_TRACE_SIZE;
		printf(" %08X", s_pc_trace[idx]);
		if ((i % 8) == 7)
			printf("\n");
	}
	printf("\n");
}

/*
 * Tick-thread correlation diagnostics (see cpu_engine_note_tick() in
 * cpu_engine.h). Plain atomics, not a lock: this only needs to answer "how
 * long ago did the tick thread last touch shared state", not provide a
 * consistent snapshot of it.
 */
static std::atomic<uint64_t> s_tick_count{0};
static std::atomic<double> s_last_tick_seconds{-1.0};
static std::atomic<bool> s_dump_written{false};

// Most recent per-exception register snapshot supplied by a CPU core.
static CPUExceptionContext s_exception_ctx = {0};
static char s_exception_ctx_engine[16] = {0};

/*
 * Header written at the start of dump_file when dump_memory=true.
 * Followed by raw guest RAM bytes [RAMBaseMac .. RAMBaseMac + RAMSize).
 */
typedef struct CrashDumpHeader {
	char magic[8];              // "CKDUMP1"
	uint32 header_version;      // Format version for forward compatibility.
	uint32 vector;
	uint32 mac_bomb_type;
	uint32 ram_base_mac;
	uint32 ram_size;
	uint32 rom_base_mac;
	uint32 rom_size;
	uint32 cpu_engine_last_pc;
	uint32 tick_count_low;
	uint32 tick_count_high;
	double ms_since_last_tick;
	CPUExceptionContext ctx;
	char engine[16];
} CrashDumpHeader;

/*
 * Returns CLOCK_MONOTONIC as a fractional second, for tick-to-crash lag.
 */
static double cpu_engine_monotonic_seconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/*
 * Records that the host 60 Hz tick thread just touched guest interrupt state.
 *
 * Arguments: none. Call from one_tickbbbb() next to TriggerInterrupt().
 */
void cpu_engine_note_tick(void)
{
	s_tick_count.fetch_add(1, std::memory_order_relaxed);
	s_last_tick_seconds.store(cpu_engine_monotonic_seconds(), std::memory_order_relaxed);
}

/*
 * Stores a register snapshot for the next cockatrice_report_cpu_exception().
 *
 * Arguments:
 *   engine: Engine id that captured the snapshot.
 *   opcode: Faulting 16-bit instruction.
 *   pc: Guest PC of that instruction (prefer instruction_pc over post-increment).
 *   ppc: Engine previous-PC / following-PC, engine-specific.
 *   sr: Status register at the fault.
 *   d, a: D0..D7 and A0..A7.
 */
void cockatrice_set_cpu_exception_context(const char *engine, uint16 opcode, uint32 pc, uint32 ppc, uint16 sr, const uint32 *d, const uint32 *a)
{
	// Defensive guard for incomplete exception paths that cannot provide regs.
	if (!engine || !d || !a)
		return;

	s_exception_ctx.valid = 1;
	s_exception_ctx.opcode = opcode;
	s_exception_ctx.sr = sr;
	s_exception_ctx.pc = pc;
	s_exception_ctx.ppc = ppc;
	for (int i = 0; i < 8; i++) {
		s_exception_ctx.d[i] = d[i];
		s_exception_ctx.a[i] = a[i];
	}

	strncpy(s_exception_ctx_engine, engine, sizeof(s_exception_ctx_engine) - 1);
	s_exception_ctx_engine[sizeof(s_exception_ctx_engine) - 1] = '\0';
}

/*
 * Names match the classic Mac OS System Error alert box, whose numeric
 * "error type" is this vector number minus one (e.g. vector 11 == Type 10,
 * "Line 1111 Trap").
 */
static const char *cpu_exception_name(int vector)
{
	switch (vector) {
		case 2:  return "Bus Error";
		case 3:  return "Address Error";
		case 4:  return "Illegal Instruction";
		case 5:  return "Zero Divide";
		case 6:  return "CHK Trap";
		case 7:  return "TRAPV";
		case 8:  return "Privilege Violation";
		case 11: return "Line 1111 Trap";
		default: return "?";
	}
}

/*
 * Builds a crash register snapshot from low-memory crash globals.
 *
 * Some exception paths may not provide an explicit engine-side context.
 * Classic Mac system-error handlers store D/A/PC/SR around 0x0C30; use that
 * as a fallback so dump_file still contains useful registers.
 */
static bool cpu_engine_context_from_lowmem(CPUExceptionContext *ctx_out)
{
	if (!ctx_out || RAMSize < 0x0c78)
		return false;

	CPUExceptionContext ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.valid = 1;
	for (int i = 0; i < 8; i++)
		ctx.d[i] = ReadMacInt32(0x0c30 + (uint32)(i * 4));
	for (int i = 0; i < 8; i++)
		ctx.a[i] = ReadMacInt32(0x0c50 + (uint32)(i * 4));
	ctx.pc = ReadMacInt32(0x0c70);
	ctx.ppc = ctx.pc;
	ctx.sr = (uint16)ReadMacInt16(0x0c74);
	if (ctx.pc + 1 < RAMSize)
		ctx.opcode = ReadMacInt16(ctx.pc);

	// Reject clearly empty snapshots (all zeros).
	if (ctx.pc == 0 && ctx.sr == 0 && ctx.d[0] == 0 && ctx.a[7] == 0)
		return false;

	*ctx_out = ctx;
	return true;
}

/*
 * Writes a binary memory snapshot to dump_file when dump_memory=true.
 *
 * The dump begins with CrashDumpHeader (registers + exception metadata),
 * followed by raw guest RAM bytes for offline analysis.
 */
static void cpu_engine_write_crash_dump(const char *engine, int vector, uint32 pc, double ms_since_last_tick, uint64_t ticks, const CPUExceptionContext *ctx)
{
	if (!PrefsFindBool("dump_memory"))
		return;

	// Avoid rewriting huge dumps in tight exception loops; first crash wins.
	bool expected = false;
	if (!s_dump_written.compare_exchange_strong(expected, true, std::memory_order_relaxed))
		return;

	const char *path = PrefsFindString("dump_file");
	if (!path || !path[0])
		path = "/tmp/memory.bin";

	FILE *f = fopen(path, "wb");
	if (!f) {
		printf("[SYSTEM-ERROR-DUMP] failed to open '%s': %s\n", path, strerror(errno));
		return;
	}

	CrashDumpHeader h;
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, "CKDUMP1", 7);
	h.header_version = 1;
	h.vector = (uint32)vector;
	h.mac_bomb_type = (uint32)(vector - 1);
	h.ram_base_mac = RAMBaseMac;
	h.ram_size = RAMSize;
	h.rom_base_mac = ROMBaseMac;
	h.rom_size = ROMSize;
	h.cpu_engine_last_pc = cpu_engine_last_pc;
	h.tick_count_low = (uint32)(ticks & 0xffffffffu);
	h.tick_count_high = (uint32)(ticks >> 32);
	h.ms_since_last_tick = ms_since_last_tick;
	strncpy(h.engine, engine ? engine : "unknown", sizeof(h.engine) - 1);

	if (ctx)
		h.ctx = *ctx;
	else
		h.ctx.valid = 0;

	size_t wrote = fwrite(&h, 1, sizeof(h), f);
	if (wrote != sizeof(h)) {
		printf("[SYSTEM-ERROR-DUMP] short header write to '%s'\n", path);
		fclose(f);
		return;
	}

	// Dump live guest RAM image exactly as the CPU core currently sees it.
	if (RAMSize > 0) {
		wrote = fwrite(Mac2HostAddr(RAMBaseMac), 1, RAMSize, f);
		if (wrote != RAMSize) {
			printf("[SYSTEM-ERROR-DUMP] short RAM write to '%s': wrote %u of %u bytes\n",
			       path, (unsigned)wrote, (unsigned)RAMSize);
			fclose(f);
			return;
		}
	}

	fclose(f);
	printf("[SYSTEM-ERROR-DUMP] wrote %u-byte RAM dump + %u-byte header to %s\n",
	       (unsigned)RAMSize, (unsigned)sizeof(h), path);
}

void cockatrice_report_cpu_exception(const char *engine, int vector, uint32 pc)
{
	double now = cpu_engine_monotonic_seconds();
	double last_tick = s_last_tick_seconds.load(std::memory_order_relaxed);
	uint64_t ticks = s_tick_count.load(std::memory_order_relaxed);
	double ms_since_last_tick = (last_tick >= 0.0) ? (now - last_tick) * 1000.0 : -1.0;
	CPUExceptionContext ctx;
	CPUExceptionContext *ctx_ptr = NULL;

	if (s_exception_ctx.valid && engine && strcmp(s_exception_ctx_engine, engine) == 0) {
		ctx = s_exception_ctx;
		ctx_ptr = &ctx;
	} else if (cpu_engine_context_from_lowmem(&ctx)) {
		ctx_ptr = &ctx;
	}

	printf("[SYSTEM-ERROR] %s: 68k exception vector=%d (Mac bomb Type %d: %s) at PC=0x%08X\n",
	       engine, vector, vector - 1, cpu_exception_name(vector), pc);
	if (last_tick >= 0.0) {
		printf("[SYSTEM-ERROR-TICK] %llu ticks fired since boot; last tick was %.3fms before this report\n",
		       (unsigned long long)ticks, ms_since_last_tick);
	} else {
		printf("[SYSTEM-ERROR-TICK] no tick has fired yet\n");
	}

	if (ctx_ptr && ctx_ptr->valid) {
		printf("[SYSTEM-ERROR-REGS] opcode=0x%04X PC=0x%08X PPC=0x%08X SR=0x%04X\n",
		       ctx_ptr->opcode, ctx_ptr->pc, ctx_ptr->ppc, ctx_ptr->sr);
		printf("[SYSTEM-ERROR-REGS] D0=%08X D1=%08X D2=%08X D3=%08X D4=%08X D5=%08X D6=%08X D7=%08X\n",
		       ctx_ptr->d[0], ctx_ptr->d[1], ctx_ptr->d[2], ctx_ptr->d[3],
		       ctx_ptr->d[4], ctx_ptr->d[5], ctx_ptr->d[6], ctx_ptr->d[7]);
		printf("[SYSTEM-ERROR-REGS] A0=%08X A1=%08X A2=%08X A3=%08X A4=%08X A5=%08X A6=%08X A7=%08X\n",
		       ctx_ptr->a[0], ctx_ptr->a[1], ctx_ptr->a[2], ctx_ptr->a[3],
		       ctx_ptr->a[4], ctx_ptr->a[5], ctx_ptr->a[6], ctx_ptr->a[7]);
	}

	/* Bytes straddling the fault PC: distinguishes plausible-but-wrong
	 * relocated code (recognizable opcodes) from zeroed/garbage RAM, and
	 * lets repeated crashes be compared byte-for-byte across runs. Plain
	 * ReadMacInt8 -- a synchronous guest-RAM read, not a reentrant call
	 * into any CPU core -- so it's safe from inside exception dispatch.
	 * Vector 5 also dumps Time Manager calibration globals and a wider
	 * window so the TimeDBRA/DIVU.W D5 stub is visible. */
	if (vector == 5) {
		printf("[SYSTEM-ERROR-TIME] TimeDBRA=0x%04X TimeSCCDBRA=0x%04X TimeSCSIDBRA=0x%04X TimeRAMDBRA=0x%04X\n",
		       ReadMacInt16(0x0d00), ReadMacInt16(0x0d02),
		       ReadMacInt16(0x0b24), ReadMacInt16(0x0cea));
		printf("[SYSTEM-ERROR-MEM] bytes at PC-128..PC+47:");
		for (int off = -128; off < 48; off++)
			printf(" %02X", ReadMacInt8(pc + (uint32)off));
		printf("\n");
	} else {
		printf("[SYSTEM-ERROR-MEM] bytes at PC-16..PC+47:");
		for (int off = -16; off < 48; off++)
			printf(" %02X", ReadMacInt8(pc + (uint32)off));
		printf("\n");
	}

	cpu_engine_dump_pc_trace_once();

	cpu_engine_write_crash_dump(engine, vector, pc, ms_since_last_tick, ticks, ctx_ptr);
	s_exception_ctx.valid = 0;

	fflush(stdout);
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
		return 4;
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
 * Returns emulated nanoseconds from the active engine, or 0.
 *
 * Arguments: none.
 * Returns: Monotonic engine-local ns, or 0 when emulated_ns is NULL.
 */
uint64 cpu_engine_emulated_ns(void)
{
	if (s_active_engine && s_active_engine->emulated_ns)
		return s_active_engine->emulated_ns();
	return 0;
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
