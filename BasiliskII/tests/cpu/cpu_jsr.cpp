/*
 * cpu_jsr.cpp - JSR/BSR/JMP/RTS call-stack integrity
 */

#include "cpu_tests.h"
#include "test_harness.h"

#include <stdio.h>
#include <string.h>
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"

void test_jsr_stack_integrity(const char *engine)
{
	printf("Running JSR/BSR/JMP/RTS call-stack integrity tests (%s)...\n", engine);
	M68kRegisters r;
	uint32 code_addr;

	code_addr = 0x7300;
	uint32 target_addr = 0x7340;
	WriteMacInt16(code_addr + 0, 0x4E91);
	WriteMacInt16(code_addr + 2, 0x4E75);
	WriteMacInt16(target_addr + 0, 0x5880);
	WriteMacInt16(target_addr + 2, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.a[1] = target_addr;
	r.d[0] = 10;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[0] == 14, engine, "JSR (An) indirect call returned correctly");

	code_addr = 0x7360;
	uint32 stub_addr = 0x7380;
	uint32 tail_addr = 0x73A0;
	WriteMacInt16(code_addr + 0, 0x4EB9);
	WriteMacInt32(code_addr + 2, stub_addr);
	WriteMacInt16(code_addr + 6, 0x4E75);
	WriteMacInt16(stub_addr + 0, 0x4EF9);
	WriteMacInt32(stub_addr + 2, tail_addr);
	WriteMacInt16(tail_addr + 0, 0x5980);
	WriteMacInt16(tail_addr + 2, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.d[0] = 100;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[0] == 96, engine, "JSR -> JMP -> RTS tail-call returned to the original caller");

	code_addr = 0x7400;
	uint32 handler_addr = 0x7420;
	uint32 after_addr = code_addr + 10;
	WriteMacInt16(code_addr + 0, 0x487A);
	WriteMacInt16(code_addr + 2, (uint16)(after_addr - (code_addr + 4)));
	WriteMacInt16(code_addr + 4, 0x4EF9);
	WriteMacInt32(code_addr + 6, handler_addr);
	WriteMacInt16(after_addr + 0, 0x5280);
	WriteMacInt16(after_addr + 2, 0x4E75);
	WriteMacInt16(handler_addr + 0, 0x5480);
	WriteMacInt16(handler_addr + 2, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.d[0] = 0;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[0] == 3, engine, "hand-built return frame (PEA+JMP) unwound via RTS");

	code_addr = 0x7500;
	uint32 loop_addr = code_addr;
	uint32 sum1_addr = 0x7520;
	const int iterations = 5000;
	WriteMacInt16(loop_addr + 0, 0x4EB9);
	WriteMacInt32(loop_addr + 2, sum1_addr);
	WriteMacInt16(loop_addr + 6, 0x51C9);
	WriteMacInt16(loop_addr + 8, (uint16)(loop_addr - (loop_addr + 6 + 2)));
	WriteMacInt16(loop_addr + 10, 0x4E75);
	WriteMacInt16(sum1_addr + 0, 0x5280);
	WriteMacInt16(sum1_addr + 2, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.d[0] = 0;
	r.d[1] = (uint32)(iterations - 1);
	Execute68k(code_addr, &r);
	char msg4[160];
	snprintf(msg4, sizeof(msg4), "%d looped JSR/RTS calls left D0 == %d (got %u)",
	         iterations, iterations, r.d[0]);
	CHECK_ENG(r.d[0] == (uint32)iterations, engine, msg4);
}
