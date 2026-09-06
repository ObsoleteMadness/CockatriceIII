/*
 * cpu_interrupt_stress.cpp - Async interrupt pressure during sustained JIT execution
 *
 * Real boot runs under a continuous ~60Hz interrupt fired from a separate host
 * thread (SDL/main_sdl.cpp's tick thread: SetInterruptFlag(INTFLAG_60HZ);
 * TriggerInterrupt();), asynchronous to whatever the CPU thread happens to be
 * executing at that instant - including mid-native-code inside an already
 * JIT-compiled block. Every other test in this suite calls Execute68k() with
 * interrupts quiescent, so none of them exercise that race.
 *
 * This test reproduces the real mechanism: a background std::thread hammers
 * SetInterruptFlag()/TriggerInterrupt() (the same host calls main_sdl.cpp
 * makes) while the main thread runs a long, JIT-hot, register-only guest loop.
 * The loop body touches no memory, so a wrong final result can only come from
 * lost/duplicated iterations or corrupted register state around an interrupt
 * take/return - not a memory-access bug already covered elsewhere.
 */

#include "cpu_tests.h"
#include "test_harness.h"
#include "test_env.h"

#include <stdio.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <thread>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "emul_op.h"
#include "main.h"

/* Guest loop iteration count. Large enough that a JIT-compiled build spends
 * real wall-clock time here (giving the interrupt thread many chances to
 * land mid-native-block); small enough that even the plain interpreter path
 * finishes comfortably inside the timeout below. */
static const uint32 kLoopIterations = 50u * 1000u * 1000u;

static std::atomic<bool> g_stop_interrupt_hammer{false};

/*
 * Fires the same host-side interrupt sequence main_sdl.cpp's 60Hz tick
 * thread uses, far faster than real 60Hz, for as long as the guest loop
 * runs. Each pulse is set then cleared after a brief window rather than
 * left latched: real hardware interrupts are pending only until acked, and
 * a permanently-set InterruptFlags with a guest handler that never acks it
 * makes the CPU re-take a level-1 interrupt after nearly every instruction,
 * starving the loop's forward progress into an unrelated livelock instead
 * of exercising the thing this test wants (an interrupt landing async
 * inside otherwise-uninterrupted compiled execution).
 */
static void interrupt_hammer(void)
{
	while (!g_stop_interrupt_hammer.load(std::memory_order_relaxed)) {
		SetInterruptFlag(INTFLAG_60HZ);
		TriggerInterrupt();
		std::this_thread::sleep_for(std::chrono::microseconds(5));
		ClearInterruptFlag(INTFLAG_60HZ);
		std::this_thread::sleep_for(std::chrono::microseconds(25));
	}
}

void test_interrupt_stress(const char *engine)
{
	printf("Running async interrupt stress test (%s)...\n", engine);

	char label[160];
	snprintf(label, sizeof(label), "[%s] interrupt-under-jit stress", engine);
	run_isolated(label, [engine]() {
		/* Level 1 autovector handler at guest 0x64: bump a RAM counter and
		 * RTE. What the handler does is irrelevant to this test; only that
		 * taking and returning from it repeatedly does not corrupt the
		 * interrupted compiled code's register state. The interrupt
		 * hammer thread acks InterruptFlags itself (see interrupt_hammer),
		 * so the handler does not need to. */
		const uint32 handler = 0xB000;
		const uint32 irq_count_addr = 0xB100;
		const uint32 entry = 0xB200;

		WriteMacInt32(irq_count_addr, 0);

		uint32 p = handler;
		WriteMacInt16(p, 0x52B9); p += 2;      /* addq.l #1,(irq_count_addr).L */
		WriteMacInt32(p, irq_count_addr); p += 4;
		WriteMacInt16(p, 0x4E73); p += 2;      /* rte */
		WriteMacInt32(0x64, handler);          /* level 1 autovector (vector 25) */

		/* andi.w #$f8ff,sr (unmask interrupts -- Execute68k leaves the CPU's
		 * live SR alone, and it is still 0x2700 from reset, mask=7, so no
		 * interrupt would ever be taken without this)
		 * moveq #0,d0 ; move.l #kLoopIterations,d1
		 * loop: addq.l #1,d0 ; subq.l #1,d1 ; bne.s loop
		 * rts */
		p = entry;
		WriteMacInt16(p, 0x027C); p += 2;               /* andi.w #$f8ff,sr */
		WriteMacInt16(p, 0xF8FF); p += 2;
		WriteMacInt16(p, 0x7000); p += 2;               /* moveq #0,d0 */
		WriteMacInt16(p, 0x223C); p += 2;               /* move.l #imm,d1 */
		WriteMacInt32(p, kLoopIterations); p += 4;
		const uint32 loop_pc = p;
		WriteMacInt16(p, 0x5280); p += 2;               /* addq.l #1,d0 */
		WriteMacInt16(p, 0x5381); p += 2;               /* subq.l #1,d1 */
		int16 disp = (int16)(loop_pc - (p + 2));
		WriteMacInt16(p, (uint16)(0x6600 | (disp & 0xFF))); p += 2; /* bne.s loop */
		WriteMacInt16(p, 0x4E75); p += 2;               /* rts */

		cpu_engine_invalidate_code(handler, 0x300);

		const uint32 exec_return = 0xA0E0;
		const uint32 boot_stack = 0x10000;
		WriteMacInt32(boot_stack, exec_return);
		WriteMacInt16(exec_return, (uint16)M68K_EXEC_RETURN);

		g_stop_interrupt_hammer.store(false, std::memory_order_relaxed);
		std::thread hammer(interrupt_hammer);

		M68kRegisters r;
		memset(&r, 0, sizeof(r));
		auto t0 = std::chrono::steady_clock::now();
		Execute68k(entry, &r);
		auto t1 = std::chrono::steady_clock::now();

		g_stop_interrupt_hammer.store(true, std::memory_order_relaxed);
		hammer.join();

		double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
		uint32 irqs = ReadMacInt32(irq_count_addr);
		printf("  [INFO] [%s] loop took %.1fms wall clock, %u interrupts observed\n",
		       engine, ms, irqs);

		CHECK_ENG(r.d[0] == kLoopIterations, engine,
		          "loop accumulator reached exact iteration count under interrupt pressure");
		CHECK_ENG(r.d[1] == 0, engine,
		          "loop counter reached exactly zero under interrupt pressure");
		CHECK_ENG(irqs > 0, engine,
		          "at least one interrupt was actually taken during the loop");
	}, 45);
}
