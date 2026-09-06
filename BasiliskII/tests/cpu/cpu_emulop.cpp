/*
 * cpu_emulop.cpp - Execute68k, EmulOp CLKNOMEM, and Execute68kTrap
 */

#include "cpu_tests.h"
#include "test_harness.h"
#include "test_env.h"

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "emul_op.h"
#include "main.h"

void test_emulop_and_execute68k(const char *engine)
{
	printf("Running EmulOp and Execute68k tests (%s)...\n", engine);

	uint32 code_addr = 0x4000;
	WriteMacInt16(code_addr + 0, 0x0680);
	WriteMacInt32(code_addr + 2, 0x11111111);
	WriteMacInt16(code_addr + 6, 0x4E75);

	struct M68kRegisters r;
	memset(&r, 0, sizeof(r));
	r.d[0] = 0x22222222;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[0] == 0x33333333, engine, "Execute68k added 0x11111111 to D0");

	XPRAM[0x10] = 0x5A;
	r.d[1] = 0x000040B8;
	WriteMacInt16(0x5000, M68K_EMUL_OP_CLKNOMEM);
	WriteMacInt16(0x5002, 0x4E75);
	Execute68k(0x5000, &r);
	CHECK_ENG((r.d[2] & 0xFF) == 0x5A, engine, "EmulOp M68K_EMUL_OP_CLKNOMEM executed correctly");

	uint32 line_a_handler = 0x9000;
	WriteMacInt16(line_a_handler + 0, 0x54AF);
	WriteMacInt16(line_a_handler + 2, 0x0002);
	WriteMacInt16(line_a_handler + 4, 0x5E80);
	WriteMacInt16(line_a_handler + 6, 0x4E73);
	WriteMacInt32(0x28, line_a_handler);

	memset(&r, 0, sizeof(r));
	r.d[0] = 100;
	Execute68kTrap(0xA000, &r);
	CHECK_ENG(r.d[0] == 107, engine, "Execute68kTrap(0xA000) Line-A trap returned via RTE (100 -> 107)");

	r.d[0] = 50;
	Execute68kTrap(0xA122, &r);
	CHECK_ENG(r.d[0] == 57, engine, "Execute68kTrap(0xA122) stack alignment (50 -> 57)");
}
