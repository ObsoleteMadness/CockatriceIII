/*
 * basilisk_patches_test.cpp - Synthetic RESET trampoline plus CheckROM/PatchROM
 */

#include <stdio.h>
#include <string.h>
#include "test_harness.h"
#include "test_env.h"
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "emul_op.h"
#include "rom_patches.h"
#include "main.h"

int main(void)
{
	test_install_crash_handler();
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== basilisk_patches_test ===\n");
	CHECK(activate_cpu_engine("musashi", false), "activate musashi");

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
	CHECK(r.d[1] == 1, "synthetic RESET trampoline reached continuation");
	CHECK(r.d[0] == 0xC0DEF00D && r.a[3] == a3_payload, "A3 preserved across RESET EmulOp");
	CHECK(r.a[6] == (RAMBaseMac + RAMSize - 0x1c), "BootGlobs in A6");

	size_t n = test_load_quadra_rom(NULL);
	if (n >= 16) {
		CHECK(CheckROM(), "CheckROM accepts Quadra 800 (32-bit clean) image");
		CHECK(ROMVersion == ROM_VERSION_32, "ROMVersion is ROM_VERSION_32 (0x067c)");
		bool patched = PatchROM();
		CHECK(patched, "PatchROM succeeded on dist/Quadra800.rom");
		if (patched) {
			uint16 op = ReadMacInt16(ROMBaseMac + 0x8C);
			CHECK(op == M68K_EMUL_OP_RESET || op == 0x7103,
			      "PatchROM planted RESET EmulOp near ROM+0x8C (or equivalent 32-bit patch)");
		}
	} else {
		printf("  [SKIP] CheckROM/PatchROM need dist/Quadra800.rom\n");
	}

	printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
