/*
 *  basilisk_glue.cpp - Glue Musashi 680x0 CPU to Basilisk II CPU engine interface
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

// RAM and ROM pointers
uint32 RAMBaseMac = 0;
uint8 *RAMBaseHost = NULL;
uint32 RAMSize = 0;
uint32 ROMBaseMac = 0;
uint8 *ROMBaseHost = NULL;
uint32 ROMSize = 0;

// Frame buffer
uint8 *MacFrameBaseHost = NULL;
uint32 MacFrameSize = 0;
int MacFrameLayout = FLAYOUT_NONE;

static jmp_buf s_cpu_reset_jmp;
static volatile bool s_cpu_reset_valid = false;
static bool s_quit_requested = false;

#define MAX_NESTED_EXEC 32
static bool *s_return_stack[MAX_NESTED_EXEC];
static int s_return_stack_top = 0;

/*
 * Musashi illegal instruction callback (for EmulOps and Execution Return)
 */
extern "C" int musashi_illg_callback(int opcode)
{
	if (opcode == M68K_EXEC_RETURN) {
		if (s_return_stack_top > 0) {
			*s_return_stack[s_return_stack_top - 1] = true;
		}
		m68k_end_timeslice();
		return 1;
	}

	if (opcode > M68K_EXEC_RETURN && opcode < M68K_EMUL_OP_MAX) {
		struct M68kRegisters r;
		for (int i = 0; i < 8; i++) {
			r.d[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + i));
			r.a[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + i));
		}
		r.sr = (uint16)m68k_get_reg(NULL, M68K_REG_SR);

		EmulOp((uint16)opcode, &r);

		for (int i = 0; i < 8; i++) {
			m68k_set_reg((m68k_register_t)(M68K_REG_D0 + i), r.d[i]);
			m68k_set_reg((m68k_register_t)(M68K_REG_A0 + i), r.a[i]);
		}
		m68k_set_reg(M68K_REG_SR, r.sr);
		return 1;
	}

	if (opcode >= 0x7000 && opcode < 0x7200) {
		printf("[EMUL-OP] Unhandled/Unknown EmulOp 0x%04X at PC=0x%08X (A7=0x%08X)\n",
		       (uint16)opcode, (uint32)m68k_get_reg(NULL, M68K_REG_PC), (uint32)m68k_get_reg(NULL, M68K_REG_A7));
		fflush(stdout);
	}

	return 0;
}

#include "cpu_engine.h"

/*
 * Initialize 680x0 emulation
 */
static bool musashi_init(void)
{
	RAMBaseMac = 0;
	switch (ROMVersion) {
		case ROM_VERSION_64K:
		case ROM_VERSION_PLUS:
		case ROM_VERSION_CLASSIC:
			ROMBaseMac = 0x00400000;
			break;
		case ROM_VERSION_II:
			ROMBaseMac = 0x00a00000;
			break;
		case ROM_VERSION_32:
			ROMBaseMac = 0x40800000;
			break;
		default:
			return false;
	}

	memory_init();
	m68k_init();

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
			m68k_set_cpu_type(M68K_CPU_TYPE_68040);
			break;
	}

	return true;
}

/*
 * Deinitialize 680x0 emulation
 */
static void musashi_exit(void)
{
	s_quit_requested = true;
	m68k_end_timeslice();
}

/*
 * Initialize frame buffer mapping
 */
void InitFrameBufferMapping(void)
{
	memory_init();
}

/*
 * Start 680x0 CPU
 */
static void musashi_start(void)
{
	s_quit_requested = false;
	for (;;) {
		if (setjmp(s_cpu_reset_jmp) == 0) {
			s_cpu_reset_valid = true;
			m68k_pulse_reset();

			// Mac boot entry point
			m68k_set_reg(M68K_REG_A7, 0x2000);
			m68k_set_reg(M68K_REG_PC, ROMBaseMac + 0x2a);
			m68k_set_reg(M68K_REG_SR, 0x2700);

			while (!s_quit_requested) {
				m68k_execute(50000);
			}
			break;
		} else {
			// Arrived here via Reset680x0()
			printf("Reset680x0: Resetting machine subsystems...\n");
			fflush(stdout);

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

			s_quit_requested = false;
			MenuBar_UpdateAll();
		}
	}
	s_cpu_reset_valid = false;
}

/*
 * Reset running 680x0 CPU
 */
static void musashi_reset(void)
{
	if (s_cpu_reset_valid) {
		longjmp(s_cpu_reset_jmp, 1);
	}
}

static int musashi_intlev(void)
{
	if (SCCInterruptRequest) {
		return TwentyFourBitAddressing ? 2 : 4;
	}
	return InterruptFlags ? 1 : 0;
}

/*
 * Trigger interrupt
 */
static void musashi_trigger_interrupt(void)
{
	int level = musashi_intlev();
	m68k_set_irq(level);
}

static void musashi_trigger_nmi(void)
{
	m68k_set_irq(7);
}

/*
 * Execute MacOS 68k trap from EMUL_OP routine
 */
static void musashi_execute_68k_trap(uint16 trap, struct M68kRegisters *r)
{
	uint32 oldpc = m68k_get_reg(NULL, M68K_REG_PC);

	for (int i = 0; i < 8; i++)
		m68k_set_reg((m68k_register_t)(M68K_REG_D0 + i), r->d[i]);
	for (int i = 0; i < 7; i++)
		m68k_set_reg((m68k_register_t)(M68K_REG_A0 + i), r->a[i]);

	uint32 sp = m68k_get_reg(NULL, M68K_REG_A7);
	if (sp < 0x1000 || sp >= RAMSize)
		sp = 0x10000;
	sp -= 2;
	WriteMacInt16(sp, (uint16)M68K_EXEC_RETURN);
	sp -= 2;
	WriteMacInt16(sp, trap);
	m68k_set_reg(M68K_REG_A7, sp);
	m68k_set_reg(M68K_REG_PC, sp);

	bool return_seen = false;
	if (s_return_stack_top < MAX_NESTED_EXEC) {
		s_return_stack[s_return_stack_top++] = &return_seen;
	}

	while (!return_seen && !s_quit_requested) {
		m68k_execute(5000);
	}

	if (s_return_stack_top > 0) {
		s_return_stack_top--;
	}

	sp = m68k_get_reg(NULL, M68K_REG_A7);
	sp += 4;
	m68k_set_reg(M68K_REG_A7, sp);
	m68k_set_reg(M68K_REG_PC, oldpc);

	for (int i = 0; i < 8; i++)
		r->d[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + i));
	for (int i = 0; i < 7; i++)
		r->a[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + i));
	r->a[7] = sp;
	r->sr = (uint16)m68k_get_reg(NULL, M68K_REG_SR);
}

/*
 * Execute 68k subroutine from EMUL_OP routine
 */
static void musashi_execute_68k(uint32 addr, struct M68kRegisters *r)
{
	uint32 oldpc = m68k_get_reg(NULL, M68K_REG_PC);

	for (int i = 0; i < 8; i++)
		m68k_set_reg((m68k_register_t)(M68K_REG_D0 + i), r->d[i]);
	for (int i = 0; i < 7; i++)
		m68k_set_reg((m68k_register_t)(M68K_REG_A0 + i), r->a[i]);

	uint32 sp = m68k_get_reg(NULL, M68K_REG_A7);
	if (sp < 0x1000 || sp >= RAMSize)
		sp = 0x10000;
	sp -= 2;
	WriteMacInt16(sp, (uint16)M68K_EXEC_RETURN);
	sp -= 4;
	WriteMacInt32(sp, sp + 4);
	m68k_set_reg(M68K_REG_A7, sp);
	m68k_set_reg(M68K_REG_PC, addr);

	bool return_seen = false;
	if (s_return_stack_top < MAX_NESTED_EXEC) {
		s_return_stack[s_return_stack_top++] = &return_seen;
	}

	while (!return_seen && !s_quit_requested) {
		m68k_execute(5000);
	}

	if (s_return_stack_top > 0) {
		s_return_stack_top--;
	}

	sp = m68k_get_reg(NULL, M68K_REG_A7);
	sp += 2;
	m68k_set_reg(M68K_REG_A7, sp);
	m68k_set_reg(M68K_REG_PC, oldpc);

	for (int i = 0; i < 8; i++)
		r->d[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + i));
	for (int i = 0; i < 7; i++)
		r->a[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + i));
	r->a[7] = sp;
	r->sr = (uint16)m68k_get_reg(NULL, M68K_REG_SR);
}

extern const CPUEngine musashi_cpu_engine = {
	"musashi",
	"Musashi 680x0 C Core (Version 4.5+)",
	false,
	musashi_init,
	musashi_exit,
	musashi_start,
	musashi_reset,
	musashi_execute_68k,
	musashi_execute_68k_trap,
	musashi_trigger_interrupt,
	musashi_trigger_nmi,
	musashi_intlev
};
