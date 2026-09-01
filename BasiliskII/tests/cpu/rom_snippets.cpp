/*
 * rom_snippets.cpp - Execute slices of dist/Quadra800.rom through each engine
 *
 * The Quadra 800 image lives at dist/Quadra800.rom. Tests copy known offsets
 * into guest ROM (WriteMacInt drops ROM writes) and run them under the 30s
 * isolator. Missing ROM is a skip, not a failure.
 */

#include "cpu_tests.h"
#include "test_harness.h"
#include "test_env.h"

#include <stdio.h>
#include <string.h>
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "emul_op.h"
#include "rom_patches.h"

/*
 * Writes a 16-bit 680x0 word into ROM via Host_Mem_Base.
 *
 * WriteMacInt16 is a no-op on the ROM window, so snippet tests poke host
 * bytes then invalidate the engine's code cache.
 *
 * Arguments:
 *   addr: Macintosh address of the word.
 *   val: Big-endian 16-bit opcode or immediate.
 */
static void poke16_rom(uint32 addr, uint16 val)
{
	Host_Mem_Base[addr + 0] = (uint8)(val >> 8);
	Host_Mem_Base[addr + 1] = (uint8)(val & 0xFF);
}

void test_rom_snippets(const char *engine)
{
	printf("Running Quadra 800 ROM snippet tests (%s)...\n", engine);

	size_t n = test_load_quadra_rom(NULL);
	if (n < 0x200) {
		printf("  [SKIP] ROM snippets require dist/Quadra800.rom\n");
		return;
	}

	/* Real 4EFA at +0x2A, plant RESET+JMP to a stub continuation in ROM padding. */
	const uint32 a3_payload = 0xA800;
	const uint32 exec_return = 0xA0E0;
	const uint32 boot_stack = 0x10000;
	WriteMacInt32(a3_payload, 0xC0DEF00D);
	WriteMacInt32(boot_stack, exec_return);
	WriteMacInt16(exec_return, (uint16)M68K_EXEC_RETURN);

	uint32 reset_op = ROMBaseMac + 0x8C;
	uint32 cont = ROMBaseMac + 0x200; /* unused ROM padding used as stub */
	poke16_rom(reset_op + 0, (uint16)M68K_EMUL_OP_RESET);
	poke16_rom(reset_op + 2, 0x4EF9);
	Host_Mem_Base[reset_op + 4] = (uint8)(cont >> 24);
	Host_Mem_Base[reset_op + 5] = (uint8)(cont >> 16);
	Host_Mem_Base[reset_op + 6] = (uint8)(cont >> 8);
	Host_Mem_Base[reset_op + 7] = (uint8)cont;
	poke16_rom(cont + 0, 0x2013);
	poke16_rom(cont + 2, 0x7201);
	poke16_rom(cont + 4, 0x4E75);
	cpu_engine_invalidate_code(ROMBaseMac, 0x300);

	char label2[160];
	snprintf(label2, sizeof(label2), "[%s] ROM+0x2A RESET", engine);
	run_isolated(label2, [engine, a3_payload]() {
		M68kRegisters r2;
		memset(&r2, 0, sizeof(r2));
		r2.a[3] = a3_payload;
		Execute68k(ROMBaseMac + 0x2A, &r2);
		CHECK_ENG(r2.d[1] == 1, engine, "real ROM 4EFA at +0x2A reached RESET continuation stub");
		CHECK_ENG(r2.d[0] == 0xC0DEF00D && r2.a[3] == a3_payload, engine,
		          "real ROM RESET trampoline preserved guest A3");
	});
}
