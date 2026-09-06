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

/*
 * Loads the real Quadra 800 ROM, runs it through the production CheckROM()/
 * PatchROM() path (the same "Patching a 32-bit clean ROM" step every boot
 * does), and validates specific patch bytes actually landed. Unlike
 * test_rom_snippets() above, this does not hand-craft trampolines: it
 * exercises the real patcher's raw ROMBaseHost writes across every engine
 * config, including the FlushCodeCache()/cpu_engine_invalidate_code() call
 * PatchROM() makes when it is done, so a per-engine code-cache-invalidation
 * bug on the actual patch set (not a synthetic poke) would show up here.
 */
void test_rom_patch_apply(const char *engine)
{
	printf("Running ROM CheckROM/PatchROM apply-and-validate test (%s)...\n", engine);

	size_t n = test_load_quadra_rom(NULL);
	if (n < 0x200) {
		printf("  [SKIP] ROM patch test requires dist/Quadra800.rom\n");
		return;
	}

	CHECK_ENG(CheckROM(), engine, "CheckROM accepts Quadra 800 (32-bit clean) image");
	CHECK_ENG(ROMVersion == ROM_VERSION_32, engine, "ROMVersion is ROM_VERSION_32 (0x067c)");

	bool patched = PatchROM();
	CHECK_ENG(patched, engine, "PatchROM succeeded on dist/Quadra800.rom");
	if (!patched)
		return;

	/* ROM+0x8c: "install special reset opcode and jump" (patch_rom_32).
	 * Validates both the planted EmulOp/JMP opcodes and the exact 32-bit
	 * jump target patch_rom_32 computes, not just "some patch happened". */
	CHECK_ENG(ReadMacInt16(ROMBaseMac + 0x8c) == M68K_EMUL_OP_RESET, engine,
	          "PatchROM planted RESET EmulOp at ROM+0x8c");
	CHECK_ENG(ReadMacInt16(ROMBaseMac + 0x8e) == M68K_JMP, engine,
	          "PatchROM planted JMP opcode at ROM+0x8e");
	CHECK_ENG(ReadMacInt32(ROMBaseMac + 0x90) == ROMBaseMac + 0xba, engine,
	          "PatchROM's RESET jump target is ROM+0xba");

	/* ROM+0xc2: "Don't GetHardwareInfo" — two NOPs, an independent patch
	 * site from +0x8c so a partial/misaligned patch application still fails
	 * this even if the first checkpoint happens to look right. */
	CHECK_ENG(ReadMacInt16(ROMBaseMac + 0xc2) == M68K_NOP, engine,
	          "PatchROM NOPed GetHardwareInfo call at ROM+0xc2 (word 1)");
	CHECK_ENG(ReadMacInt16(ROMBaseMac + 0xc4) == M68K_NOP, engine,
	          "PatchROM NOPed GetHardwareInfo call at ROM+0xc4 (word 2)");
}

/*
 * Goes one step past test_rom_patch_apply(): actually executes the bytes
 * PatchROM() itself just wrote, through the real CPU/engine dispatch
 * (Execute68k), rather than only reading them back. This is the write-then-
 * execute sequence every real boot performs at "Patching a 32-bit clean
 * ROM" - PatchROM() pokes ROMBaseHost directly (host pointer writes, same
 * as poke16_rom()), then the CPU fetches and runs those exact bytes for the
 * first time. A JIT engine that caches/misreads a stale translation for a
 * freshly-(self-)patched region, or whose direct-mode codegen mishandles
 * the newly-written opcodes, would fail here even though
 * test_rom_patch_apply()'s plain ReadMacInt16/32 checks already passed.
 *
 * PatchROM's own RESET+JMP at ROM+0x8c targets ROM+0xba, which is real,
 * unmodified Apple hardware-init ROM code this harness cannot run (no SCC/
 * VIA/sound emulation here). That landing spot is redirected to a small
 * controlled stub so the test can observe a clean result without needing a
 * full boot environment past the patched entry itself.
 */
void test_rom_patch_execute(const char *engine)
{
	printf("Running PatchROM write-then-execute test (%s)...\n", engine);

	size_t n = test_load_quadra_rom(NULL);
	if (n < 0x200) {
		printf("  [SKIP] PatchROM execute test requires dist/Quadra800.rom\n");
		return;
	}

	if (!CheckROM()) {
		CHECK_ENG(false, engine, "CheckROM accepts Quadra 800 (32-bit clean) image");
		return;
	}
	bool patched = PatchROM();
	CHECK_ENG(patched, engine, "PatchROM succeeded on dist/Quadra800.rom");
	if (!patched)
		return;

	const uint32 jmp_target = ROMBaseMac + 0xba;
	if (ReadMacInt32(ROMBaseMac + 0x90) != jmp_target) {
		CHECK_ENG(false, engine, "PatchROM's RESET jump target is ROM+0xba (precondition)");
		return;
	}

	/* Redirect the patched JMP's landing spot to a controlled stub:
	 * move.l (a3),d0 ; moveq #1,d1 ; rts -- same shape test_rom_snippets()
	 * uses for its own synthetic continuation. */
	poke16_rom(jmp_target + 0, 0x2013);
	poke16_rom(jmp_target + 2, 0x7201);
	poke16_rom(jmp_target + 4, 0x4E75);
	cpu_engine_invalidate_code(ROMBaseMac, 0x200);

	const uint32 a3_payload = 0xA800;
	const uint32 exec_return = 0xA0E0;
	const uint32 boot_stack = 0x10000;
	WriteMacInt32(a3_payload, 0xC0DEF00D);
	WriteMacInt32(boot_stack, exec_return);
	WriteMacInt16(exec_return, (uint16)M68K_EXEC_RETURN);

	char label[160];
	snprintf(label, sizeof(label), "[%s] PatchROM write-then-execute", engine);
	run_isolated(label, [engine, a3_payload]() {
		M68kRegisters r;
		memset(&r, 0, sizeof(r));
		r.a[3] = a3_payload;
		/* Real ROM+0x2A is Apple's genuine reset-vector JMP into +0x8c,
		 * which PatchROM() just overwrote with its own RESET EmulOp+JMP. */
		Execute68k(ROMBaseMac + 0x2A, &r);
		CHECK_ENG(r.d[1] == 1, engine,
		          "CPU executed PatchROM's freshly-written RESET EmulOp + JMP and reached the stub");
		CHECK_ENG(r.d[0] == 0xC0DEF00D && r.a[3] == a3_payload, engine,
		          "PatchROM's patched entry preserved guest A3 across RESET EmulOp + JMP");
	});
}
