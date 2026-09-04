/*
 *  m68k_rs_glue.cpp - Glue m68k-rs (Rust) CPU core to Basilisk II CPUEngine interface
 *
 *  CockatriceIII (C) 2026
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <time.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "main.h"
#include "prefs.h"
#include "emul_op.h"
#include "rom_patches.h"
#include "timer.h"
#include "ether.h"
#include "scsi.h"
#include "sony.h"
#include "disk.h"
#include "audio.h"
#include "menu_bar.h"
#include "macos_util.h"
#include "cockatrice_m68k_rs.h"

static jmp_buf s_cpu_reset_jmp;
static volatile bool s_cpu_reset_valid = false;
static bool s_quit_requested = false;
static M68kRsCpu *s_cpu = nullptr;

/*
 * FastMem mode selected from preferences (m68k_rs_fastmem):
 *   off    - disable direct window (callback-only memory path)
 *   ram    - expose RAM only (0..RAMSize), so holes/ROM/MMIO stay callbacked
 *   multi  - select one direct region per batch (RAM/ROM/framebuffer) based
 *            on current PC; accesses outside that region fall back callbacks
 *   legacy - expose 0..SCC (historic wide window for A/B testing)
 */
enum M68kRsFastMemMode {
	M68K_RS_FASTMEM_OFF = 0,
	M68K_RS_FASTMEM_RAM,
	M68K_RS_FASTMEM_MULTI,
	M68K_RS_FASTMEM_LEGACY
};
static M68kRsFastMemMode s_fastmem_mode = M68K_RS_FASTMEM_OFF;
static bool s_fastmem_forced_off = false;

/* Set from UseJIT in init: true selects m68k_rs_run_batch (decoded-op cache,
 * fastmem window, trace JIT), false keeps the cycle-accurate interpreter. */
static bool s_use_batch = false;

/* Opt-in throughput logging (M68K_RS_PERF_LOG=1) for FastMem A/B benchmarking;
 * prints instructions/sec every ~2s of wall clock instead of on every slice
 * so it stays cheap enough to run during a real timing measurement. */
static int s_perf_log_enabled = -1; /* -1 = unchecked, 0 = off, 1 = on */
static uint64 s_perf_total_instructions = 0;
static struct timespec s_perf_last_report;

static void m68k_rs_perf_note(uint32 instructions)
{
	if (s_perf_log_enabled < 0)
		s_perf_log_enabled = (getenv("M68K_RS_PERF_LOG") != nullptr) ? 1 : 0;
	if (!s_perf_log_enabled)
		return;

	s_perf_total_instructions += instructions;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (s_perf_last_report.tv_sec == 0 && s_perf_last_report.tv_nsec == 0)
		s_perf_last_report = now;

	double elapsed = (now.tv_sec - s_perf_last_report.tv_sec) +
	                  (now.tv_nsec - s_perf_last_report.tv_nsec) / 1e9;
	if (elapsed >= 2.0) {
		printf("[m68k-rs][PERF] instructions=%llu elapsed=%.2fs ips=%.0f\n",
		       (unsigned long long)s_perf_total_instructions, elapsed,
		       (double)s_perf_total_instructions / elapsed);
		fflush(stdout);
		s_perf_total_instructions = 0;
		s_perf_last_report = now;
	}
}

/*
 * Maps the m68k_rs_fastmem preference string onto an execution mode.
 *
 * Arguments:
 *   value: Preference string (off/ram/legacy), may be null/empty.
 *
 * Returns:
 *   Selected FastMem mode; unknown values fall back to OFF.
 */
static M68kRsFastMemMode m68k_rs_parse_fastmem_mode(const char *value)
{
	if (!value || !value[0])
		return M68K_RS_FASTMEM_OFF;
	if (strcmp(value, "ram") == 0)
		return M68K_RS_FASTMEM_RAM;
	if (strcmp(value, "multi") == 0)
		return M68K_RS_FASTMEM_MULTI;
	if (strcmp(value, "legacy") == 0)
		return M68K_RS_FASTMEM_LEGACY;
	return M68K_RS_FASTMEM_OFF;
}

/*
 * Chooses a Mac address that should lie in an unmapped hole when hole trapping
 * is configured correctly (between low RAM and ROM on 32-bit clean machines).
 *
 * Returns:
 *   Guest address used for memory_is_mapped() probing.
 */
static uint32 m68k_rs_hole_probe_addr(void)
{
	/* Keep probe away from RAM end and ROM start boundaries. */
	uint64 ram_end = (uint64)RAMBaseMac + (uint64)RAMSize;
	if (ROMBaseMac > ram_end + 0x2000ULL)
		return (uint32)(ram_end + 0x1000ULL);
	return 0x10000000U;
}

/*
 * Validates whether host memory layout exposes real unmapped holes.
 *
 * Returns:
 *   true when a known hole address is unmapped (fault-producing),
 *   false when the host maps it as ordinary memory.
 */
static bool m68k_rs_holes_visible(void)
{
	const uint32 probe = m68k_rs_hole_probe_addr();
	return !memory_is_mapped(probe, 1);
}

/*
 * Slice budgets. The cycle path is budgeted in CPU cycles and polls interrupts
 * from its per-instruction boundary hook; the batch path is budgeted in
 * instructions and polls between batches, so its budget also sets worst-case
 * interrupt latency. 16K instructions is well under a 60 Hz tick while still
 * amortising the batch entry cost across a long run of guest code.
 */
enum {
	M68K_RS_SLICE_CYCLES = 50000,
	M68K_RS_SLICE_INSNS = 65536, //16384,
	M68K_RS_NESTED_CYCLES = 5000,
	M68K_RS_NESTED_INSNS = 2048
};

/*
 * Writes trap callback results back into the live CPU, gating A7 like Musashi.
 */
static void m68k_rs_import_regs(const M68kRsRegs *regs, uint32 old_a7)
{
	for (int i = 0; i < 8; i++)
		m68k_rs_set_reg(s_cpu, (M68kRsReg)(M68K_RS_REG_D0 + i), regs->d[i]);
	for (int i = 0; i < 8; i++) {
		if (i == 7 && !cpu_engine_should_commit_a7(old_a7, regs->a[7]))
			continue;
		m68k_rs_set_reg(s_cpu, (M68kRsReg)(M68K_RS_REG_A0 + i), regs->a[i]);
	}
	m68k_rs_set_reg(s_cpu, M68K_RS_REG_SR, regs->sr);
}

static uint8 m68k_rs_host_read_byte(void *ctx, uint32 addr)
{
	(void)ctx;
	return ReadMacInt8(addr);
}

static uint16 m68k_rs_host_read_word(void *ctx, uint32 addr)
{
	(void)ctx;
	return ReadMacInt16(addr);
}

static uint32 m68k_rs_host_read_long(void *ctx, uint32 addr)
{
	(void)ctx;
	return ReadMacInt32(addr);
}

static void m68k_rs_host_write_byte(void *ctx, uint32 addr, uint8 val)
{
	(void)ctx;
	WriteMacInt8(addr, val);
}

static void m68k_rs_host_write_word(void *ctx, uint32 addr, uint16 val)
{
	(void)ctx;
	WriteMacInt16(addr, val);
}

static void m68k_rs_host_write_long(void *ctx, uint32 addr, uint32 val)
{
	(void)ctx;
	WriteMacInt32(addr, val);
}

/*
 * Handles Basilisk EmulOp illegal opcodes and the Execute68k return sentinel.
 */
static int m68k_rs_host_handle_illegal(void *ctx, uint16 opcode, M68kRsRegs *io_regs)
{
	(void)ctx;

	if (opcode == M68K_EXEC_RETURN) {
		TriggerExecutionReturn();
		return 1;
	}

	if (opcode > M68K_EXEC_RETURN && opcode < M68K_EMUL_OP_MAX) {
		uint32 old_a7 = m68k_rs_get_reg(s_cpu, M68K_RS_REG_A7);
		EmulOp(opcode, (struct M68kRegisters *)io_regs);
		m68k_rs_import_regs(io_regs, old_a7);
		return 1;
	}

	if (opcode >= 0x7000 && opcode < 0x7200) {
		printf("[EMUL-OP] Unhandled/Unknown EmulOp 0x%04X at PC=0x%08X (A7=0x%08X)\n",
		       opcode,
		       (uint32)m68k_rs_get_reg(s_cpu, M68K_RS_REG_PC),
		       (uint32)m68k_rs_get_reg(s_cpu, M68K_RS_REG_A7));
		fflush(stdout);
	}

	return 0;
}

/*
 * Handles Line-A toolbox traps used by Execute68kTrap stubs.
 */
static int m68k_rs_host_handle_aline(void *ctx, uint16 opcode, M68kRsRegs *io_regs)
{
	(void)ctx;
	(void)opcode;
	(void)io_regs;
	/* Toolbox traps run to completion via guest ROM; let hardware deliver them. */
	return 0;
}

#ifdef M68K_RS_BATCH_TRACE
/*
 * Prints the post-retirement register state for the first M68K_RS_TRACE_PC
 * instructions so the batch and cycle paths can be diffed against each other.
 */
static long s_trace_left = -1;

static void m68k_rs_trace_step(void)
{
	if (s_trace_left < 0) {
		const char *n = getenv("M68K_RS_TRACE_PC");
		s_trace_left = n ? atol(n) : 0;
	}
	if (s_trace_left == 0)
		return;
	s_trace_left--;
	printf("[TR] pc=%08X sr=%04X d3=%08X a2=%08X a3=%08X a6=%08X a7=%08X\n",
	       (uint32)m68k_rs_get_reg(s_cpu, M68K_RS_REG_PC),
	       (uint32)m68k_rs_get_reg(s_cpu, M68K_RS_REG_SR),
	       (uint32)m68k_rs_get_reg(s_cpu, M68K_RS_REG_D3),
	       (uint32)m68k_rs_get_reg(s_cpu, M68K_RS_REG_A2),
	       (uint32)m68k_rs_get_reg(s_cpu, M68K_RS_REG_A3),
	       (uint32)m68k_rs_get_reg(s_cpu, M68K_RS_REG_A6),
	       (uint32)m68k_rs_get_reg(s_cpu, M68K_RS_REG_A7));
}
#endif

/*
 * Polls Basilisk interrupt sources and records the guest PC each instruction.
 */
static void m68k_rs_host_boundary_hook(void *ctx, uint32 cycles)
{
	(void)ctx;
	(void)cycles;
	cpu_engine_note_pc(m68k_rs_get_reg(s_cpu, M68K_RS_REG_PC));
#ifdef M68K_RS_BATCH_TRACE
	m68k_rs_trace_step();
#endif
}

static int m68k_rs_host_get_irq(void *ctx)
{
	(void)ctx;
	return cpu_engine_intlev();
}

/*
 * Builds the committed-range table pushed to the Rust bus so checked memory
 * accesses (outside FastMem) can validate locally instead of calling back
 * into the host per access. Mirrors the legality rule m68k_rs_host_is_mapped()
 * used to apply per-call: RAM/ROM/framebuffer commits, plus every registered
 * MMIO region (see RegisterMMIORegion() in cpu_emulation.h), which is always
 * legal even though it is not host-committed pages. A future MMIO device is
 * picked up here automatically once registered.
 */
static void m68k_rs_push_mapped_ranges(void)
{
	uint32 starts[M68K_RS_MAX_MAPPED_RANGES];
	uint32 ends[M68K_RS_MAX_MAPPED_RANGES];
	int n = memory_get_mapped_ranges(starts, ends, M68K_RS_MAX_MAPPED_RANGES);

	for (int i = 0; i < g_mmio_region_count && n < M68K_RS_MAX_MAPPED_RANGES; i++) {
		starts[n] = g_mmio_regions[i].base;
		ends[n] = g_mmio_regions[i].base + g_mmio_regions[i].length;
		n++;
	}

	m68k_rs_set_mapped_ranges(s_cpu, starts, ends, (uint32_t)n, 0);
}

/*
 * Publishes the flat Macintosh RAM window to the batch executor.
 *
 * UAE routes every access through memory_get_* → ReadMacInt/WriteMacInt so
 * SCC MMIO and ROM write suppression stay in effect. m68k-rs FastMem is a
 * single direct window; when it covers RAM (or ROM) the batch executor can
 * bypass those hooks and we have seen silent corruption/stalls during heavy
 * SCSI (17k READs then hang) while the same build with FastMem disabled
 * reaches full boot (~27k READs in 45s).
 *
 * Leave the window off until the vendor fastmem path is audited; set
 * M68K_RS_FASTMEM=1 to experiment with the layout below.
 *
 * Intended layout when enabled (32-bit):
 *   base 0, len RAMSize — RAM only; ROM/framebuffer/SCC use callbacks.
 * 24-bit: len 0x900000 (below SCC mirrors; Mac2HostAddr masking stays
 * on the callback path).
 */
static uint8 *m68k_rs_host_fast_mem(void *ctx, uint32 *base, uint32 *len)
{
	uint32 pc;
	uint32 rom_window;
	uint32 fb_end;

	(void)ctx;
	if (!Host_Mem_Base)
		return nullptr;
	if (s_fastmem_mode == M68K_RS_FASTMEM_OFF)
		return nullptr;
	*base = 0;
	if (s_fastmem_mode == M68K_RS_FASTMEM_LEGACY) {
		/* Historic broad window; kept only for targeted A/B tests. */
		*len = 0x50000000U;
	} else if (s_fastmem_mode == M68K_RS_FASTMEM_MULTI && s_cpu) {
		/*
		 * Multi-region selection without changing the Rust FastMem ABI:
		 * publish one contiguous region per batch, choosing the region that
		 * currently contains PC. This keeps direct fetches in hot code
		 * regions while preserving callback semantics outside that window.
		 */
		pc = m68k_rs_get_reg(s_cpu, M68K_RS_REG_PC);
		rom_window = (ROMSize > 0x00800000U) ? ROMSize : 0x00800000U;
		if (pc >= ROMBaseMac && pc < ROMBaseMac + rom_window) {
			*base = ROMBaseMac;
			*len = rom_window;
		} else if (MacFrameLayout != FLAYOUT_NONE && MacFrameSize > 0) {
			fb_end = MacFrameBaseMac + MacFrameSize;
			if (fb_end > MacFrameBaseMac && pc >= MacFrameBaseMac && pc < fb_end) {
				*base = MacFrameBaseMac;
				*len = MacFrameSize;
			} else if (RAMSize > 0) {
				*len = RAMSize;
			} else if (ROMBaseMac != 0) {
				*len = ROMBaseMac;
			} else {
				*len = 0x50000000U;
			}
		} else if (RAMSize > 0) {
			*len = RAMSize;
		} else if (ROMBaseMac != 0) {
			*len = ROMBaseMac;
		} else {
			*len = 0x50000000U;
		}
	} else if (RAMSize > 0) {
		*len = RAMSize;
	} else if (ROMBaseMac != 0) {
		*len = ROMBaseMac;
	} else {
		*len = 0x50000000U;
	}
	return Host_Mem_Base;
}

/*
 * Runs one slice, stopping early when the host requests exit or an unhandled trap fires.
 *
 * Arguments:
 *   cycles: budget for the cycle-accurate interpreter path.
 *   instructions: budget for the batch path (also its interrupt poll interval).
 */
/*
 * Every vector dispatch (interrupts and A-line/Toolbox trap dispatch
 * included) sets the core's last-exception-vector field, so this filters to
 * the small set that indicates a genuine fault before logging -- see
 * cockatrice_report_cpu_exception() in cpu_engine.cpp for the shared,
 * engine-independent "did Mac OS just bomb" rationale.
 */
static void m68k_rs_check_exception_vector(void)
{
	int32_t vector = m68k_rs_take_last_exception_vector(s_cpu);
	if (vector < 0)
		return;
	bool reportable = (vector >= 2 && vector <= 8) || vector == 11;
	if (reportable)
		cockatrice_report_cpu_exception("m68k_rs", vector, m68k_rs_get_reg(s_cpu, M68K_RS_REG_PC));
}

static void m68k_rs_run_slice(int32 cycles, uint32 instructions)
{
#ifdef M68K_RS_BATCH_TRACE
	/* One-instruction batches while tracing so batch and cycle paths emit
	 * comparable per-instruction [TR] lines. */
	const char *trace_env = getenv("M68K_RS_TRACE_PC");
	const bool tracing = trace_env && trace_env[0] != '\0' && trace_env[0] != '0';
	if (s_use_batch && tracing)
		instructions = 1;
#endif
	for (;;) {
		M68kRsRunResult result;
		if (s_use_batch) {
			/* No per-instruction boundary hook on this path: sample the
			 * interrupt level and the heartbeat PC per batch instead. */
			m68k_rs_set_irq(s_cpu, cpu_engine_intlev());
			result = m68k_rs_run_batch(s_cpu, instructions);
			cpu_engine_note_pc(m68k_rs_get_reg(s_cpu, M68K_RS_REG_PC));
#ifdef M68K_RS_BATCH_TRACE
			if (tracing)
				m68k_rs_trace_step();
			else {
				static int dbg_n = 0;
				if (dbg_n < 200) {
					dbg_n++;
					printf("[RSDBG] batch exit=%d insns=%u pc=%08X sr=%04X a7=%08X\n",
					       (int)result.exit, result.instructions,
					       (uint32)m68k_rs_get_reg(s_cpu, M68K_RS_REG_PC),
					       (uint32)m68k_rs_get_reg(s_cpu, M68K_RS_REG_SR),
					       (uint32)m68k_rs_get_reg(s_cpu, M68K_RS_REG_A7));
					fflush(stdout);
				}
			}
#endif
		} else {
			result = m68k_rs_run_cycles(s_cpu, cycles);
		}
		m68k_rs_check_exception_vector();
		m68k_rs_perf_note(result.instructions);
		if (s_quit_requested)
			return;
		if (result.exit == M68K_RS_EXIT_HALTED) {
			printf("[m68k-rs] CPU halted\n");
			fflush(stdout);
			return;
		}
		if (result.exit != M68K_RS_EXIT_BUDGET)
			return;
		if (IsExecutionReturnTriggered())
			return;
	}
}

static M68kRsCpuType m68k_rs_map_cpu_type(void)
{
	switch (CPUType) {
		case 0: return M68K_RS_CPU_68000;
		case 1: return M68K_RS_CPU_68010;
		case 2: return M68K_RS_CPU_68020;
		case 3: return M68K_RS_CPU_68030;
		case 4:
		default:
			return M68K_RS_CPU_68040;
	}
}

static bool m68k_rs_engine_init(void)
{
	if (!cpu_engine_map_rom_base())
		return false;

	s_quit_requested = false;

	if (!s_cpu) {
		M68kRsHostCallbacks callbacks = {};
		callbacks.read_byte = m68k_rs_host_read_byte;
		callbacks.read_word = m68k_rs_host_read_word;
		callbacks.read_long = m68k_rs_host_read_long;
		callbacks.write_byte = m68k_rs_host_write_byte;
		callbacks.write_word = m68k_rs_host_write_word;
		callbacks.write_long = m68k_rs_host_write_long;
		callbacks.handle_illegal = m68k_rs_host_handle_illegal;
		callbacks.handle_aline = m68k_rs_host_handle_aline;
		callbacks.boundary_hook = m68k_rs_host_boundary_hook;
		callbacks.get_irq = m68k_rs_host_get_irq;
		callbacks.fast_mem = m68k_rs_host_fast_mem;
		callbacks.host_ctx = nullptr;
		s_cpu = m68k_rs_create(&callbacks);
		if (!s_cpu)
			return false;
	}

	s_use_batch = UseJIT;
	s_fastmem_mode = m68k_rs_parse_fastmem_mode(PrefsFindString("m68k_rs_fastmem"));
	s_fastmem_forced_off = false;
	/*
	 * Host window policy for this engine instance:
	 * - Batch + FastMem requested: require strict holes so out-of-range
	 *   accesses fault instead of reading dummy zeros.
	 * - Otherwise keep the historical flat dummy map.
	 */
	memory_set_flat_dummy_window(!(s_use_batch && s_fastmem_mode != M68K_RS_FASTMEM_OFF));
	memory_init();
	m68k_rs_push_mapped_ranges();
	/*
	 * Guard rail:
	 * - FastMem only applies to the batch/JIT path.
	 * - If host memory does not expose real holes, FastMem can turn expected
	 *   bus-error probes into zero/garbage reads and break boot (Error 10 path).
	 */
	if (!s_use_batch && s_fastmem_mode != M68K_RS_FASTMEM_OFF) {
		s_fastmem_mode = M68K_RS_FASTMEM_OFF;
		s_fastmem_forced_off = true;
	}
	if (s_use_batch && s_fastmem_mode != M68K_RS_FASTMEM_OFF && !m68k_rs_holes_visible()) {
		s_fastmem_mode = M68K_RS_FASTMEM_OFF;
		s_fastmem_forced_off = true;
	}
	printf("[m68k-rs] execution path: %s (cranelift %s)\n",
	       s_use_batch ? "batch/JIT" : "cycle-accurate interpreter",
	       m68k_rs_jit_available() ? "compiled in" : "not compiled in");
	if (s_use_batch && m68k_rs_jit_available()) {
		/*
		 * Forces cranelift-jit's JITBuilder::new() to run now, on this
		 * thread, so a runtime failure to allocate executable memory
		 * (silently swallowed upstream, falling back to the portable
		 * trace executor) is visible in the boot log instead of just
		 * showing up as unexplained slowness later.
		 */
		printf("[m68k-rs] cranelift native codegen: %s\n",
		       m68k_rs_jit_native_active() ? "active" : "unavailable (falling back to the portable trace executor)");
	}
	printf("[m68k-rs] fastmem mode: %s\n",
	       s_fastmem_mode == M68K_RS_FASTMEM_RAM ? "ram" :
	       s_fastmem_mode == M68K_RS_FASTMEM_MULTI ? "multi" :
	       s_fastmem_mode == M68K_RS_FASTMEM_LEGACY ? "legacy" : "off");
	if (s_fastmem_forced_off) {
		printf("[m68k-rs] fastmem guard rail: forced off (jit=%s, holes=%s)\n",
		       s_use_batch ? "on" : "off",
		       m68k_rs_holes_visible() ? "visible" : "collapsed");
	}
	fflush(stdout);

	m68k_rs_pulse_reset(s_cpu);
	return m68k_rs_init(s_cpu, m68k_rs_map_cpu_type()) != 0;
}

static void m68k_rs_engine_exit(void)
{
	s_quit_requested = true;
	if (s_cpu)
		m68k_rs_request_stop(s_cpu);
}

static void m68k_rs_engine_start(void)
{
	s_quit_requested = false;
	for (;;) {
		if (setjmp(s_cpu_reset_jmp) == 0) {
			s_cpu_reset_valid = true;

			m68k_rs_pulse_reset(s_cpu);
			m68k_rs_set_reg(s_cpu, M68K_RS_REG_A7, CPU_ENGINE_BOOT_SP);
			m68k_rs_set_reg(s_cpu, M68K_RS_REG_PC, ROMBaseMac + CPU_ENGINE_BOOT_PC_OFF);
			m68k_rs_set_reg(s_cpu, M68K_RS_REG_SR, CPU_ENGINE_BOOT_SR);
			m68k_rs_invalidate_prefetch(s_cpu);

			while (!s_quit_requested)
				m68k_rs_run_slice(M68K_RS_SLICE_CYCLES, M68K_RS_SLICE_INSNS);
			break;
		} else {
			printf("Reset680x0: Resetting machine subsystems...\n");
			fflush(stdout);
			cpu_engine_reset_peripherals();
			s_quit_requested = false;
			MenuBar_UpdateAll();
		}
	}
	s_cpu_reset_valid = false;
}

static void m68k_rs_engine_reset(void)
{
	if (s_cpu_reset_valid)
		longjmp(s_cpu_reset_jmp, 1);
}

static int m68k_rs_engine_intlev(void)
{
	return cpu_engine_intlev();
}

static void m68k_rs_engine_trigger_interrupt(void)
{
	m68k_rs_set_irq(s_cpu, cpu_engine_intlev());
}

static void m68k_rs_engine_trigger_nmi(void)
{
	m68k_rs_set_irq(s_cpu, 7);
}

static void m68k_rs_engine_execute_68k_trap(uint16 trap, struct M68kRegisters *r)
{
	uint32 oldpc = m68k_rs_get_reg(s_cpu, M68K_RS_REG_PC);

	for (int i = 0; i < 8; i++)
		m68k_rs_set_reg(s_cpu, (M68kRsReg)(M68K_RS_REG_D0 + i), r->d[i]);
	for (int i = 0; i < 7; i++)
		m68k_rs_set_reg(s_cpu, (M68kRsReg)(M68K_RS_REG_A0 + i), r->a[i]);

	uint32 sp = cpu_engine_clamp_sp(m68k_rs_get_reg(s_cpu, M68K_RS_REG_A7));
	uint32 stub = cpu_engine_write_trap_stub(sp, trap);
	m68k_rs_set_reg(s_cpu, M68K_RS_REG_A7, stub);
	m68k_rs_set_reg(s_cpu, M68K_RS_REG_PC, stub);
	m68k_rs_invalidate_prefetch(s_cpu);

	bool return_seen = false;
	PushReturnStack(&return_seen);
	while (!return_seen && !s_quit_requested)
		m68k_rs_run_slice(M68K_RS_NESTED_CYCLES, M68K_RS_NESTED_INSNS);
	PopReturnStack();

	sp = m68k_rs_get_reg(s_cpu, M68K_RS_REG_A7) + 4;
	m68k_rs_set_reg(s_cpu, M68K_RS_REG_A7, sp);
	m68k_rs_set_reg(s_cpu, M68K_RS_REG_PC, oldpc);

	for (int i = 0; i < 8; i++)
		r->d[i] = m68k_rs_get_reg(s_cpu, (M68kRsReg)(M68K_RS_REG_D0 + i));
	for (int i = 0; i < 7; i++)
		r->a[i] = m68k_rs_get_reg(s_cpu, (M68kRsReg)(M68K_RS_REG_A0 + i));
}

static void m68k_rs_engine_execute_68k(uint32 addr, struct M68kRegisters *r)
{
	uint32 oldpc = m68k_rs_get_reg(s_cpu, M68K_RS_REG_PC);

	for (int i = 0; i < 8; i++)
		m68k_rs_set_reg(s_cpu, (M68kRsReg)(M68K_RS_REG_D0 + i), r->d[i]);
	for (int i = 0; i < 7; i++)
		m68k_rs_set_reg(s_cpu, (M68kRsReg)(M68K_RS_REG_A0 + i), r->a[i]);

	uint32 sp = cpu_engine_clamp_sp(m68k_rs_get_reg(s_cpu, M68K_RS_REG_A7));
	uint32 ret_addr = 0;
	sp = cpu_engine_write_exec_return_frame(sp, &ret_addr);
	m68k_rs_set_reg(s_cpu, M68K_RS_REG_A7, sp);
	m68k_rs_set_reg(s_cpu, M68K_RS_REG_PC, addr);
	m68k_rs_invalidate_prefetch(s_cpu);

	bool return_seen = false;
	PushReturnStack(&return_seen);
	while (!return_seen && !s_quit_requested)
		m68k_rs_run_slice(M68K_RS_NESTED_CYCLES, M68K_RS_NESTED_INSNS);
	PopReturnStack();

	sp = m68k_rs_get_reg(s_cpu, M68K_RS_REG_A7) + 2;
	m68k_rs_set_reg(s_cpu, M68K_RS_REG_A7, sp);
	m68k_rs_set_reg(s_cpu, M68K_RS_REG_PC, oldpc);

	for (int i = 0; i < 8; i++)
		r->d[i] = m68k_rs_get_reg(s_cpu, (M68kRsReg)(M68K_RS_REG_D0 + i));
	for (int i = 0; i < 7; i++)
		r->a[i] = m68k_rs_get_reg(s_cpu, (M68kRsReg)(M68K_RS_REG_A0 + i));
}

extern const CPUEngine m68k_rs_cpu_engine = {
	"m68k_rs",
	"m68k-rs (Rust interpreter)",
	false,
	CPU_MEM_STRATEGY_CALLBACK,
	CPU_ENGINE_TIER_PERFORMANCE,
	m68k_rs_engine_init,
	m68k_rs_engine_exit,
	m68k_rs_engine_start,
	m68k_rs_engine_reset,
	m68k_rs_engine_execute_68k,
	m68k_rs_engine_execute_68k_trap,
	m68k_rs_engine_trigger_interrupt,
	m68k_rs_engine_trigger_nmi,
	m68k_rs_engine_intlev,
	nullptr
};
