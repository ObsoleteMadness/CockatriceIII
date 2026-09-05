/*
 * basilisk_patchguard_test.cpp - PatchROM() must fail loudly, not corrupt (T2)
 *
 * Most patch sites are located by scanning the ROM: a byte signature, an A-trap
 * resolved through the dispatch table, or a ROM resource. Every locator reports
 * failure as offset 0 -- and ROM offset 0 is a real, writable address in the
 * image (it holds the ROM checksum). So a locator that misses does not just
 * skip its patch, it writes the patch over the ROM header and the machine bombs
 * much later somewhere unrelated.
 *
 * Each case here corrupts a *copy* of the loaded ROM so one locator fails, then
 * asserts two things:
 *
 *   1. PatchROM() returns false, rather than reporting success.
 *   2. The bytes at ROM offset 0 are untouched -- the direct signature of the
 *      corruption these guards exist to prevent.
 *
 * Everything runs under run_isolated(), so a guard that faults instead of
 * returning is reported as a failure rather than wedging the suite.
 */

#include <stdio.h>
#include <string.h>
#include <vector>
#include "test_harness.h"
#include "test_env.h"
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "emul_op.h"
#include "rom_patches.h"
#include "main.h"

#define HEADER_BYTES 16

/*
 * Big-endian stores straight into the ROM image.
 *
 * ROM addresses are not writable through WriteMacInt32() in this harness, so
 * these mirror what rom_patches.cpp does: poke ROMBaseHost directly.
 */
static void poke32(uint32 ofs, uint32 val)
{
	uint8 *p = ROMBaseHost + ofs;
	p[0] = (uint8)(val >> 24);
	p[1] = (uint8)(val >> 16);
	p[2] = (uint8)(val >> 8);
	p[3] = (uint8)val;
}

static void poke16(uint32 ofs, uint16 val)
{
	uint8 *p = ROMBaseHost + ofs;
	p[0] = (uint8)(val >> 8);
	p[1] = (uint8)val;
}

/*
 * Reloads a pristine ROM and pins the prefs PatchROM() branches on.
 *
 * Returns:
 *   true if a ROM image was available; callers skip when it is not.
 */
static bool reload_rom(void)
{
	if (test_load_quadra_rom(NULL) < 16)
		return false;
	test_prefs_set_bool("ltoudp", false);
	test_prefs_set_int32("modelid", 29);
	return CheckROM();
}

/* Snapshots the ROM header so we can prove nothing scribbled on it. */
static void snapshot_header(uint8 *out)
{
	memcpy(out, ROMBaseHost, HEADER_BYTES);
}

/*
 * Asserts the outcome of a deliberately-broken patch pass.
 *
 * Arguments:
 *   label:  case name for the messages.
 *   ok:     what PatchROM() returned.
 *   before: header bytes captured before the pass.
 */
static void expect_clean_failure(const char *label, bool ok, const uint8 *before)
{
	char msg[192];
	snprintf(msg, sizeof(msg), "%s: PatchROM() reports failure", label);
	CHECK(!ok, msg);

	snprintf(msg, sizeof(msg), "%s: ROM header not overwritten", label);
	CHECK(memcmp(before, ROMBaseHost, HEADER_BYTES) == 0, msg);
}

/*
 * Makes every find_rom_trap() lookup fail.
 *
 * The compressed dispatch table lives at the offset stored in ROMBase+0x22.
 * A table whose first entry decodes to a zero delta terminates the walk
 * immediately, so every trap resolves to "not found" -- which is the same
 * value find_rom_trap() returns for "unimplemented".
 *
 * Real-world equivalent: a ROM whose dispatch table we cannot decode.
 */
static void test_trap_table_unreadable(void)
{
	if (!reload_rom()) {
		printf("  [SKIP] needs dist/Quadra800.rom\n");
		return;
	}

	// Park the table in a run of zero bytes near the end of the image.
	uint32 zeros = ROMSize - 0x400;
	memset(ROMBaseHost + zeros, 0, 0x100);
	poke32(0x22, zeros);

	uint8 before[HEADER_BYTES];
	snapshot_header(before);
	expect_clean_failure("unreadable trap table", PatchROM(), before);
}

/*
 * Walks the ROM resource map and renames one entry so lookups for it miss.
 *
 * Arguments:
 *   want_type: resource type to hide ('DRVR', 'SERD', ...).
 *   want_id:   resource ID.
 *
 * Returns:
 *   true if the entry was found and renamed.
 */
static bool hide_rom_resource(uint32 want_type, int16 want_id)
{
	uint32 lp = ROMBaseMac + ReadMacInt32(ROMBaseMac + 0x1a);
	uint32 ptr = ReadMacInt32(lp);
	for (int guard = 0; guard < 4096 && ptr; guard++) {
		lp = ROMBaseMac + ptr;
		if (ReadMacInt32(lp + 16) == want_type && ReadMacInt16(lp + 20) == want_id) {
			poke32(lp - ROMBaseMac + 16, 'XXXX');	// no patch looks for this
			return true;
		}
		ptr = ReadMacInt32(lp + 8);
	}
	return false;
}

/*
 * A missing .Sony driver resource must not send the driver blob to offset 0.
 *
 * sony_offset is also the base for the .Disk driver, the disk icons and the
 * PutScrap trampoline, so an unguarded miss scatters several kilobytes over the
 * start of the ROM.
 */
static void test_missing_sony_resource(void)
{
	if (!reload_rom()) {
		printf("  [SKIP] needs dist/Quadra800.rom\n");
		return;
	}
	if (!hide_rom_resource('DRVR', 4)) {
		CHECK(false, "missing .Sony: could not locate DRVR 4 in the resource map");
		return;
	}

	uint8 before[HEADER_BYTES];
	snapshot_header(before);
	expect_clean_failure("missing .Sony (DRVR 4)", PatchROM(), before);
}

/*
 * A missing SERD resource must not write the serial stubs at offset 0.
 */
static void test_missing_serd_resource(void)
{
	if (!reload_rom()) {
		printf("  [SKIP] needs dist/Quadra800.rom\n");
		return;
	}
	if (!hide_rom_resource('SERD', 0)) {
		CHECK(false, "missing SERD: could not locate SERD 0 in the resource map");
		return;
	}

	uint8 before[HEADER_BYTES];
	snapshot_header(before);
	expect_clean_failure("missing SERD 0", PatchROM(), before);
}

/*
 * CheckROM() must reject ROM versions it cannot patch.
 *
 * Its contract says it returns false for an unsupported ROM, but it used to
 * fall through to ROM_VERSION_CLASSIC (0x0276, truthy) for anything it did not
 * recognise -- so an unknown image was accepted and then patched with the
 * Classic offsets, which are raw magic numbers with no verification.
 *
 * The two named-but-unsupported versions come from the master ROM table in
 * $SM/Internal/Asm/LinkedPatchMacros.a.
 */
static void test_checkrom_rejects_unsupported(void)
{
	if (test_load_quadra_rom(NULL) < 16) {
		printf("  [SKIP] needs dist/Quadra800.rom\n");
		return;
	}

	struct { uint16 version; const char *what; } cases[] = {
		{ ROM_VERSION_PORTABLE,   "Portable ($037A)" },
		{ ROM_VERSION_SUPERMARIO, "SuperMario ($077D)" },
		{ 0x0075,                 "Plus ($0075)" },
		{ 0xdead,                 "garbage ($DEAD)" },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		poke16(8, cases[i].version);
		char msg[192];
		snprintf(msg, sizeof(msg), "CheckROM() rejects %s", cases[i].what);
		CHECK(!CheckROM(), msg);
	}

	// ...and still accepts the one it does support.
	poke16(8, ROM_VERSION_32);
	CHECK(CheckROM(), "CheckROM() still accepts 32-bit clean ($067C)");
	CHECK(ROMVersion == ROM_VERSION_32, "ROMVersion set to ROM_VERSION_32");
}

/*
 * Every fixed-offset patch site must verify its bytes before writing.
 *
 * These offsets are bare magic numbers -- there is no signature scan to miss,
 * so on a ROM whose layout differs the write simply lands in unrelated code.
 * Corrupting one byte at each site must produce a named VERIFY FAILED and a
 * refusal, not a silently mispatched ROM.
 *
 * The offsets and what lives at them are documented in
 * docs/rom-patches-vs-supermario.md section 5.
 *
 * The 60 Hz handler at 0xa296 used to be in this list. It is no longer a fixed
 * offset: the handler is reached through the ROM's own jVBLInt vector, and the
 * same signature is verified at whatever address that vector holds. The
 * equivalent guard is test_vbl_vector_resolution() in basilisk_stubabi_test.
 */
static void test_fixed_offsets_are_verified(void)
{
	static const struct { uint32 offset; const char *what; } sites[] = {
		{ 0x1142,  ".Sound open hook" },
		{ 0x1b8f4, "vCheckLoad hook" },
		{ 0x5b78,  "GetDevBase" },
		{ 0x9bc4,  "VIA level-1 dispatcher" },
		{ 0xb2c6a, "InitADB VIA write" },
		{ 0xb2d2e, "InitADB state wait" },
	};

	for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]); i++) {
		if (!reload_rom()) {
			printf("  [SKIP] needs dist/Quadra800.rom\n");
			return;
		}
		// Flip the first byte of the expected instruction.
		ROMBaseHost[sites[i].offset] ^= 0xff;

		uint8 before[HEADER_BYTES];
		snapshot_header(before);
		bool ok = PatchROM();

		char msg[192];
		snprintf(msg, sizeof(msg), "corrupt %s (%06x): PatchROM() refuses",
		         sites[i].what, sites[i].offset);
		CHECK(!ok, msg);
		snprintf(msg, sizeof(msg), "corrupt %s (%06x): ROM header not overwritten",
		         sites[i].what, sites[i].offset);
		CHECK(memcmp(before, ROMBaseHost, HEADER_BYTES) == 0, msg);
	}
}

/*
 * The happy path still works: an untouched ROM patches cleanly and every
 * required patch lands.
 */
static void test_clean_rom_still_patches(void)
{
	if (!reload_rom()) {
		printf("  [SKIP] needs dist/Quadra800.rom\n");
		return;
	}
	CHECK(PatchROM(), "unmodified ROM still patches successfully");

	int missing_required = 0;
	const std::vector<PatchRecord> &log = GetPatchLog();
	for (size_t i = 0; i < log.size(); i++)
		if (log[i].required && !log[i].applied)
			missing_required++;
	CHECK(missing_required == 0, "no required patch is missing on a clean ROM");
}

int main(void)
{
	test_install_crash_handler();
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== basilisk_patchguard_test ===\n");
	CHECK(activate_cpu_engine("musashi", false), "activate musashi");

	run_isolated("trap table unreadable", test_trap_table_unreadable);
	run_isolated("missing .Sony", test_missing_sony_resource);
	run_isolated("missing SERD", test_missing_serd_resource);
	run_isolated("CheckROM rejects unsupported", test_checkrom_rejects_unsupported);
	run_isolated("fixed offsets verified", test_fixed_offsets_are_verified);
	run_isolated("clean ROM", test_clean_rom_still_patches);

	printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
