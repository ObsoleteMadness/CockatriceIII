/*
 * cpu_regressions.cpp - RESET trampoline, MOVEM/LEA PC, JMP-table (RAM and ROM)
 */

#include "cpu_tests.h"
#include "test_harness.h"
#include "test_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "emul_op.h"

void test_rom_boot_after_reset(const char *engine)
{
	printf("Running ROM-boot RESET/4EFA/JMP sequence (%s)...\n", engine);

	const uint32 a3_payload = 0xA800;
	const uint32 entry = 0xA020;
	const uint32 reset_op = 0xA040;
	const uint32 cont = 0xA060;
	const uint32 exec_return = 0xA0E0;
	const uint32 boot_stack = 0x10000;

	WriteMacInt32(a3_payload, 0xC0DEF00D);
	WriteMacInt32(boot_stack, exec_return);
	WriteMacInt16(exec_return, (uint16)M68K_EXEC_RETURN);

	WriteMacInt16(entry + 0, 0x4EFA);
	WriteMacInt16(entry + 2, (uint16)(reset_op - (entry + 2)));
	WriteMacInt16(reset_op + 0, (uint16)M68K_EMUL_OP_RESET);
	WriteMacInt16(reset_op + 2, 0x4EF9);
	WriteMacInt32(reset_op + 4, cont);
	WriteMacInt16(cont + 0, 0x2013);
	WriteMacInt16(cont + 2, 0x7201);
	WriteMacInt16(cont + 4, 0x4E75);

	M68kRegisters r;
	memset(&r, 0, sizeof(r));
	r.a[3] = a3_payload;
	Execute68k(entry, &r);

	CHECK_ENG(r.d[1] == 1, engine, "ROM-boot sequence reached JMP continuation after RESET");
	CHECK_ENG(r.d[0] == 0xC0DEF00D && r.a[3] == a3_payload, engine,
	          "MOVE.L (A3) after 4EFA/RESET/JMP used guest A3");
	CHECK_ENG(r.a[6] == (RAMBaseMac + RAMSize - 0x1c), engine,
	          "RESET EmulOp installed BootGlobs in A6");
}

void test_movem_lea_pc_tracking(const char *engine)
{
	printf("Running MOVEM.L -(An) / LEA (d16,PC) PC-tracking test (%s)...\n", engine);

	const uint32 code_addr = 0x7250;
	WriteMacInt16(code_addr + 0,  0x48E7);
	WriteMacInt16(code_addr + 2,  0x0706);
	WriteMacInt16(code_addr + 4,  0x4DFA);
	WriteMacInt16(code_addr + 6,  0x0006);
	WriteMacInt16(code_addr + 8,  0x48E7);
	WriteMacInt16(code_addr + 10, 0xE0F0);
	WriteMacInt16(code_addr + 12, 0x4DFA);
	WriteMacInt16(code_addr + 14, 0x000C);
	WriteMacInt16(code_addr + 16, 0x49FA);
	WriteMacInt16(code_addr + 18, 0x0000);
	WriteMacInt16(code_addr + 20, 0x4CDF);
	WriteMacInt16(code_addr + 22, 0x0F07);
	WriteMacInt16(code_addr + 24, 0x4CDF);
	WriteMacInt16(code_addr + 26, 0x60E0);
	WriteMacInt16(code_addr + 28, 0x4E75);

	const uint32 expected_a4 = code_addr + 18;
	M68kRegisters r;
	memset(&r, 0, sizeof(r));
	Execute68k(code_addr, &r);
	CHECK_ENG(r.a[4] == expected_a4, engine,
	          "PC-relative LEA right after MOVEM.L -(An) computed the correct address");
}

void test_jmp_table_low_ram(const char *engine)
{
	printf("Running MOVEM.L / LEA / JMP(d8,PC,Xn.L) table-dispatch (%s, RAM)...\n", engine);

	const uint32 code_addr = 0x7250;
	const uint32 table_addr = 0x9000;
	WriteMacInt16(code_addr + 0,  0x48E7);
	WriteMacInt16(code_addr + 2,  0x0706);
	WriteMacInt16(code_addr + 4,  0x4DFA);
	WriteMacInt16(code_addr + 6,  0x0006);
	WriteMacInt16(code_addr + 8,  0x48E7);
	WriteMacInt16(code_addr + 10, 0xE0F0);
	WriteMacInt16(code_addr + 12, 0x4DFA);
	WriteMacInt16(code_addr + 14, 0x000C);
	WriteMacInt16(code_addr + 16, 0x4BF9);
	WriteMacInt16(code_addr + 18, 0x0000);
	WriteMacInt16(code_addr + 20, 0x1DA0);
	WriteMacInt16(code_addr + 22, 0x4EFB);
	WriteMacInt16(code_addr + 24, 0xD8F8);

	WriteMacInt16(table_addr + 0, 0x4CDF);
	WriteMacInt16(table_addr + 2, 0x0F07);
	WriteMacInt16(table_addr + 4, 0x4CDF);
	WriteMacInt16(table_addr + 6, 0x60E0);
	WriteMacInt16(table_addr + 8, 0x5880);
	WriteMacInt16(table_addr + 10, 0x4E75);

	M68kRegisters r;
	memset(&r, 0, sizeof(r));
	r.d[0] = 100;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[0] == 104, engine, "JMP(d8,PC,A5.L) after MOVEM/LEA landed on jump-table entry");
}

void test_jmp_table_rom_range(const char *engine)
{
	printf("Running JMP-table dispatch in ROM window (%s)...\n", engine);

	uint32 code_addr = ROMBaseMac + 0x00007000;
	uint32 table_addr = code_addr + 0x100;
	int nop_count = 16;

	auto poke16 = [](uint32 addr, uint16 val) {
		Host_Mem_Base[addr + 0] = (uint8)(val >> 8);
		Host_Mem_Base[addr + 1] = (uint8)(val & 0xFF);
	};

	uint32 p = code_addr;
	poke16(p, 0x48E7); p += 2;
	poke16(p, 0x0706); p += 2;
	poke16(p, 0x4DFA); p += 2;
	poke16(p, 0x0006); p += 2;
	for (int i = 0; i < nop_count; i++) {
		poke16(p, 0x4E71);
		p += 2;
	}
	poke16(p, 0x48E7); p += 2;
	poke16(p, 0xE0F0); p += 2;
	poke16(p, 0x4DFA); p += 2;
	poke16(p, 0x000C); p += 2;
	poke16(p, 0x4BF9); p += 2;
	uint32 a5_lo_addr = p;
	p += 4;
	uint32 jmp_addr = p;
	poke16(p, 0x4EFB); p += 2;
	poke16(p, 0xD8F8); p += 2;
	uint32 ext_word_addr = jmp_addr + 2;
	int32_t a5val = (int32_t)table_addr - (int32_t)ext_word_addr + 8;
	poke16(a5_lo_addr + 0, (uint16)((uint32_t)a5val >> 16));
	poke16(a5_lo_addr + 2, (uint16)(a5val & 0xFFFF));
	poke16(table_addr + 0, 0x4CDF);
	poke16(table_addr + 2, 0x0F07);
	poke16(table_addr + 4, 0x4CDF);
	poke16(table_addr + 6, 0x60E0);
	poke16(table_addr + 8, 0x5880);
	poke16(table_addr + 10, 0x4E75);
	cpu_engine_invalidate_code(code_addr, p - code_addr + 0x20);

	M68kRegisters r;
	memset(&r, 0, sizeof(r));
	r.d[0] = 100;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[0] == 104, engine, "ROM-range JMP(d8,PC,A5.L) landed on jump-table entry");
}

void test_execute68k_test_fail_no_pass_reg(const char *engine)
{
	printf("Running Execute68k TEST_FAIL must not reach pass_reg (%s)...\n", engine);

	const uint32 entry = 0x8000;
	const uint32 run_test = 0x8012;
	const uint32 test_fail = 0x8020;
	const uint32 pass_reg = 0x100004;
	const uint32 fail_reg = 0x100000;

	WriteMacInt32(pass_reg, 0xFFFFFFFF);
	WriteMacInt32(fail_reg, 0xFFFFFFFF);

	WriteMacInt16(entry + 0, 0x4EB9);
	WriteMacInt32(entry + 2, run_test);
	WriteMacInt16(entry + 6, 0x23FC);
	WriteMacInt32(entry + 8, 1);
	WriteMacInt32(entry + 12, pass_reg);
	WriteMacInt16(entry + 16, (uint16)M68K_EXEC_RETURN);
	WriteMacInt16(entry + 18, (uint16)M68K_NOP);

	WriteMacInt16(run_test + 0, 0x6000);
	WriteMacInt16(run_test + 2, (uint16)(test_fail - (run_test + 2)));

	WriteMacInt16(test_fail + 0, 0x23FC);
	WriteMacInt32(test_fail + 2, 0);
	WriteMacInt32(test_fail + 6, fail_reg);
	WriteMacInt16(test_fail + 10, (uint16)M68K_EXEC_RETURN);
	WriteMacInt16(test_fail + 12, (uint16)M68K_NOP);

	M68kRegisters r;
	memset(&r, 0, sizeof(r));
	Execute68k(entry, &r);

	CHECK_ENG(ReadMacInt32(fail_reg) == 0, engine, "TEST_FAIL wrote fail_reg");
	CHECK_ENG(ReadMacInt32(pass_reg) == 0xFFFFFFFF, engine,
	          "TEST_FAIL via M68K_EXEC_RETURN did not fall through to pass_reg");
}
