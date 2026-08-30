/*
 *  emu68_glue.cpp - Emu68 AArch64 Dynamic Binary Translation / JIT Adapter
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

static jmp_buf s_emu68_reset_jmp;
static volatile bool s_emu68_reset_valid = false;
static bool s_emu68_quit_requested = false;

static bool emu68_init(void)
{
#if !defined(__aarch64__) && !defined(__arm64__)
	printf("[Emu68] Error: Emu68 JIT requires an AArch64 / ARM64 host architecture.\n");
	return false;
#endif

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

static void emu68_exit(void)
{
	s_emu68_quit_requested = true;
}

static void emu68_start(void)
{
	s_emu68_quit_requested = false;
	printf("[Emu68] Starting AArch64 JIT execution at 0x%08X...\n", ROMBaseMac + 0x2a);
	fflush(stdout);
}

static void emu68_reset(void)
{
	if (s_emu68_reset_valid) {
		longjmp(s_emu68_reset_jmp, 1);
	}
}

static void emu68_trigger_interrupt(void)
{
}

static void emu68_trigger_nmi(void)
{
}

static int emu68_intlev(void)
{
	if (SCCInterruptRequest) {
		return TwentyFourBitAddressing ? 2 : 4;
	}
	return InterruptFlags ? 1 : 0;
}

static void emu68_execute_68k_trap(uint16 trap, struct M68kRegisters *r)
{
	(void)trap;
	(void)r;
}

static void emu68_execute_68k(uint32 addr, struct M68kRegisters *r)
{
	(void)addr;
	(void)r;
}

extern const CPUEngine emu68_cpu_engine = {
	"emu68",
	"Emu68 AArch64 Dynamic Binary Translation / JIT",
	true,
	emu68_init,
	emu68_exit,
	emu68_start,
	emu68_reset,
	emu68_execute_68k,
	emu68_execute_68k_trap,
	emu68_trigger_interrupt,
	emu68_trigger_nmi,
	emu68_intlev
};
