/*
 * basilisk_scc_test.cpp - Z8530 SCC registers, MMIO, 68k MOVE.B
 */

#include <stdio.h>
#include <string.h>
#include "test_harness.h"
#include "test_env.h"
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "scc.h"
#include "main.h"

int main(void)
{
	test_install_crash_handler();
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== basilisk_scc_test ===\n");
	CHECK(activate_cpu_engine("musashi", false), "activate musashi");

	SCCInit();
	SCC_Reset();

	uint32 rr0_a = SCC_Access(0, false, 1);
	CHECK((rr0_a & 0x04) != 0, "SCC Channel A RR0 Tx Buffer Empty flag is set");
	uint32 rr0_b = SCC_Access(0, false, 0);
	CHECK((rr0_b & 0x04) != 0, "SCC Channel B RR0 Tx Buffer Empty flag is set");

	SCC_Access(0x0C, true, 1);
	SCC_Access(0x55, true, 1);
	SCC_Access(0x0D, true, 1);
	SCC_Access(0xAA, true, 1);
	SCC_Access(0x0C, true, 1);
	uint32 rr12 = SCC_Access(0, false, 1);
	CHECK(rr12 == 0x55, "SCC WR12 time constant low byte written and read back");
	SCC_Access(0x0D, true, 1);
	uint32 rr13 = SCC_Access(0, false, 1);
	CHECK(rr13 == 0xAA, "SCC WR13 time constant high byte written and read back");

	uint32 mmio_rr0_a = ReadMacInt8(0x50000002);
	CHECK((mmio_rr0_a & 0x04) != 0, "32-bit MMIO ReadMacInt8(0x50000002) Channel A status");
	WriteMacInt8(0x50000002, 0x0C);
	WriteMacInt8(0x50000002, 0x33);
	WriteMacInt8(0x50000002, 0x0C);
	uint32 mmio_rr12 = ReadMacInt8(0x50000002);
	CHECK(mmio_rr12 == 0x33, "32-bit MMIO WR12 roundtrip");

	uint32 code_addr = 0x8200;
	WriteMacInt16(code_addr + 0, 0x1039);
	WriteMacInt32(code_addr + 2, 0x50000002);
	WriteMacInt16(code_addr + 6, 0x4E75);
	struct M68kRegisters r;
	memset(&r, 0, sizeof(r));
	Execute68k(code_addr, &r);
	CHECK((r.d[0] & 0x04) != 0, "68k MOVE.B (0x50000002), D0 read SCC Channel A RR0");

	SCC_Reset();
	WriteMacInt8(0x50F00002, 0x0C);
	WriteMacInt8(0x50F00002, 0x7E);
	WriteMacInt8(0x50F00002, 0x0C);
	uint32 mmio_upper_rr12 = ReadMacInt8(0x50F00002);
	CHECK(mmio_upper_rr12 == 0x7E, "32-bit MMIO at 0x50F00002 (Quadra 800 SCC range)");

	WriteMacInt8(0x50F00002, 0x0C);
	uint32 cpu_upper = ReadMacInt8(0x50F00002);
	CHECK(cpu_upper == 0x7E, "RR12 in upper 32-bit SCC window");

	printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
