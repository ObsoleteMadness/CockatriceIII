/*
 * basilisk_romguard_test.cpp - ROM write guard (memory_set_rom_write_guard)
 *
 * With jitdirect, translated code stores to guest memory inline, so the ROM
 * pages are made read-only on the host while the guest runs. Two things must
 * then hold on every host (POSIX signals, Windows vectored exceptions):
 *
 *   - host code inside an EmulOp may still patch ROM: its first write faults,
 *     the fault handler unlocks ROM, and leaving the EmulOp locks it again;
 *   - a translated guest store that reaches ROM faults into the JIT's own
 *     handler, which drops it, and the guest carries on.
 *
 * The guard is armed by the engine's start(), which the tests never call, so
 * this suite arms it directly around Execute68k. Only the uae engine uses it;
 * the other engines write ROM through accessors that already drop the write.
 */

#include <stdio.h>
#include <string.h>
#include "test_harness.h"
#include "test_env.h"
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "main.h"

// ROM offsets the checks write to; far from anything the harness plants
static const uint32 kHostPatchOff = 0x300;
static const uint32 kGuestStoreOff = 0x200;

/*
 * Host write to guarded ROM from inside an EmulOp bracket.
 *
 * Arguments:
 *   engine: Label for CHECK messages.
 */
static void test_host_patch(const char *engine)
{
	memory_set_rom_write_guard(true);
	memory_host_call_enter();
	// Faults once; the handler unlocks ROM and the write goes through
	ROMBaseHost[kHostPatchOff] = 0x77;
	memory_host_call_leave();
	memory_set_rom_write_guard(false);
	CHECK_ENG(ROMBaseHost[kHostPatchOff] == 0x77, engine,
	          "host write to guarded ROM inside an EmulOp goes through");
}

/*
 * A guest byte store translated against RAM and then aimed at ROM, the
 * Device Manager pattern that first broke jitdirect.
 *
 * Arguments:
 *   engine: Label for CHECK messages.
 */
static void test_guest_store(const char *engine)
{
	uint32 code = 0x4000;
	uint32 ram_target = 0x8000;
	uint32 rom_target = ROMBaseMac + kGuestStoreOff;

	static const uint16 program[] = {
		0x203C, 0x8000, 0x8000,	// MOVE.L #$80008000,D0
		0x4E7B, 0x0002,		// MOVEC D0,CACR: the JIT translates nothing until this is set
		0x343C, 0x07CF,		// MOVE.W #1999,D2
		0x1081,			// loop: MOVE.B D1,(A0)
		0x0C42, 0x03E8,		// CMP.W #1000,D2
		0x6602,			// BNE.S skip
		0x2049,			// MOVEA.L A1,A0 (from now on the store hits ROM)
		0x51CA, 0xFFF4,		// skip: DBRA D2,loop
		0x4E75			// RTS
	};
	for (size_t i = 0; i < sizeof(program) / sizeof(program[0]); i++)
		WriteMacInt16(code + 2 * i, program[i]);
	cpu_engine_invalidate_code(code, sizeof(program));

	ROMBaseHost[kGuestStoreOff] = 0xC3;
	WriteMacInt8(ram_target, 0);

	M68kRegisters r;
	memset(&r, 0, sizeof(r));
	r.d[1] = 0x5A;
	r.a[0] = ram_target;
	r.a[1] = rom_target;

	memory_set_rom_write_guard(true);
	Execute68k(code, &r);
	memory_set_rom_write_guard(false);

	CHECK_ENG(ReadMacInt8(ram_target) == 0x5A, engine, "guest store reached RAM");
	CHECK_ENG(ROMBaseHost[kGuestStoreOff] == 0xC3, engine,
	          "guest store to guarded ROM was dropped, ROM unchanged");
	CHECK_ENG((r.d[2] & 0xffff) == 0xffff, engine, "guest loop ran to completion");
}

int main(int argc, char **argv)
{
	test_install_crash_handler();
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== basilisk_romguard_test ===\n");

	const char *filter = NULL;
	for (int i = 1; i < argc - 1; i++)
		if (strcmp(argv[i], "--engine") == 0)
			filter = argv[i + 1];

	bool ran = false;
	for (int i = 0; i < kTestEngineConfigCount; i++) {
		const TestEngineConfig &cfg = kTestEngineConfigs[i];
		if (strcmp(cfg.id, "uae") != 0 || !test_engine_matches(filter, &cfg))
			continue;
		ran = true;
		printf("---- %s ----\n", cfg.label);
		if (!activate_cpu_engine(cfg.id, cfg.jit, cfg.jitfpu, cfg.jitdirect)) {
			CHECK_ENG(false, cfg.label, "activate engine");
			continue;
		}
		const char *label = cfg.label;
		char name[96];
		snprintf(name, sizeof(name), "[%s] host patch", label);
		run_isolated(name, [label]() { test_host_patch(label); });
		snprintf(name, sizeof(name), "[%s] guest store", label);
		run_isolated(name, [label]() { test_guest_store(label); });
	}
	if (!ran)
		printf("  [SKIP] uae engine not built\n");

	printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
