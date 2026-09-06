/*
 * basilisk_toolbox_test.cpp - Toolbox/OS trap trampoline dispatch, on every engine
 *
 * toolbox_traps.cpp is Cockatrice's mechanism for hooking a Toolbox or OS trap
 * the way Mac OS itself does -- a RAM trampoline installed through
 * _SetToolTrap / _SetOSTrapAddress, rather than a byte patch into the ROM
 * image. It is the piece Phase 3c builds on, so it has to be right on all three
 * CPU engines before anything is moved onto it.
 *
 * The whole mechanism turns on one question the dispatcher has to answer:
 * *which* hooked trap am I running for? All the trampolines share a single
 * EmulOp word, so the only thing that distinguishes them is where in guest
 * memory that word sits. ToolboxTrap_Dispatch() answers it from the guest PC,
 * and the answer must be right regardless of which engine is executing and
 * whether that engine has advanced its PC past the trapping word yet.
 *
 * Every case here therefore runs on musashi, m68k_rs and uae (interpreter and
 * JIT). Run with --engine <id> to narrow it.
 */

#include <stdio.h>
#include <string.h>
#include "test_harness.h"
#include "test_env.h"
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "emul_op.h"
#include "toolbox_traps.h"
#include "main.h"

#define POOL_BASE   0x30000	/* trampoline pool */
#define CALLER_BASE 0x34000	/* the 68k code that "calls" a hooked trap */
#define ROM_BASE    0x35000	/* stand-in for the original ROM routine */

/* What the handler saw, so the test can check it after the run. */
static uint16 g_seen_trap;
static uint32 g_seen_original;
static void  *g_seen_user;
static int    g_call_count;
static ToolboxAction g_action;

static void reset_observations(void)
{
	g_seen_trap = 0;
	g_seen_original = 0;
	g_seen_user = NULL;
	g_call_count = 0;
}

/*
 * Records what the dispatcher passed in and returns the action the case under
 * test asked for.
 */
static ToolboxAction observing_handler(uint16 trap_num, struct M68kRegisters *r,
                                       uint32 original_addr, void *user_data)
{
	g_seen_trap = trap_num;
	g_seen_original = original_addr;
	g_seen_user = user_data;
	g_call_count++;

	if (g_action == TOOLBOX_ACTION_REPLACE) {
		/*
		 * Stand in for the ROM routine: pop the return address into A0 and
		 * hand back the caller's stack in A1, which is what the trampoline's
		 * "move.l a1,a7 / jmp (a0)" tail consumes. D0 carries a value the test
		 * can recognise.
		 */
		r->a[0] = ReadMacInt32(r->a[7]);
		r->a[1] = r->a[7] + 4;
		r->d[0] = 0x5eaf00d;
	}
	return g_action;
}

/*
 * Registers three hooks and lays their trampolines out in a pool.
 *
 * Three, not one, because a dispatcher that always returns the first slot
 * would pass a single-trampoline test.
 */
static const uint16 kTraps[3] = { 0xa937, 0xa933, 0xa02e };	/* two Toolbox, one OS */

static void build_pool(void)
{
	for (int i = 0; i < 3; i++)
		ToolboxTrap_Unregister(kTraps[i]);
	ToolboxTrap_SetStubPool(0);

	static int tags[3] = { 11, 22, 33 };
	for (int i = 0; i < 3; i++)
		ToolboxTrap_Register(kTraps[i], "test hook", observing_handler, &tags[i]);

	// Writing a trampoline claims its slot, so the cursor ends up past all three.
	ToolboxTrap_SetStubPool(POOL_BASE);
	for (int i = 0; i < 3; i++)
		ToolboxTrap_WriteTrampoline(POOL_BASE + i * 12, kTraps[i], ROM_BASE + i * 0x10);
}

/*
 * Calls one trampoline the way the 68k trap dispatcher would, and returns
 * through it.
 *
 *   jsr (stub).L      the trampoline ends in jmp (a0), so this returns here
 *   rts
 *
 * Arguments:
 *   slot: index of the trampoline in the pool.
 *   r:    register state in and out.
 */
static void call_stub(int slot, M68kRegisters *r)
{
	uint32 stub = POOL_BASE + slot * 12;
	WriteMacInt16(CALLER_BASE + 0, 0x4eb9);			// jsr (stub).L
	WriteMacInt32(CALLER_BASE + 2, stub);
	WriteMacInt16(CALLER_BASE + 6, 0x4e75);			// rts
	cpu_engine_invalidate_code(CALLER_BASE, 8);
	Execute68k(CALLER_BASE, r);
}

/*
 * Passthrough: the handler runs, then control continues into the original
 * routine with the caller's stack intact.
 *
 * The stand-in "ROM routine" is "moveq #<slot+1>,d1 / rts", so D1 says which
 * original address the trampoline actually jumped to -- the check that the
 * dispatcher matched the right slot rather than merely running something.
 */
static void test_passthrough(const char *engine)
{
	build_pool();
	g_action = TOOLBOX_ACTION_PASSTHROUGH;

	for (int i = 0; i < 3; i++) {
		uint32 rom = ROM_BASE + i * 0x10;
		WriteMacInt16(rom + 0, (uint16)(0x7200 | (i + 1)));	// moveq #i+1,d1
		WriteMacInt16(rom + 2, 0x4e75);						// rts
		cpu_engine_invalidate_code(rom, 4);
	}

	for (int i = 0; i < 3; i++) {
		reset_observations();
		M68kRegisters r;
		memset(&r, 0, sizeof(r));
		r.d[1] = 0xffffffff;
		call_stub(i, &r);

		char msg[192];
		snprintf(msg, sizeof(msg), "passthrough slot %d: handler ran exactly once", i);
		CHECK_ENG(g_call_count == 1, engine, msg);
		snprintf(msg, sizeof(msg), "passthrough slot %d: handler saw trap 0x%04x", i, kTraps[i]);
		CHECK_ENG(g_seen_trap == kTraps[i], engine, msg);
		snprintf(msg, sizeof(msg), "passthrough slot %d: handler saw its own original address", i);
		CHECK_ENG(g_seen_original == ROM_BASE + (uint32)i * 0x10, engine, msg);
		snprintf(msg, sizeof(msg), "passthrough slot %d: handler saw its own user_data", i);
		CHECK_ENG(g_seen_user != NULL && *(int *)g_seen_user == 11 + i * 11, engine, msg);
		snprintf(msg, sizeof(msg), "passthrough slot %d: control reached that original routine", i);
		CHECK_ENG(r.d[1] == (uint32)(i + 1), engine, msg);
	}
}

/*
 * Replace: the handler returns straight to the caller without entering the
 * original routine at all.
 */
static void test_replace(const char *engine)
{
	build_pool();
	g_action = TOOLBOX_ACTION_REPLACE;

	// Make the original routines detectable: reaching one sets D1.
	for (int i = 0; i < 3; i++) {
		uint32 rom = ROM_BASE + i * 0x10;
		WriteMacInt16(rom + 0, 0x72ff);		// moveq #-1,d1
		WriteMacInt16(rom + 2, 0x4e75);		// rts
		cpu_engine_invalidate_code(rom, 4);
	}

	reset_observations();
	M68kRegisters r;
	memset(&r, 0, sizeof(r));
	r.d[0] = 0;
	r.d[1] = 0;
	call_stub(1, &r);

	CHECK_ENG(g_call_count == 1, engine, "replace: handler ran exactly once");
	CHECK_ENG(g_seen_trap == kTraps[1], engine, "replace: handler saw trap 0xa933");
	CHECK_ENG(r.d[0] == 0x5eaf00d, engine, "replace: handler's result reached the caller in D0");
	CHECK_ENG(r.d[1] == 0, engine, "replace: the original routine was not entered");
}

/*
 * A dispatch from outside the pool must be refused, not acted on.
 *
 * This is the shape of the bug being fixed: the dispatcher used to take the PC
 * on faith, so a wrong PC produced a bogus trap number and a jmp through a
 * garbage A0. Now an out-of-pool PC leaves A0 alone, and the EmulOp word
 * planted in ordinary RAM below just falls through to the rts after it.
 */
static void test_dispatch_outside_pool(const char *engine)
{
	build_pool();
	g_action = TOOLBOX_ACTION_PASSTHROUGH;
	reset_observations();

	uint32 stray = CALLER_BASE + 0x100;
	WriteMacInt16(stray + 0, (uint16)M68K_EMUL_OP_TOOLBOX_DISPATCH);
	WriteMacInt16(stray + 2, 0x4e75);		// rts
	cpu_engine_invalidate_code(stray, 4);

	M68kRegisters r;
	memset(&r, 0, sizeof(r));
	r.a[0] = 0xa0a0a0a0;
	Execute68k(stray, &r);

	CHECK_ENG(g_call_count == 0, engine, "stray dispatch: no handler was invoked");
	CHECK_ENG(r.a[0] == 0xa0a0a0a0, engine, "stray dispatch: A0 left alone rather than set from garbage");
}

int main(int argc, char **argv)
{
	test_install_crash_handler();
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== basilisk_toolbox_test ===\n");

	// ToolboxTrap_Register() is gated on this pref and silently refuses without it.
	test_prefs_set_bool("toolbox_hooks", true);

	const char *filter = NULL;
	for (int i = 1; i < argc - 1; i++)
		if (strcmp(argv[i], "--engine") == 0)
			filter = argv[i + 1];

	for (int i = 0; i < kTestEngineConfigCount; i++) {
		const TestEngineConfig &cfg = kTestEngineConfigs[i];
		if (!test_engine_matches(filter, &cfg))
			continue;
		printf("---- %s ----\n", cfg.label);
		if (!activate_cpu_engine(cfg.id, cfg.jit, cfg.jitfpu)) {
			CHECK_ENG(false, cfg.label, "activate engine");
			continue;
		}
		test_passthrough(cfg.label);
		test_replace(cfg.label);
		test_dispatch_outside_pool(cfg.label);
	}

	printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
