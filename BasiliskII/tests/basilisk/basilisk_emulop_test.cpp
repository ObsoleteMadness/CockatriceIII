/*
 * basilisk_emulop_test.cpp - Execute68k, CLKNOMEM EmulOp, Execute68kTrap on Musashi
 */

#include <stdio.h>
#include <string.h>
#include "test_harness.h"
#include "test_env.h"
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "emul_op.h"
#include "main.h"
#include "xpram.h"

int main(void)
{
	test_install_crash_handler();
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== basilisk_emulop_test ===\n");
	CHECK(activate_cpu_engine("musashi", false), "activate musashi");

	uint32 code_addr = 0x4000;
	WriteMacInt16(code_addr + 0, 0x0680);
	WriteMacInt32(code_addr + 2, 0x11111111);
	WriteMacInt16(code_addr + 6, 0x4E75);
	struct M68kRegisters r;
	memset(&r, 0, sizeof(r));
	r.d[0] = 0x22222222;
	Execute68k(code_addr, &r);
	CHECK(r.d[0] == 0x33333333, "Execute68k added 0x11111111 to D0");

	XPRAM[0x10] = 0x5A;
	r.d[1] = 0x000040B8;
	WriteMacInt16(0x5000, M68K_EMUL_OP_CLKNOMEM);
	WriteMacInt16(0x5002, 0x4E75);
	Execute68k(0x5000, &r);
	CHECK((r.d[2] & 0xFF) == 0x5A, "EmulOp M68K_EMUL_OP_CLKNOMEM");

	uint32 line_a_handler = 0x9000;
	WriteMacInt16(line_a_handler + 0, 0x54AF);
	WriteMacInt16(line_a_handler + 2, 0x0002);
	WriteMacInt16(line_a_handler + 4, 0x5E80);
	WriteMacInt16(line_a_handler + 6, 0x4E73);
	WriteMacInt32(0x28, line_a_handler);
	memset(&r, 0, sizeof(r));
	r.d[0] = 100;
	Execute68kTrap(0xA000, &r);
	CHECK(r.d[0] == 107, "Execute68kTrap(0xA000) Line-A (100 -> 107)");

	const uint32 wide_addr = 0x6000;
	WriteMacInt32(wide_addr, 0xDEADBEEF);
	WriteMacInt32(wide_addr + 4, 0xDEADBEEF);
	memset(&r, 0, sizeof(r));
	r.a[0] = wide_addr;
	r.d[0] = 0x12345678;
	EmulOp(M68K_EMUL_OP_MICROSECONDS, &r);
	CHECK(r.a[0] != wide_addr || r.d[0] != 0x12345678,
		"Microseconds EmulOp returns 64-bit time in A0/D0");
	CHECK(ReadMacInt32(wide_addr) == 0xDEADBEEF && ReadMacInt32(wide_addr + 4) == 0xDEADBEEF,
		"Microseconds EmulOp does not write through A0");

	printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
