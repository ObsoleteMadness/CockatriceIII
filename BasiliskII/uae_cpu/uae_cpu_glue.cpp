/*
 *  uae_cpu_glue.cpp - uae-portable-cpu 680x0 engine adapter for Cockatrice III
 *
 *  Prefs `cpu_emulator uaecpu` selects the vendored uae-portable-cpu core
 *  (BasiliskII/vendor/uae-portable-cpu), driven entirely through its public
 *  uae_cpu_* API. This is the GPL-2 replacement for the GPL-3 Amiberry
 *  engine in ../amiberry; both are registered while the two are compared.
 *
 *  The core is linked with UAE_CPU_MUSASHI_API=OFF: its Musashi-compatible
 *  m68k_* API would otherwise collide with the 27 same-named symbols this
 *  binary already gets from ../Musashi.
 */

#include <stdio.h>
#include <string.h>
#include <setjmp.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "emul_op.h"
#include "main.h"
#include "macos_util.h"
#include "menu_bar.h"
#include "prefs.h"

#include "uae_cpu.h"

static jmp_buf s_reset_jmp;
static volatile bool s_reset_valid = false;
static bool s_quit_requested = false;
static uae_cpu_t *s_cpu = NULL;

/*
 * Cycle budget per uae_cpu_execute() call. The top-level loop re-checks the
 * quit flag and publishes the guest PC between slices; a nested Execute68k
 * keeps a budget too, so a runaway subroutine still returns to C++.
 *
 * These are deliberately large. An installed get_irq hook makes the core arm
 * an interrupt check at the start of every execute call, so the budget sets
 * the re-arm rate: with a 10000-cycle nested budget the CPU suite's
 * interrupt-stress test took 2.87M interrupts against Amiberry's 624 for the
 * same workload, because Amiberry runs a nested Execute68k as one unbounded
 * slice (m68k_run_interpreter_slice) rather than a stream of short ones. A
 * call still returns as soon as uae_cpu_end_timeslice() fires, which is how
 * the EXEC_RETURN opcode ends a nested run, so a large budget costs nothing
 * in the normal case and only bounds a subroutine that never returns.
 */
enum {
	UAE_SLICE_CYCLES = 1000000,
	UAE_NESTED_CYCLES = 1000000
};

/*
 * MMIO accessors. ReadMacInt* / WriteMacInt* already dispatch through
 * FindMMIORegion(), so routing the core's custom-region callbacks at them
 * reuses Cockatrice's own SCC decode (and its ROM write suppression) rather
 * than duplicating the register decode here.
 */
static uint8_t uaecpu_mmio_r8(void *ud, uint32_t addr) { (void)ud; return (uint8_t)ReadMacInt8(addr); }
static uint16_t uaecpu_mmio_r16(void *ud, uint32_t addr) { (void)ud; return (uint16_t)ReadMacInt16(addr); }
static uint32_t uaecpu_mmio_r32(void *ud, uint32_t addr) { (void)ud; return ReadMacInt32(addr); }
static void uaecpu_mmio_w8(void *ud, uint32_t addr, uint8_t v) { (void)ud; WriteMacInt8(addr, v); }
static void uaecpu_mmio_w16(void *ud, uint32_t addr, uint16_t v) { (void)ud; WriteMacInt16(addr, v); }
static void uaecpu_mmio_w32(void *ud, uint32_t addr, uint32_t v) { (void)ud; WriteMacInt32(addr, v); }

/*
 * Handles the Basilisk II EmulOp range and the execution-return opcode.
 *
 * uae_cpu_reserve_opcodes() routes 0x7100..M68K_EMUL_OP_MAX-1 here before the
 * core raises vector 4, so these never reach illegal-instruction processing.
 *
 * Returns:
 *   1 when the opcode was handled (the core resumes at pc + 2), 0 to let the
 *   core take the illegal-instruction exception.
 */
static int uaecpu_on_illegal(void *ud, uint16_t opcode, uint32_t pc)
{
	(void)ud;

	if (opcode == (uint16_t)M68K_EXEC_RETURN) {
		TriggerExecutionReturn();
		uae_cpu_end_timeslice(s_cpu);
		return 1;
	}

	if (opcode > (uint16_t)M68K_EXEC_RETURN && opcode < (uint16_t)M68K_EMUL_OP_MAX) {
		struct M68kRegisters r;

		for (int i = 0; i < 8; i++) {
			r.d[i] = uae_cpu_get_reg(s_cpu, (uae_reg_t)(UAE_REG_D0 + i));
			r.a[i] = uae_cpu_get_reg(s_cpu, (uae_reg_t)(UAE_REG_A0 + i));
		}
		r.sr = (uint16)uae_cpu_get_reg(s_cpu, UAE_REG_SR);

		EmulOp(opcode, &r);

		/* Unconditional A7 writeback, as the Musashi and Amiberry paths do:
		 * the RESET EmulOp relies on being able to move A7 itself. */
		for (int i = 0; i < 8; i++) {
			uae_cpu_set_reg(s_cpu, (uae_reg_t)(UAE_REG_D0 + i), r.d[i]);
			uae_cpu_set_reg(s_cpu, (uae_reg_t)(UAE_REG_A0 + i), r.a[i]);
		}
		uae_cpu_set_reg(s_cpu, UAE_REG_SR, r.sr);
		return 1;
	}

	if (opcode >= 0x7000 && opcode < 0x7200) {
		printf("[EMUL-OP] Unhandled EmulOp 0x%04X at PC=0x%08X\n", opcode, pc);
		fflush(stdout);
	}
	return 0;
}

/*
 * Reports 680x0 fault vectors to the shared crash logger. Interrupts and
 * trap vectors are normal traffic and are not reported.
 */
static void uaecpu_on_exception(void *ud, const uae_cpu_exception_info_t *info)
{
	uint32 d[8], a[8];

	(void)ud;
	if (info->interrupt)
		return;
	switch (info->vector) {
		case 2: case 3: case 4: case 5:
		case 6: case 7: case 8: case 11:
			break;
		default:
			return;
	}

	for (int i = 0; i < 8; i++) {
		d[i] = uae_cpu_get_reg(s_cpu, (uae_reg_t)(UAE_REG_D0 + i));
		a[i] = uae_cpu_get_reg(s_cpu, (uae_reg_t)(UAE_REG_A0 + i));
	}
	cockatrice_set_cpu_exception_context("uaecpu", info->opcode, info->fault_pc,
	                                     info->current_pc, info->sr, d, a);
	cockatrice_report_cpu_exception("uaecpu", info->vector, info->fault_pc);
}

static int uaecpu_on_get_irq(void *ud)
{
	(void)ud;
	return cpu_engine_intlev();
}

static uae_cpu_type_t uaecpu_map_cpu_type(void)
{
	switch (CPUType) {
		case 0: return UAE_CPU_TYPE_68000;
		case 1: return UAE_CPU_TYPE_68010;
		case 2: return UAE_CPU_TYPE_68020;
		case 3: return UAE_CPU_TYPE_68030;
		case 4:
		default:
			return UAE_CPU_TYPE_68040;
	}
}

static uae_fpu_type_t uaecpu_map_fpu_type(void)
{
	if (!FPUType)
		return UAE_FPU_NONE;
	return (CPUType >= 4) ? UAE_FPU_68040 : UAE_FPU_68882;
}

/*
 * Describes Macintosh memory to the core.
 *
 * RAM, ROM and the framebuffer are host-committed inside the flat
 * Host_Mem_Base window, so they map as direct regions: with
 * jit_direct_memory the JIT then reads and writes them inline, and ROM stays
 * executable-but-not-writable (the core drops writes to an ABFLAG_ROM bank on
 * both the handler and the JIT path). Every registered MMIO window becomes a
 * custom region, so a device added later through RegisterMMIORegion() is
 * picked up here without another edit.
 */
static void uaecpu_map_memory(void)
{
	/*
	 * Back the whole 32-bit window first, the way Amiberry's dummy bank did:
	 * Mac2HostAddr() is Host_Mem_Base + addr for every address, and host code
	 * (ROM patches, test harnesses, drivers) writes through it directly, so a
	 * hole the guest touches has to resolve to the same host memory instead of
	 * dropping the write. Deliberately not UAE_MEM_JIT_DIRECT: as in Amiberry,
	 * translated code calls the handlers for a hole rather than inlining an
	 * access that might land in I/O space, and the handlers still resolve to
	 * base + address. The real regions below overwrite these bank entries.
	 *
	 * Two halves because uae_cpu_map_memory() takes a uint32_t size.
	 */
	uae_cpu_map_memory(s_cpu, 0x00000000u, 0x80000000u,
	                   Host_Mem_Base, UAE_MEM_RAM);
	uae_cpu_map_memory(s_cpu, 0x80000000u, 0x80000000u,
	                   Host_Mem_Base + 0x80000000u, UAE_MEM_RAM);

	uae_cpu_map_memory(s_cpu, RAMBaseMac, RAMSize, Host_Mem_Base + RAMBaseMac,
	                   UAE_MEM_RAM | UAE_MEM_CACHEABLE | UAE_MEM_JIT_DIRECT);
	uae_cpu_map_memory(s_cpu, ROMBaseMac, ROMSize, Host_Mem_Base + ROMBaseMac,
	                   UAE_MEM_ROM | UAE_MEM_CACHEABLE | UAE_MEM_JIT_DIRECT);
	if (MacFrameSize > 0) {
		/* A MOVE16/MOVEM burst can run off the end of the framebuffer. */
		uae_cpu_map_memory(s_cpu, MacFrameBaseMac, MacFrameSize,
		                   Host_Mem_Base + MacFrameBaseMac,
		                   UAE_MEM_RAM | UAE_MEM_JIT_DIRECT | UAE_MEM_JIT_UNSAFE_BURST);
	}

	for (int i = 0; i < g_mmio_region_count; i++) {
		uae_cpu_map_custom(s_cpu, g_mmio_regions[i].base, g_mmio_regions[i].length,
		                   uaecpu_mmio_r8, uaecpu_mmio_r16, uaecpu_mmio_r32,
		                   uaecpu_mmio_w8, uaecpu_mmio_w16, uaecpu_mmio_w32, NULL);
	}
}

static bool uaecpu_init(void)
{
	uae_cpu_config_t cfg;
	uae_cpu_host_hooks_t hooks;

	if (!cpu_engine_map_rom_base())
		return false;

	s_quit_requested = false;
	memory_init();

	uae_cpu_global_init();
	if (!s_cpu) {
		s_cpu = uae_cpu_create(NULL);
		if (!s_cpu)
			return false;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.cpu_type = uaecpu_map_cpu_type();
	cfg.fpu_type = uaecpu_map_fpu_type();
	/* jit_fpu needs host doubles; SoftFloat otherwise for full 80-bit results. */
	cfg.fpu_softfloat = !UseJITFPU;
	cfg.unmapped_bus_error = false;
	cfg.jit_enabled = UseJIT;
	cfg.jit_cache_size = JITCacheSize;
	/* Mac OS leaves the 68040 caches enabled; translate from the first block
	 * instead of waiting for a CACR write that a warm boot never repeats. */
	cfg.jit_follow_cacr = false;
	cfg.jit_direct_memory = true;
	cfg.jit_fpu = UseJITFPU;
	/* Cockatrice reports its own code writes through FlushCodeCache, but the
	 * guest also patches code and announces it with CPUSHA/CINVA only. */
	cfg.jit_ignore_guest_cache_flush = false;
	uae_cpu_set_config(s_cpu, &cfg);

	uaecpu_map_memory();
	if (uae_cpu_set_jit_memory_base(s_cpu, Host_Mem_Base) != 0 && UseJIT) {
		printf("[uaecpu] JIT memory base rejected; translation disabled\n");
		fflush(stdout);
	}

	memset(&hooks, 0, sizeof(hooks));
	hooks.illegal = uaecpu_on_illegal;
	hooks.exception = uaecpu_on_exception;
	hooks.get_irq = uaecpu_on_get_irq;
	uae_cpu_set_host_hooks(s_cpu, &hooks);

	/* EmulOps are not illegal instructions: claim the range up front so the
	 * core never builds an exception frame for them. */
	if (uae_cpu_reserve_opcodes(s_cpu, (uint16_t)M68K_EXEC_RETURN,
	                            (uint16_t)(M68K_EMUL_OP_MAX - 1)) != 0) {
		printf("[uaecpu] could not reserve the EmulOp opcode range\n");
		return false;
	}

	uae_cpu_reset(s_cpu);
	printf("[uaecpu] uae-portable-cpu 680%d0, fpu %s, jit %s\n",
	       CPUType, FPUType ? "on" : "off",
	       UseJIT ? (UseJITFPU ? "on+fpu" : "on") : "off");
	fflush(stdout);
	return true;
}

static void uaecpu_exit(void)
{
	s_quit_requested = true;
	if (s_cpu)
		uae_cpu_end_timeslice(s_cpu);
}

static void uaecpu_start(void)
{
	s_quit_requested = false;
	for (;;) {
		if (setjmp(s_reset_jmp) == 0) {
			s_reset_valid = true;

			uae_cpu_reset(s_cpu);
			uae_cpu_set_reg(s_cpu, UAE_REG_A7, CPU_ENGINE_BOOT_SP);
			uae_cpu_set_reg(s_cpu, UAE_REG_PC, ROMBaseMac + CPU_ENGINE_BOOT_PC_OFF);
			uae_cpu_set_reg(s_cpu, UAE_REG_SR, CPU_ENGINE_BOOT_SR);

			while (!s_quit_requested) {
				cpu_engine_note_pc(uae_cpu_get_pc(s_cpu));
				uae_cpu_execute(s_cpu, UAE_SLICE_CYCLES);
			}
			break;
		} else {
			printf("Reset680x0 (uaecpu): Resetting machine subsystems...\n");
			fflush(stdout);
			cpu_engine_reset_peripherals();
			s_quit_requested = false;
			MenuBar_UpdateAll();
		}
	}
	s_reset_valid = false;
}

static void uaecpu_reset(void)
{
	if (s_reset_valid)
		longjmp(s_reset_jmp, 1);
}

static int uaecpu_intlev(void)
{
	return cpu_engine_intlev();
}

static void uaecpu_trigger_interrupt(void)
{
	/* Thread-safe: re-samples the level through the get_irq hook. */
	uae_cpu_signal_irq(s_cpu);
}

static void uaecpu_trigger_nmi(void)
{
	uae_cpu_set_irq(s_cpu, 7);
}

/*
 * Discards translations covering guest code a host driver just rewrote.
 */
static void uaecpu_invalidate_code(uint32 addr, uint32 size)
{
	if (s_cpu)
		uae_cpu_invalidate_code(s_cpu, addr, size);
}

/*
 * Reports emulated time for the Time Manager as 40 MHz nanoseconds, matching
 * the rate the Amiberry engine reported.
 */
static uint64 uaecpu_emulated_ns(void)
{
	if (!s_cpu)
		return 0;
	return uae_cpu_get_cycles(s_cpu) * 25ULL;
}

static uint32 uaecpu_get_pc(void)
{
	if (!s_cpu)
		return 0;
	return (uint32)uae_cpu_get_pc(s_cpu);
}

/*
 * Runs a nested 68k subroutine, returning when it hits the planted
 * M68K_EXEC_RETURN word.
 */
static void uaecpu_execute_68k(uint32 addr, struct M68kRegisters *r)
{
	uint32 oldpc = uae_cpu_get_pc(s_cpu);
	uint32 sp, ret_addr = 0;
	bool return_seen = false;

	for (int i = 0; i < 8; i++)
		uae_cpu_set_reg(s_cpu, (uae_reg_t)(UAE_REG_D0 + i), r->d[i]);
	for (int i = 0; i < 7; i++)
		uae_cpu_set_reg(s_cpu, (uae_reg_t)(UAE_REG_A0 + i), r->a[i]);

	sp = cpu_engine_clamp_sp(uae_cpu_get_reg(s_cpu, UAE_REG_A7));
	sp = cpu_engine_write_exec_return_frame(sp, &ret_addr);
	uae_cpu_set_reg(s_cpu, UAE_REG_A7, sp);
	uae_cpu_set_reg(s_cpu, UAE_REG_PC, addr);

	/* The EXEC_RETURN word is code this host just wrote into guest memory. */
	uae_cpu_invalidate_code(s_cpu, ret_addr, 2);

	PushReturnStack(&return_seen);
	while (!return_seen && !s_quit_requested)
		uae_cpu_execute(s_cpu, UAE_NESTED_CYCLES);
	PopReturnStack();

	uae_cpu_set_reg(s_cpu, UAE_REG_A7, uae_cpu_get_reg(s_cpu, UAE_REG_A7) + 2);
	uae_cpu_set_reg(s_cpu, UAE_REG_PC, oldpc);

	for (int i = 0; i < 8; i++)
		r->d[i] = uae_cpu_get_reg(s_cpu, (uae_reg_t)(UAE_REG_D0 + i));
	for (int i = 0; i < 7; i++)
		r->a[i] = uae_cpu_get_reg(s_cpu, (uae_reg_t)(UAE_REG_A0 + i));
}

/*
 * Runs a Toolbox / Line-A trap through a planted 4-byte stub.
 */
static void uaecpu_execute_68k_trap(uint16 trap, struct M68kRegisters *r)
{
	uint32 oldpc = uae_cpu_get_pc(s_cpu);
	uint32 sp, stub;
	bool return_seen = false;

	for (int i = 0; i < 8; i++)
		uae_cpu_set_reg(s_cpu, (uae_reg_t)(UAE_REG_D0 + i), r->d[i]);
	for (int i = 0; i < 7; i++)
		uae_cpu_set_reg(s_cpu, (uae_reg_t)(UAE_REG_A0 + i), r->a[i]);

	sp = cpu_engine_clamp_sp(uae_cpu_get_reg(s_cpu, UAE_REG_A7));
	stub = cpu_engine_write_trap_stub(sp, trap);
	uae_cpu_set_reg(s_cpu, UAE_REG_A7, stub);
	uae_cpu_set_reg(s_cpu, UAE_REG_PC, stub);

	/* The stub is code this host just wrote into guest memory. */
	uae_cpu_invalidate_code(s_cpu, stub, 4);

	PushReturnStack(&return_seen);
	while (!return_seen && !s_quit_requested)
		uae_cpu_execute(s_cpu, UAE_NESTED_CYCLES);
	PopReturnStack();

	uae_cpu_set_reg(s_cpu, UAE_REG_A7, uae_cpu_get_reg(s_cpu, UAE_REG_A7) + 4);
	uae_cpu_set_reg(s_cpu, UAE_REG_PC, oldpc);

	for (int i = 0; i < 8; i++)
		r->d[i] = uae_cpu_get_reg(s_cpu, (uae_reg_t)(UAE_REG_D0 + i));
	for (int i = 0; i < 7; i++)
		r->a[i] = uae_cpu_get_reg(s_cpu, (uae_reg_t)(UAE_REG_A0 + i));
}

extern const CPUEngine uae_portable_cpu_engine = {
	"uaecpu",
	"UAE Portable 680x0 Core (interpreter + ARM64/x86-64 JIT)",
	false,
	CPU_MEM_STRATEGY_DIRECT_POINTER,
	CPU_ENGINE_TIER_PERFORMANCE,
	uaecpu_init,
	uaecpu_exit,
	uaecpu_start,
	uaecpu_reset,
	uaecpu_execute_68k,
	uaecpu_execute_68k_trap,
	uaecpu_trigger_interrupt,
	uaecpu_trigger_nmi,
	uaecpu_intlev,
	uaecpu_invalidate_code,
	uaecpu_emulated_ns,
	uaecpu_get_pc
};
