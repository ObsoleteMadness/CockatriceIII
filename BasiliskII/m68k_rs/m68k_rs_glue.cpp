/*
 *  m68k_rs_glue.cpp - Glue m68k-rs (Rust) CPU core to Basilisk II CPUEngine interface
 *
 *  CockatriceIII (C) 2026
 */

#include <stdio.h>
#include <string.h>
#include <setjmp.h>

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

/*
 * Polls Basilisk interrupt sources and records the guest PC each instruction.
 */
static void m68k_rs_host_boundary_hook(void *ctx, uint32 cycles)
{
	(void)ctx;
	(void)cycles;
	cpu_engine_note_pc(m68k_rs_get_reg(s_cpu, M68K_RS_REG_PC));
}

static int m68k_rs_host_get_irq(void *ctx)
{
	(void)ctx;
	return cpu_engine_intlev();
}

/*
 * Runs a cycle slice, stopping early when the host requests exit or an unhandled trap fires.
 */
static void m68k_rs_run_slice(int32 cycles)
{
	for (;;) {
		M68kRsRunResult result = m68k_rs_run_cycles(s_cpu, cycles);
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
		callbacks.host_ctx = nullptr;
		s_cpu = m68k_rs_create(&callbacks);
		if (!s_cpu)
			return false;
	}

	memory_init();
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
				m68k_rs_run_slice(50000);
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
		m68k_rs_run_slice(5000);
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
		m68k_rs_run_slice(5000);
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
