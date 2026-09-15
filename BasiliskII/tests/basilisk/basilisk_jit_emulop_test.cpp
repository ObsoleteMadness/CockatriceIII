/*
 * basilisk_jit_emulop_test.cpp - EmulOps called from JIT-compiled code
 *
 * The UAE JIT used to flush its whole translation cache after every EmulOp,
 * on the theory that a host call could leave compiled code with stale state.
 * That flush cost most of the emulator's CPU time, so it is gone. This test
 * holds the JIT to what the flush was standing in for, by running a guest loop
 * at top level (m68k_run_jit, not the interpreter slice Execute68k uses) until
 * it is compiled, with a host call on every iteration that:
 *
 *   - returns a result in D0, which compiled code must pick up;
 *   - every so often runs nested 68k code through Execute68k that clobbers
 *     every register, which the loop's own registers must survive;
 *   - once, halfway, patches an instruction in the *middle* of a compiled
 *     block and reports it through cpu_engine_invalidate_code, which the loop
 *     must see on the next iteration.
 *
 * The same program runs on the UAE interpreter as a reference. JIT goes first:
 * macOS allows one MAP_JIT region per process, so a second JIT activation
 * silently falls back to the interpreter.
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
#include "amiberry_cpu_api.h"

#define POOL_BASE    0x40000	/* toolbox trampolines */
#define OUTER_BASE   0x44000	/* the loop under test */
#define CALLEE_BASE  0x45000	/* routine the loop calls; patched mid-run */
#define NESTED_BASE  0x46000	/* routine the host call runs via Execute68k */
#define NEST_COUNTER 0x47000	/* long incremented by the nested routine */
#define STACK_TOP    0x7f000

#define LOOP_TRAP 0xa933
#define DONE_TRAP 0xa937

static const uint32 kIterations = 4000;
static const uint32 kPatchAt = 2000;	/* host call number that patches CALLEE */
static const uint32 kNestEvery = 97;
static const uint32 kXor = 0x13579bdf;

static uint32 g_calls;
static uint32 g_nested_bad;
static bool   g_done;
static M68kRegisters g_final;

/* Returns straight to the caller, the way a REPLACE hook does. */
static void return_to_caller(M68kRegisters *r)
{
	r->a[0] = ReadMacInt32(r->a[7]);
	r->a[1] = r->a[7] + 4;
}

static ToolboxAction loop_handler(uint16, M68kRegisters *r, uint32, void *)
{
	g_calls++;
	r->d[0] ^= kXor;

	if (g_calls % kNestEvery == 0) {
		M68kRegisters nr;
		for (int i = 0; i < 8; i++) {
			nr.d[i] = 0xc0de0000 + i;
			nr.a[i] = 0xa0de0000 + i;
		}
		Execute68k(NESTED_BASE, &nr);
		if (nr.d[0] != 0x77 || nr.d[2] != 0x11111111)
			g_nested_bad++;
	}

	if (g_calls == kPatchAt) {
		/* Second instruction of the callee: moveq #1,d4 becomes moveq #2,d4. */
		WriteMacInt16(CALLEE_BASE + 2, 0x7802);
		cpu_engine_invalidate_code(CALLEE_BASE + 2, 2);
	}

	return_to_caller(r);
	return TOOLBOX_ACTION_REPLACE;
}

static ToolboxAction done_handler(uint16, M68kRegisters *r, uint32, void *)
{
	g_final = *r;
	g_done = true;
	return_to_caller(r);
	amiberry_cpu_set_mode_change();
	return TOOLBOX_ACTION_REPLACE;
}

static void put16(uint32 *p, uint16 v) { WriteMacInt16(*p, v); *p += 2; }
static void put32(uint32 *p, uint32 v) { WriteMacInt32(*p, v); *p += 4; }

static void build_program(void)
{
	ToolboxTrap_Unregister(LOOP_TRAP);
	ToolboxTrap_Unregister(DONE_TRAP);
	ToolboxTrap_SetStubPool(0);
	ToolboxTrap_Register(LOOP_TRAP, "jit loop hook", loop_handler, NULL);
	ToolboxTrap_Register(DONE_TRAP, "jit done hook", done_handler, NULL);
	ToolboxTrap_SetStubPool(POOL_BASE);
	ToolboxTrap_WriteTrampoline(POOL_BASE, LOOP_TRAP, 0);
	ToolboxTrap_WriteTrampoline(POOL_BASE + 12, DONE_TRAP, 0);

	uint32 p = OUTER_BASE;
	/* Translation is gated on the guest's own instruction cache: compile_block
	 * does nothing until CACR bit 15 is set, which on a real machine the ROM
	 * does at startup. Without this the loop below runs interpreted. */
	put16(&p, 0x203c); put32(&p, 0x00008000);	// move.l #$8000,d0
	put16(&p, 0x4e7b); put16(&p, 0x0002);		// movec d0,cacr
	put16(&p, 0x263c); put32(&p, kIterations);	// move.l #N,d3
	put16(&p, 0x7400);				// moveq #0,d2
	put16(&p, 0x7a00);				// moveq #0,d5
	put16(&p, 0x2a7c); put32(&p, 0x5a5aa5a5);	// movea.l #$5a5aa5a5,a5
	uint32 loop = p;
	put16(&p, 0x2003);				// move.l d3,d0
	put16(&p, 0x4eb9); put32(&p, POOL_BASE);	// jsr loop_stub
	put16(&p, 0xd480);				// add.l d0,d2
	put16(&p, 0x4eb9); put32(&p, CALLEE_BASE);	// jsr callee
	put16(&p, 0xda84);				// add.l d4,d5
	put16(&p, 0x5383);				// subq.l #1,d3
	put16(&p, (uint16)(0x6600 | ((loop - (p + 2)) & 0xff)));	// bne.s loop
	put16(&p, 0x4eb9); put32(&p, POOL_BASE + 12);	// jsr done_stub
	put16(&p, 0x60fe);				// bra.s *
	cpu_engine_invalidate_code(OUTER_BASE, p - OUTER_BASE);

	p = CALLEE_BASE;
	put16(&p, 0x7c00);				// moveq #0,d6
	put16(&p, 0x7801);				// moveq #1,d4
	put16(&p, 0x4e75);				// rts
	cpu_engine_invalidate_code(CALLEE_BASE, p - CALLEE_BASE);

	p = NESTED_BASE;
	put16(&p, 0x7eff);				// moveq #-1,d7
	put16(&p, 0x2a7c); put32(&p, 0xdeadbeef);	// movea.l #$deadbeef,a5
	put16(&p, 0x243c); put32(&p, 0x11111111);	// move.l #$11111111,d2
	put16(&p, 0x7600);				// moveq #0,d3
	put16(&p, 0x7a00);				// moveq #0,d5
	put16(&p, 0x52b9); put32(&p, NEST_COUNTER);	// addq.l #1,counter
	put16(&p, 0x7077);				// moveq #$77,d0
	put16(&p, 0x4e75);				// rts
	cpu_engine_invalidate_code(NESTED_BASE, p - NESTED_BASE);
	WriteMacInt32(NEST_COUNTER, 0);
}

static void test_emulop_loop(const char *engine, bool want_jit)
{
	build_program();
	g_calls = 0;
	g_nested_bad = 0;
	g_done = false;
	memset(&g_final, 0, sizeof(g_final));

	amiberry_cpu_set_sr(0x2700);
	amiberry_cpu_set_reg(15, STACK_TOP);
	amiberry_cpu_set_pc(OUTER_BASE);
	amiberry_cpu_fill_prefetch();
	for (int slice = 0; slice < 100000 && !g_done; slice++)
		amiberry_cpu_execute_slice();
	amiberry_cpu_clear_mode_change();

	uint32 want_d2 = 0;
	for (uint32 i = 1; i <= kIterations; i++)
		want_d2 += i ^ kXor;
	uint32 want_d5 = (kPatchAt - 1) * 1 + (kIterations - kPatchAt + 1) * 2;

	if (want_jit)
		CHECK_ENG(amiberry_cpu_jit_enabled(), engine,
		          "JIT translated the loop (guest enabled the instruction cache)");
	CHECK_ENG(g_done, engine, "loop ran to completion");
	CHECK_ENG(g_calls == kIterations, engine, "host call ran once per iteration");
	CHECK_ENG(g_final.d[3] == 0, engine, "loop counter D3 reached zero");
	CHECK_ENG(g_final.d[2] == want_d2, engine, "D2 accumulated every host call's D0 result");
	CHECK_ENG(g_final.d[5] == want_d5, engine,
	          "D5 saw the mid-block callee patch from the next iteration on");
	CHECK_ENG(g_final.a[5] == 0x5a5aa5a5, engine, "A5 survived nested Execute68k clobbering");
	CHECK_ENG(g_nested_bad == 0, engine, "nested Execute68k returned its own results");
	CHECK_ENG(ReadMacInt32(NEST_COUNTER) == kIterations / kNestEvery, engine,
	          "nested routine ran every time it was called");
	if (g_final.d[5] != want_d5)
		printf("  [INFO] [%s] D5 = %u, want %u\n", engine, g_final.d[5], want_d5);
}

int main(int argc, char **argv)
{
	test_install_crash_handler();
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== basilisk_jit_emulop_test ===\n");

	test_prefs_set_bool("toolbox_hooks", true);

	const char *filter = NULL;
	for (int i = 1; i < argc - 1; i++)
		if (strcmp(argv[i], "--engine") == 0)
			filter = argv[i + 1];

	static const struct { bool jit; const char *label; } kRuns[] = {
		{ true,  "uae+jit" },
		{ false, "uae" },
	};
	for (const auto &run : kRuns) {
		if (filter && strcmp(filter, run.label) != 0 && strcmp(filter, "uae") != 0)
			continue;
		printf("---- %s ----\n", run.label);
		if (!activate_cpu_engine("uae", run.jit)) {
			CHECK_ENG(false, run.label, "activate engine");
			continue;
		}
		test_emulop_loop(run.label, run.jit);
	}

	printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
