/*
 *  amiberry_glue.cpp - Amiberry 680x0 engine adapter for Cockatrice III
 *
 *  Prefs `cpu_emulator uae` always uses the Amiberry CPU (interpreter when
 *  `jit false`, ARM64 or x86-64 JIT when `jit true`). Musashi is not used
 *  as a fallback on this path.
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
#include "menu_bar.h"
#include "scc.h"
#include "scsi.h"
#include "sony.h"
#include "disk.h"
#include "audio.h"
#include "ether.h"
#include "timer.h"

#include "hosted/amiberry_cpu_api.h"

static jmp_buf s_winuae_reset_jmp;
static volatile bool s_winuae_reset_valid = false;
static bool s_winuae_quit_requested = false;

/*
 * Handles Basilisk II EmulOp opcodes from dedicated UAE opcode handlers
 * (op_emulop_1 / op_emulop_return_1). The 0x7100..0x713F range is not routed
 * through op_illg (see macemu table68k EMULOP / basilisk_glue.cpp).
 */
extern "C" void cockatrice_m68k_emulop(uint32_t opcode)
{
	struct M68kRegisters r;
	for (int i = 0; i < 8; i++) {
		r.d[i] = amiberry_cpu_get_reg(i);
		r.a[i] = amiberry_cpu_get_reg(i + 8);
	}
	r.sr = amiberry_cpu_get_sr();
	EmulOp((uint16)opcode, &r);
	for (int i = 0; i < 8; i++)
		amiberry_cpu_set_reg(i, r.d[i]);
	for (int i = 0; i < 8; i++)
		amiberry_cpu_set_reg(i + 8, r.a[i]);
	amiberry_cpu_set_sr(r.sr);
}

extern "C" void cockatrice_m68k_emulop_return(void)
{
	TriggerExecutionReturn();
	/* macemu: BRK + quit_program inside nested m68k_execute only; top-level JIT
	 * boot also uses BRK so we scope the early exit to nested Execute68k. */
	if (amiberry_cpu_nested_execute_depth() > 0)
		amiberry_cpu_nested_request_quit();
	else
		amiberry_cpu_set_mode_change();
}

extern "C" int cockatrice_uae_illg(uint32_t opcode)
{
	if (opcode == (uint32_t)M68K_EXEC_RETURN) {
		TriggerExecutionReturn();
		if (amiberry_cpu_nested_execute_depth() > 0)
			amiberry_cpu_nested_request_quit();
		else
			amiberry_cpu_set_mode_change();
		return 1;
	}
	if (opcode > (uint32_t)M68K_EXEC_RETURN && opcode < (uint32_t)M68K_EMUL_OP_MAX) {
		cockatrice_m68k_emulop(opcode);
		return 1;
	}
	if (opcode >= 0x7000 && opcode < 0x7200) {
		printf("[EMUL-OP] Unhandled EmulOp 0x%04X at PC=0x%08X (expected op_emulop_* handler)\n",
			(uint16)opcode, amiberry_cpu_get_pc());
		fflush(stdout);
	}
	return 0;
}

uint32_t cockatrice_mac_get_long(uint32_t addr)
{
	return ReadMacInt32(addr);
}

uint32_t cockatrice_mac_get_word(uint32_t addr)
{
	return ReadMacInt16(addr);
}

uint32_t cockatrice_mac_get_byte(uint32_t addr)
{
	return ReadMacInt8(addr);
}

void cockatrice_mac_put_long(uint32_t addr, uint32_t v)
{
	WriteMacInt32(addr, v);
}

void cockatrice_mac_put_word(uint32_t addr, uint32_t v)
{
	WriteMacInt16(addr, (uint16)v);
}

void cockatrice_mac_put_byte(uint32_t addr, uint32_t v)
{
	WriteMacInt8(addr, (uint8)v);
}

uint8_t *cockatrice_mac_host_addr(uint32_t addr)
{
	return Mac2HostAddr(addr);
}

int cockatrice_mac_valid_addr(uint32_t addr, uint32_t size)
{
	if (is_scc_addr(addr))
		return 1;
	return memory_is_mapped(addr, size) ? 1 : 0;
}

extern "C" void cockatrice_memory_raise_guest_fault(uint32_t addr)
{
	memory_raise_guest_fault(addr);
}

static bool winuae_init(void)
{
	// Clear leftover exit flags so Execute68k works after a previous winuae_exit()
	s_winuae_quit_requested = false;

	if (!cpu_engine_map_rom_base())
		return false;

	memory_init();
	if (!amiberry_cpu_init(CPUType, FPUType, UseJIT ? 1 : 0, JITCacheSize, UseJITFPU ? 1 : 0))
		return false;

	return true;
}

static void winuae_exit(void)
{
	s_winuae_quit_requested = true;
	amiberry_cpu_exit();
}

static void winuae_start(void)
{
	s_winuae_quit_requested = false;
	for (;;) {
		if (setjmp(s_winuae_reset_jmp) == 0) {
			s_winuae_reset_valid = true;
			printf("[UAE] Starting 680%d0 at 0x%08X (jit %s)...\n",
				CPUType, ROMBaseMac + 0x2a,
				UseJIT ? (UseJITFPU ? "true+jitfpu" : "true") : "false");
			fflush(stdout);

			amiberry_cpu_reset();
			amiberry_cpu_set_reg(15, CPU_ENGINE_BOOT_SP);
			amiberry_cpu_set_pc(ROMBaseMac + CPU_ENGINE_BOOT_PC_OFF);
			amiberry_cpu_set_sr(CPU_ENGINE_BOOT_SR);

			while (!s_winuae_quit_requested) {
				cpu_engine_note_pc(amiberry_cpu_get_pc());
				amiberry_cpu_execute_slice();
			}
			break;
		} else {
			// Subsystem reset triggered via Reset680x0()
			printf("Reset680x0 (UAE): Resetting machine subsystems...\n");
			fflush(stdout);
			cpu_engine_reset_peripherals();
			s_winuae_quit_requested = false;
			MenuBar_UpdateAll();
		}
	}
	s_winuae_reset_valid = false;
}

static void winuae_reset(void)
{
	if (s_winuae_reset_valid)
		longjmp(s_winuae_reset_jmp, 1);
}

static int winuae_intlev(void)
{
	return cpu_engine_intlev();
}

static void winuae_trigger_interrupt(void)
{
	amiberry_cpu_request_irq();
}

static void winuae_trigger_nmi(void)
{
	amiberry_cpu_request_irq();
}

/*
 * Forwards Basilisk code-cache invalidation to the UAE JIT backend.
 *
 * Arguments:
 *   addr: Start of modified guest code.
 *   size: Byte length of the modified range.
 */
static void winuae_invalidate_code(uint32 addr, uint32 size)
{
	amiberry_cpu_invalidate_code(addr, size);
}

/*
 * Reports Amiberry currcycle as 40 MHz nanoseconds for Time Manager.
 *
 * Returns:
 *   Emulated ns since CPU reset, or 0 before the first cycle.
 */
static uint64 winuae_emulated_ns(void)
{
	return (uint64)amiberry_cpu_emulated_ns();
}

static void winuae_execute_68k_trap(uint16 trap, struct M68kRegisters *r)
{
	uint32 oldpc = amiberry_cpu_get_pc();

	for (int i = 0; i < 8; i++)
		amiberry_cpu_set_reg(i, r->d[i]);
	for (int i = 0; i < 7; i++)
		amiberry_cpu_set_reg(i + 8, r->a[i]);

	/* macemu basilisk_glue: push EXEC_RETURN + trap on stack, run m68k_execute(). */
	uint32 sp = cpu_engine_clamp_sp(amiberry_cpu_get_reg(15));
	sp -= 2;
	cockatrice_mac_put_word(sp, (uint16)M68K_EXEC_RETURN);
	sp -= 2;
	cockatrice_mac_put_word(sp, trap);
	amiberry_cpu_set_reg(15, sp);
	amiberry_cpu_set_pc(sp);
	amiberry_cpu_fill_prefetch();

	bool return_seen = false;
	PushReturnStack(&return_seen);
	amiberry_cpu_nested_execute_begin();
	while (!return_seen && !s_winuae_quit_requested && !amiberry_cpu_nested_quit_requested())
		amiberry_cpu_execute_interpreter_slice();
	amiberry_cpu_nested_execute_end();
	PopReturnStack();

	amiberry_cpu_clear_mode_change();

	sp = amiberry_cpu_get_reg(15);
	sp += 4;
	amiberry_cpu_set_reg(15, sp);
	amiberry_cpu_set_pc(oldpc);
	amiberry_cpu_fill_prefetch();

	for (int i = 0; i < 8; i++)
		r->d[i] = amiberry_cpu_get_reg(i);
	for (int i = 0; i < 7; i++)
		r->a[i] = amiberry_cpu_get_reg(i + 8);
}

static void winuae_execute_68k(uint32 addr, struct M68kRegisters *r)
{
	uint32 oldpc = amiberry_cpu_get_pc();

	for (int i = 0; i < 8; i++)
		amiberry_cpu_set_reg(i, r->d[i]);
	for (int i = 0; i < 7; i++)
		amiberry_cpu_set_reg(i + 8, r->a[i]);

	/* macemu basilisk_glue: EXEC_RETURN + faked RTS return, then m68k_execute(). */
	uint32 sp = cpu_engine_clamp_sp(amiberry_cpu_get_reg(15));
	sp -= 2;
	cockatrice_mac_put_word(sp, (uint16)M68K_EXEC_RETURN);
	sp -= 4;
	cockatrice_mac_put_long(sp, sp + 4);
	amiberry_cpu_set_reg(15, sp);
	amiberry_cpu_set_pc(addr);
	amiberry_cpu_fill_prefetch();

	bool return_seen = false;
	PushReturnStack(&return_seen);
	amiberry_cpu_nested_execute_begin();
	while (!return_seen && !s_winuae_quit_requested && !amiberry_cpu_nested_quit_requested())
		amiberry_cpu_execute_interpreter_slice();
	amiberry_cpu_nested_execute_end();
	PopReturnStack();

	amiberry_cpu_clear_mode_change();

	sp = amiberry_cpu_get_reg(15);
	sp += 2;
	amiberry_cpu_set_reg(15, sp);
	amiberry_cpu_set_pc(oldpc);
	amiberry_cpu_fill_prefetch();

	for (int i = 0; i < 8; i++)
		r->d[i] = amiberry_cpu_get_reg(i);
	for (int i = 0; i < 7; i++)
		r->a[i] = amiberry_cpu_get_reg(i + 8);
}

extern const CPUEngine amiberry_cpu_engine = {
	"uae",
	"Amiberry 680x0 Engine (interpreter + ARM64/x86-64 JIT)",
	false,
	CPU_MEM_STRATEGY_DIRECT_POINTER,
	CPU_ENGINE_TIER_PERFORMANCE,
	winuae_init,
	winuae_exit,
	winuae_start,
	winuae_reset,
	winuae_execute_68k,
	winuae_execute_68k_trap,
	winuae_trigger_interrupt,
	winuae_trigger_nmi,
	winuae_intlev,
	winuae_invalidate_code,
	winuae_emulated_ns
};
