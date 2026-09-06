/*
 * basilisk_patches_test.cpp - Synthetic RESET trampoline plus CheckROM/PatchROM
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "test_harness.h"
#include "test_env.h"
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "emul_op.h"
#include "rom_patches.h"
#include "main.h"

/*
 * Golden patch manifest (T1).
 *
 * PatchROM() records every patch it attempts (see GetPatchLog()). Comparing the
 * whole log against a checked-in manifest turns "a patch stopped matching this
 * ROM" from a boot bomb tens of thousands of instructions later into a one-line
 * test diff.
 *
 * The manifest is specific to dist/Quadra800.rom and to the prefs pinned in
 * main(); it deliberately includes ROM offsets so that a signature matching a
 * *different* place is also caught.
 *
 * Regenerate after an intentional change with:
 *     REGEN_PATCH_MANIFEST=1 ./basilisk_patches_test
 * and read the diff before committing it.
 */

#define MANIFEST_REL "/BasiliskII/tests/basilisk/fixtures/quadra800_patches.txt"

/*
 * Renders the patch log in manifest form: one "name<TAB>offset<TAB>flags" line
 * per record, in application order.
 */
static std::string render_patch_manifest(void)
{
	std::string out;
	char line[512];
	const std::vector<PatchRecord> &log = GetPatchLog();
	for (size_t i = 0; i < log.size(); i++) {
		snprintf(line, sizeof(line), "%s\t%06x\t%s\t%s\n",
		         log[i].name, log[i].offset,
		         log[i].required ? "required" : "optional",
		         log[i].applied ? "applied" : "MISSED");
		out += line;
	}
	return out;
}

/*
 * Compares the current patch log against the checked-in manifest.
 *
 * Reports the first differing line rather than a wall of text, since a single
 * moved patch shifts nothing else.
 */
static void check_patch_manifest(void)
{
	std::string path = std::string(TEST_REPO_ROOT) + MANIFEST_REL;
	std::string actual = render_patch_manifest();

	if (getenv("REGEN_PATCH_MANIFEST")) {
		FILE *f = fopen(path.c_str(), "w");
		if (!f) {
			CHECK(false, "regenerate manifest: cannot open fixture for writing");
			return;
		}
		fwrite(actual.data(), 1, actual.size(), f);
		fclose(f);
		printf("  [REGEN] wrote %zu patch records to %s\n",
		       GetPatchLog().size(), path.c_str());
		return;
	}

	FILE *f = fopen(path.c_str(), "r");
	if (!f) {
		printf("  [SKIP] no patch manifest at %s (run REGEN_PATCH_MANIFEST=1)\n", path.c_str());
		return;
	}
	std::string expected;
	char buf[4096];
	size_t got;
	while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
		expected.append(buf, got);
	fclose(f);

	if (expected == actual) {
		char msg[160];
		snprintf(msg, sizeof(msg), "patch manifest matches (%zu records)", GetPatchLog().size());
		CHECK(true, msg);
		return;
	}

	// Report the first divergence; everything after it is usually noise.
	size_t ea = 0, aa = 0;
	int lineno = 1;
	while (ea < expected.size() && aa < actual.size()) {
		size_t ee = expected.find('\n', ea), ae = actual.find('\n', aa);
		std::string el = expected.substr(ea, ee - ea);
		std::string al = actual.substr(aa, ae - aa);
		if (el != al) {
			printf("  manifest line %d:\n    expected: %s\n    actual:   %s\n",
			       lineno, el.c_str(), al.c_str());
			break;
		}
		ea = ee + 1; aa = ae + 1; lineno++;
	}
	if (ea >= expected.size() && aa < actual.size())
		printf("  manifest: %zu extra record(s) after line %d\n", GetPatchLog().size(), lineno - 1);
	else if (aa >= actual.size() && ea < expected.size())
		printf("  manifest: missing record(s) from line %d\n", lineno);
	CHECK(false, "patch manifest matches");
}

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
		/*
		 * Pin the prefs PatchROM() branches on, so the manifest describes one
		 * fixed configuration. In particular the test stub defaults "ltoudp" to
		 * true, which would skip the SERD/serial patch that a normal boot
		 * applies -- see PrefsFindBool() in tests/include/test_env.cpp.
		 */
		test_prefs_set_bool("ltoudp", false);
		test_prefs_set_int32("modelid", 29);	// Quadra 800

		CHECK(CheckROM(), "CheckROM accepts Quadra 800 (32-bit clean) image");
		CHECK(ROMVersion == ROM_VERSION_32, "ROMVersion is ROM_VERSION_32 (0x067c)");
		bool patched = PatchROM();
		CHECK(patched, "PatchROM succeeded on dist/Quadra800.rom");
		if (patched) {
			uint16 op = ReadMacInt16(ROMBaseMac + 0x8C);
			CHECK(op == M68K_EMUL_OP_RESET || op == 0x7103,
			      "PatchROM planted RESET EmulOp near ROM+0x8C (or equivalent 32-bit patch)");
			check_patch_manifest();
		}
	} else {
		printf("  [SKIP] CheckROM/PatchROM need dist/Quadra800.rom\n");
	}

	printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
