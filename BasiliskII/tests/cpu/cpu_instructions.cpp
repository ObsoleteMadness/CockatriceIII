/*
 * cpu_instructions.cpp - 68020/040 instruction snippets through Execute68k
 */

#include "cpu_tests.h"
#include "test_harness.h"

#include <string.h>
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"

void test_cpu_instruction_suite(const char *engine)
{
	printf("Running 680x0 Core Instruction Verification suite (%s)...\n", engine);
	M68kRegisters r;
	uint32 code_addr;

	code_addr = 0x6100;
	WriteMacInt16(code_addr + 0, 0xD081);
	WriteMacInt16(code_addr + 2, 0x9082);
	WriteMacInt16(code_addr + 4, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.d[0] = 1000;
	r.d[1] = 500;
	r.d[2] = 200;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[0] == 1300, engine, "ADD.L + SUB.L (1000 + 500 - 200 == 1300)");

	code_addr = 0x7010;
	WriteMacInt16(code_addr + 0, 0xC0C1);
	WriteMacInt16(code_addr + 2, 0x80C2);
	WriteMacInt16(code_addr + 4, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.d[0] = 300;
	r.d[1] = 200;
	r.d[2] = 300;
	Execute68k(code_addr, &r);
	CHECK_ENG((r.d[0] & 0xFFFF) == 200, engine, "MULU.W + DIVU.W (300 * 200 / 300 == 200)");

	code_addr = 0x7020;
	WriteMacInt16(code_addr + 0, 0xE988);
	WriteMacInt16(code_addr + 2, 0xE888);
	WriteMacInt16(code_addr + 4, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.d[0] = 0x12345678;
	Execute68k(code_addr, &r);
	CHECK_ENG((r.d[0] & 0x0FFFFFFF) == (0x12345678 & 0x0FFFFFFF), engine, "LSL.L + LSR.L roundtrip");

	code_addr = 0x7030;
	WriteMacInt16(code_addr + 0, 0x08C0);
	WriteMacInt16(code_addr + 2, 0x0003);
	WriteMacInt16(code_addr + 4, 0x0840);
	WriteMacInt16(code_addr + 6, 0x0007);
	WriteMacInt16(code_addr + 8, 0x0880);
	WriteMacInt16(code_addr + 10, 0x0003);
	WriteMacInt16(code_addr + 12, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.d[0] = 0x00;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[0] == 0x80, engine, "BSET, BCHG, BCLR set bit 7 and cleared bit 3");

	code_addr = 0x7050;
	WriteMacInt16(code_addr + 0, 0x5481);
	WriteMacInt16(code_addr + 2, 0x51C8);
	WriteMacInt16(code_addr + 4, 0xFFFC);
	WriteMacInt16(code_addr + 6, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.d[0] = 9;
	r.d[1] = 0;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[1] == 20 && (int16)(r.d[0] & 0xFFFF) == -1, engine, "DBRA loop iterated 10 times");

	uint32 movem_buf = 0x8200;
	code_addr = 0x7070;
	WriteMacInt16(code_addr + 0, 0x48D0);
	WriteMacInt16(code_addr + 2, 0x000F);
	WriteMacInt16(code_addr + 4, 0x4280);
	WriteMacInt16(code_addr + 6, 0x4281);
	WriteMacInt16(code_addr + 8, 0x4282);
	WriteMacInt16(code_addr + 10, 0x4283);
	WriteMacInt16(code_addr + 12, 0x4CD0);
	WriteMacInt16(code_addr + 14, 0x000F);
	WriteMacInt16(code_addr + 16, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.a[0] = movem_buf;
	r.d[0] = 0x11111111;
	r.d[1] = 0x22222222;
	r.d[2] = 0x33333333;
	r.d[3] = 0x44444444;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[0] == 0x11111111 && r.d[1] == 0x22222222 && r.d[2] == 0x33333333 && r.d[3] == 0x44444444,
	          engine, "MOVEM.L save and restore D0-D3");

	uint32 sub2_addr = 0x7120;
	uint32 sub1_addr = 0x7100;
	code_addr = 0x7090;
	WriteMacInt16(code_addr + 0, 0x4EB9);
	WriteMacInt32(code_addr + 2, sub1_addr);
	WriteMacInt16(code_addr + 6, 0x4E75);
	WriteMacInt16(sub1_addr + 0, 0x5080);
	WriteMacInt16(sub1_addr + 2, 0x5480);
	WriteMacInt16(sub1_addr + 4, 0x611A);
	WriteMacInt16(sub1_addr + 6, 0x5A80);
	WriteMacInt16(sub1_addr + 8, 0x4E75);
	WriteMacInt16(sub2_addr + 0, 0x5E80);
	WriteMacInt16(sub2_addr + 2, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.d[0] = 100;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[0] == 122, engine, "Nested JSR + BSR + RTS (100 + 10 + 7 + 5 == 122)");

	code_addr = 0x7140;
	WriteMacInt16(code_addr + 0, 0xE9C0);
	WriteMacInt16(code_addr + 2, 0x1210);
	WriteMacInt16(code_addr + 4, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.d[0] = 0x12345678;
	Execute68k(code_addr, &r);
	CHECK_ENG((r.d[1] & 0xFFFF) == 0x3456, engine, "BFEXTU D0 {8:16}, D1 extracted 0x3456");

	code_addr = 0x7160;
	WriteMacInt16(code_addr + 0, 0x4C01);
	WriteMacInt16(code_addr + 2, 0x2C03);
	WriteMacInt16(code_addr + 4, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.d[1] = 0x20000;
	r.d[2] = 0x30000;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[3] == 6 && r.d[2] == 0, engine, "MULU.L 0x20000 * 0x30000 == 6:0");

	code_addr = 0xA000;
	WriteMacInt16(code_addr + 0, 0x4E71);
	WriteMacInt16(code_addr + 2, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.a[2] = 0xA2A2A2A2;
	r.a[3] = 0xA3A3A3A3;
	r.a[4] = 0xA4A4A4A4;
	r.a[5] = 0xA5A5A5A5;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.a[2] == 0xA2A2A2A2 && r.a[3] == 0xA3A3A3A3 &&
	          r.a[4] == 0xA4A4A4A4 && r.a[5] == 0xA5A5A5A5,
	          engine, "A2-A5 unchanged across NOP");

	uint32 a3_payload = 0xA800;
	WriteMacInt32(a3_payload, 0xC0DEF00D);
	code_addr = 0xA010;
	WriteMacInt16(code_addr + 0, 0x2013);
	WriteMacInt16(code_addr + 2, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.a[3] = a3_payload;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[0] == 0xC0DEF00D && r.a[3] == a3_payload,
	          engine, "MOVE.L (A3), D0 loaded guest 0xC0DEF00D");

	code_addr = 0xA120;
	WriteMacInt16(code_addr + 0, 0x4E7B);
	WriteMacInt16(code_addr + 2, 0x0002);
	WriteMacInt16(code_addr + 4, 0x4E7A);
	WriteMacInt16(code_addr + 6, 0x1002);
	WriteMacInt16(code_addr + 8, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.d[0] = 0x80008000;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[1] == 0x80008000, engine, "MOVEC D0,CACR / MOVEC CACR,D1");
}
