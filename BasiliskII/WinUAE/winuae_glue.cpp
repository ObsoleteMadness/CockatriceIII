/*
 *  winuae_glue.cpp - WinUAE 680x0 Engine Adapter for CockatriceIII
 *
 *  (C) 2026 CockatriceIII Project
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
#include "rom_patches.h"
#include "scc.h"

static jmp_buf s_winuae_reset_jmp;
static volatile bool s_winuae_reset_valid = false;
static bool s_winuae_quit_requested = false;

static bool winuae_init(void)
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
	return true;
}

static void winuae_exit(void)
{
	s_winuae_quit_requested = true;
}

static void winuae_start(void)
{
	s_winuae_quit_requested = false;
	printf("[WinUAE] Starting 680%d0 CPU execution at 0x%08X...\n", CPUType, ROMBaseMac + 0x2a);
	fflush(stdout);
}

static void winuae_reset(void)
{
	if (s_winuae_reset_valid) {
		longjmp(s_winuae_reset_jmp, 1);
	}
}

static void winuae_trigger_interrupt(void)
{
}

static void winuae_trigger_nmi(void)
{
}

static int winuae_intlev(void)
{
	if (SCCInterruptRequest) {
		return TwentyFourBitAddressing ? 2 : 4;
	}
	return InterruptFlags ? 1 : 0;
}

static void winuae_execute_68k_trap(uint16 trap, struct M68kRegisters *r)
{
	(void)trap;
	(void)r;
}

static void winuae_execute_68k(uint32 addr, struct M68kRegisters *r)
{
	(void)addr;
	(void)r;
}

extern const CPUEngine winuae_cpu_engine = {
	"uae",
	"WinUAE 680x0 Engine (with SoftFloat 68882/68040 FPU)",
	false,
	winuae_init,
	winuae_exit,
	winuae_start,
	winuae_reset,
	winuae_execute_68k,
	winuae_execute_68k_trap,
	winuae_trigger_interrupt,
	winuae_trigger_nmi,
	winuae_intlev
};
