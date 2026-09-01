/*
 * cpu_exceptions.cpp - TRAP, TRAPV, divide-by-zero, illegal 0x773F
 */

#include "cpu_tests.h"
#include "test_harness.h"

#include <string.h>
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"

void test_exception_traps(const char *engine)
{
	printf("Running 680x0 Exception Traps verification suite (%s)...\n", engine);
	M68kRegisters r;
	uint32 code_addr;

	uint32 trap0_handler = 0x8000;
	WriteMacInt16(trap0_handler + 0, 0x5A80);
	WriteMacInt16(trap0_handler + 2, 0x4E73);
	WriteMacInt32(0x80, trap0_handler);
	code_addr = 0x7200;
	WriteMacInt16(code_addr + 0, 0x4E40);
	WriteMacInt16(code_addr + 2, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.d[0] = 10;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[0] == 15, engine, "TRAP #0 vectored to handler and returned via RTE");

	uint32 trapv_handler = 0x8010;
	WriteMacInt16(trapv_handler + 0, 0x5080);
	WriteMacInt16(trapv_handler + 2, 0x4E73);
	WriteMacInt32(0x1C, trapv_handler);
	code_addr = 0x7210;
	WriteMacInt16(code_addr + 0, 0x4E76);
	WriteMacInt16(code_addr + 2, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.d[0] = 10;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[0] == 10, engine, "TRAPV with V=0 did not take trap");

	uint32 code_addr_v1 = 0x7230;
	WriteMacInt16(code_addr_v1 + 0, 0x003C);
	WriteMacInt16(code_addr_v1 + 2, 0x0002);
	WriteMacInt16(code_addr_v1 + 4, 0x4E76);
	WriteMacInt16(code_addr_v1 + 6, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.d[0] = 10;
	Execute68k(code_addr_v1, &r);
	CHECK_ENG(r.d[0] == 18, engine, "TRAPV with V=1 vectored to handler (10 -> 18)");

	uint32 divzero_handler = 0x8020;
	WriteMacInt16(divzero_handler + 0, 0x7063);
	WriteMacInt16(divzero_handler + 2, 0x4E73);
	WriteMacInt32(0x14, divzero_handler);
	code_addr = 0x7220;
	WriteMacInt16(code_addr + 0, 0x81C1);
	WriteMacInt16(code_addr + 2, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.d[0] = 50;
	r.d[1] = 0;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[0] == 99, engine, "DIVS.W by zero vectored to Vector 5");
}

void test_illegal_opcode_exception(const char *engine)
{
	printf("Running Illegal Instruction (opcode 0x773F) isolation test (%s)...\n", engine);

	const uint32 illegal_handler = 0x8030;
	const uint32 code_addr = 0x7240;
	WriteMacInt16(illegal_handler + 0, 0x222F);
	WriteMacInt16(illegal_handler + 2, 0x0002);
	WriteMacInt16(illegal_handler + 4, 0x54AF);
	WriteMacInt16(illegal_handler + 6, 0x0002);
	WriteMacInt16(illegal_handler + 8, 0x5880);
	WriteMacInt16(illegal_handler + 10, 0x4E73);
	WriteMacInt32(0x10, illegal_handler);

	WriteMacInt16(code_addr + 0, 0x773F);
	WriteMacInt16(code_addr + 2, 0x4E75);

	M68kRegisters r;
	memset(&r, 0, sizeof(r));
	r.d[0] = 20;
	r.d[1] = 0;
	Execute68k(code_addr, &r);

	CHECK_ENG(r.d[1] == code_addr, engine, "Illegal opcode 0x773F pushed the faulting PC");
	CHECK_ENG(r.d[0] == 24, engine, "Illegal opcode 0x773F vectored to Vector 4 (20 -> 24)");
}
