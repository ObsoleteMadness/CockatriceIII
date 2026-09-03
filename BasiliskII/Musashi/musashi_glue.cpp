/*
 *  musashi_glue.cpp - Glue Musashi 680x0 CPU to Basilisk II CPU engine interface
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *  CockatriceIII Musashi Core Migration (C) 2026
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include <stdio.h>
#include <string.h>
#include <setjmp.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "main.h"
#include "prefs.h"
#include "scc.h"
#include "emul_op.h"
#include "rom_patches.h"
#include "timer.h"
#include "ether.h"
#include "scsi.h"
#include "sony.h"
#include "disk.h"
#include "audio.h"
#include "menu_bar.h"
#include "m68k.h"
#include "macos_util.h"

// Longjmp reset buffer for handling Mac OS warm resets
static jmp_buf s_cpu_reset_jmp;
static volatile bool s_cpu_reset_valid = false;
static bool s_quit_requested = false;

/*
 * Logs 680x0 register and stack context when execution enters low heap fill.
 * Called from Musashi Line-F / PMMU paths when PC is in 0x65000..0x66FFF so we
 * can see which return address led extension init into uninitialized memory.
 *
 * Arguments:
 *   opcode: Faulting 16-bit instruction word.
 *   pc: Program counter (680x0 logical address).
 *   kind: Short tag ("line-f" or "pmmu") for the log prefix.
 */
extern "C" void cockatrice_m68k_low_heap_fault(uint16 opcode, uint32 pc, const char *kind)
{
	if (pc < 0x65000 || pc >= 0x67000)
		return;

	printf("[CPU-FAULT] %s in low heap at PC=0x%08X opcode=0x%04X PPC=0x%08X A7=0x%08X SR=0x%04X\n",
	       kind, pc, opcode,
	       (uint32)m68k_get_reg(NULL, M68K_REG_PPC),
	       (uint32)m68k_get_reg(NULL, M68K_REG_A7),
	       (uint16)m68k_get_reg(NULL, M68K_REG_SR));
	for (int i = 0; i < 8; i++)
		printf("  D%d=0x%08X", i, (uint32)m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + i)));
	printf("\n");
	for (int i = 0; i < 8; i++)
		printf("  A%d=0x%08X", i, (uint32)m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + i)));
	printf("\n");

	uint32 sp = m68k_get_reg(NULL, M68K_REG_A7);
	printf("  stack at A7:");
	for (int i = 0; i < 8; i++) {
		if (sp + (uint32)(i * 4) + 3 >= RAMSize)
			break;
		printf(" %08X", ReadMacInt32(sp + i * 4));
	}
	printf("\n  code@PC:");
	for (int off = -4; off <= 8; off += 2) {
		uint32 addr = pc + off;
		if (addr + 1 >= RAMSize)
			break;
		printf(" %04X", ReadMacInt16(addr));
	}
	printf("\n");
	fflush(stdout);
}

/*
 * Musashi illegal instruction callback (for EmulOps and Execution Return).
 *
 * Arguments:
 *   opcode: 16-bit 680x0 opcode that triggered the illegal instruction exception.
 *
 * Returns:
 *   1 if the opcode was handled internally by the emulator, 0 to pass to 680x0 exception vector.
 */
extern "C" int musashi_illg_callback(int opcode)
{
	// Check for magic execution return opcode (0x7100)
	if (opcode == M68K_EXEC_RETURN) {
		// Signal return flag to break out of nested Execute68k/Execute68kTrap loops
		TriggerExecutionReturn();
		m68k_end_timeslice();
		return 1;
	}

	// Check for Basilisk II EmulOp opcode range (0x7101 .. 0x713F)
	if (opcode > M68K_EXEC_RETURN && opcode < M68K_EMUL_OP_MAX) {
		struct M68kRegisters r;

		// Load current CPU registers into EmulOp register structure
		for (int i = 0; i < 8; i++) {
			r.d[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + i));
			r.a[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + i));
		}
		r.sr = (uint16)m68k_get_reg(NULL, M68K_REG_SR);

		// Execute host emulation trap handler
		EmulOp((uint16)opcode, &r);

		// Write EmulOp results back; gate A7 against the live ISP, not the struct default
		for (int i = 0; i < 8; i++)
			m68k_set_reg((m68k_register_t)(M68K_REG_D0 + i), r.d[i]);
		for (int i = 0; i < 8; i++)
			m68k_set_reg((m68k_register_t)(M68K_REG_A0 + i), r.a[i]);
		m68k_set_reg(M68K_REG_SR, r.sr);
		return 1;
	}

	// Warn if opcode falls in EmulOp range but is unhandled
	if (opcode >= 0x7000 && opcode < 0x7200) {
		printf("[EMUL-OP] Unhandled/Unknown EmulOp 0x%04X at PC=0x%08X (A7=0x%08X)\n",
		       (uint16)opcode, (uint32)m68k_get_reg(NULL, M68K_REG_PC), (uint32)m68k_get_reg(NULL, M68K_REG_A7));
		fflush(stdout);
	}

	return 0;
}

/*
 * Initializes the Musashi 680x0 CPU engine.
 *
 * Returns true if initialization succeeded, false otherwise.
 */
static bool musashi_init(void)
{
	if (!cpu_engine_map_rom_base())
		return false;

	// Clear leftover exit flags so Execute68k works after a previous musashi_exit()
	s_quit_requested = false;

	// Initialize the memory subsystem and core CPU state
	memory_init();
	m68k_init();

	// Configure CPU core model based on preferences
	switch (CPUType) {
		case 0:
			m68k_set_cpu_type(M68K_CPU_TYPE_68000);
			break;
		case 1:
			m68k_set_cpu_type(M68K_CPU_TYPE_68010);
			break;
		case 2:
			m68k_set_cpu_type(M68K_CPU_TYPE_68020);
			break;
		case 3:
			m68k_set_cpu_type(M68K_CPU_TYPE_68030);
			break;
		case 4:
		default:
			/*
			 * Basilisk disables the Mac PMMU (Patch BootGlobs / InitMMU NOPs).
			 * 68EC040 keeps 68040+FPU behavior without PMMU F-line decode on
			 * heap fill (F0xx), which otherwise surfaces as spurious PMOVE logs.
			 */
			m68k_set_cpu_type(M68K_CPU_TYPE_68040);
			break;
	}

	/* Musashi requires at least one hardware reset after set_cpu_type so SR, SP,
	 * and VBR are defined. Without this, Execute68k runs in user mode and MOVEC
	 * (and other privileged ops) fault into uninitialized exception vectors. */
	m68k_pulse_reset();

	return true;
}

/*
 * Signals the Musashi engine to terminate execution and end current slice.
 */
static void musashi_exit(void)
{
	// Set quit request flag and end current execution slice
	s_quit_requested = true;
	m68k_end_timeslice();
}

/*
 * Main execution entry point for the Musashi engine.
 * Sets up the reset longjmp buffer, pulses CPU reset, sets entry PC,
 * and enters the main continuous execution loop.
 */
static void musashi_start(void)
{
	s_quit_requested = false;
	for (;;) {
		if (setjmp(s_cpu_reset_jmp) == 0) {
			s_cpu_reset_valid = true;

			// Pulse CPU hardware reset line
			m68k_pulse_reset();

			// Initialize boot registers: default SP, ROM boot entry point, interrupts masked
			m68k_set_reg(M68K_REG_A7, CPU_ENGINE_BOOT_SP);
			m68k_set_reg(M68K_REG_PC, ROMBaseMac + CPU_ENGINE_BOOT_PC_OFF);
			m68k_set_reg(M68K_REG_SR, CPU_ENGINE_BOOT_SR);

			while (!s_quit_requested) {
				cpu_engine_note_pc(m68k_get_reg(NULL, M68K_REG_PC));
				m68k_execute(50000);
			}
			break;
		} else {
			// Subsystem reset triggered via Reset680x0()
			printf("Reset680x0: Resetting machine subsystems...\n");
			fflush(stdout);

			// Reset all peripheral subsystems to initial state
			cpu_engine_reset_peripherals();

			s_quit_requested = false;
			MenuBar_UpdateAll();
		}
	}
	s_cpu_reset_valid = false;
}

/*
 * Performs a software reset of the 680x0 CPU, jumping back to the reset handler.
 */
static void musashi_reset(void)
{
	// Jump back to the active reset setjmp handler if valid
	if (s_cpu_reset_valid) {
		longjmp(s_cpu_reset_jmp, 1);
	}
}

/*
 * Calculates the current highest pending interrupt request level.
 *
 * Returns:
 *   Interrupt level (0 = none, 1..7).
 */
static int musashi_intlev(void)
{
	return cpu_engine_intlev();
}

/*
 * Asserts the calculated pending interrupt level on the CPU core.
 */
static void musashi_trigger_interrupt(void)
{
	// Determine pending interrupt level and assert on CPU
	int level = musashi_intlev();
	m68k_set_irq(level);
}

/*
 * Triggers a non-maskable interrupt (NMI, level 7).
 */
static void musashi_trigger_nmi(void)
{
	// Assert level 7 NMI
	m68k_set_irq(7);
}

/*
 * Executes a Mac OS Line-A or Toolbox trap subroutine and returns control to C++.
 *
 * Historic Basilisk contract (Amiga native / UAE): r->d[0..7] and r->a[0..6]
 * are inputs/outputs; r->a[7] and r->sr are unused. Only PC is restored on the
 * live CPU; nested trap results remain in D/A until the host reads r.
 */
static void musashi_execute_68k_trap(uint16 trap, struct M68kRegisters *r)
{
	uint32 oldpc = m68k_get_reg(NULL, M68K_REG_PC);

	for (int i = 0; i < 8; i++)
		m68k_set_reg((m68k_register_t)(M68K_REG_D0 + i), r->d[i]);
	for (int i = 0; i < 7; i++)
		m68k_set_reg((m68k_register_t)(M68K_REG_A0 + i), r->a[i]);

	uint32 sp = cpu_engine_clamp_sp(m68k_get_reg(NULL, M68K_REG_A7));
	uint32 stub = cpu_engine_write_trap_stub(sp, trap);
	m68k_set_reg(M68K_REG_A7, stub);
	m68k_set_reg(M68K_REG_PC, stub);

	bool return_seen = false;
	PushReturnStack(&return_seen);

	while (!return_seen && !s_quit_requested)
		m68k_execute(5000);

	PopReturnStack();

	sp = m68k_get_reg(NULL, M68K_REG_A7);
	sp += 4;
	m68k_set_reg(M68K_REG_A7, sp);
	m68k_set_reg(M68K_REG_PC, oldpc);

	for (int i = 0; i < 8; i++)
		r->d[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + i));
	for (int i = 0; i < 7; i++)
		r->a[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + i));
}

/*
 * Executes a 680x0 subroutine at `addr` (must end with RTS) and returns to C++.
 *
 * Same M68kRegisters contract as musashi_execute_68k_trap: live A7/SR, PC restored.
 */
static void musashi_execute_68k(uint32 addr, struct M68kRegisters *r)
{
	uint32 oldpc = m68k_get_reg(NULL, M68K_REG_PC);

	for (int i = 0; i < 8; i++)
		m68k_set_reg((m68k_register_t)(M68K_REG_D0 + i), r->d[i]);
	for (int i = 0; i < 7; i++)
		m68k_set_reg((m68k_register_t)(M68K_REG_A0 + i), r->a[i]);

	uint32 sp = cpu_engine_clamp_sp(m68k_get_reg(NULL, M68K_REG_A7));
	uint32 ret_addr = 0;
	sp = cpu_engine_write_exec_return_frame(sp, &ret_addr);
	m68k_set_reg(M68K_REG_A7, sp);
	m68k_set_reg(M68K_REG_PC, addr);

	bool return_seen = false;
	PushReturnStack(&return_seen);

	while (!return_seen && !s_quit_requested)
		m68k_execute(5000);

	PopReturnStack();

	sp = m68k_get_reg(NULL, M68K_REG_A7);
	sp += 2;
	m68k_set_reg(M68K_REG_A7, sp);
	m68k_set_reg(M68K_REG_PC, oldpc);

	for (int i = 0; i < 8; i++)
		r->d[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + i));
	for (int i = 0; i < 7; i++)
		r->a[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + i));
}

// Musashi CPUEngine dispatch table
extern const CPUEngine musashi_cpu_engine = {
	"musashi",
	"Musashi 680x0 C Core (Version 4.5+)",
	false, // Interpreter
	musashi_init,
	musashi_exit,
	musashi_start,
	musashi_reset,
	musashi_execute_68k,
	musashi_execute_68k_trap,
	musashi_trigger_interrupt,
	musashi_trigger_nmi,
	musashi_intlev,
	nullptr
};
