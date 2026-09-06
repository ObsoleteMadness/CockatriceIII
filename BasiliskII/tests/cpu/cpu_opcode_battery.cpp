/*
 * cpu_opcode_battery.cpp - Musashi mc68000/mc68040 .bin images through every engine
 *
 * Each image is run in run_isolated() at TEST_DEFAULT_TIMEOUT so a hung
 * translation cannot stall the rest of the battery. interrupt.bin needs real
 * MMIO IRQ delivery (harness gap). rtd.bin fails on musashi itself.
 */

#include "cpu_tests.h"
#include "test_harness.h"
#include "test_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <vector>
#include <string>
#include <algorithm>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "emul_op.h"

static const char *musashi_test_dir(void)
{
	const char *env = getenv("MUSASHI_TEST_DIR");
	if (env && env[0])
		return env;
#ifdef MUSASHI_TEST_DIR
	return MUSASHI_TEST_DIR;
#else
	return "../Musashi/test";
#endif
}

static void run_one_opcode_image(const char *engine, const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		char msg[300];
		snprintf(msg, sizeof(msg), "opcode battery: could not open %s", path);
		CHECK_ENG(false, engine, msg);
		return;
	}
	static uint8_t buf[0x10000];
	size_t n = fread(buf, 1, sizeof(buf), f);
	fclose(f);

	/* Musashi images end with STOP #0x2700 (pass and TEST_FAIL). Execute68k must
	 * return to C++ without unwinding the guest stack: RTS at TEST_FAIL would pop
	 * the jsr run_test return and fall through to mov.l #1,TEST_PASS_REG. */
	for (size_t off = 0; off + 4 <= n; off++) {
		if (buf[off] == 0x4E && buf[off + 1] == 0x72 &&
		    buf[off + 2] == 0x27 && buf[off + 3] == 0x00) {
			buf[off] = (uint8)(M68K_EXEC_RETURN >> 8);
			buf[off + 1] = (uint8)(M68K_EXEC_RETURN & 0xff);
			buf[off + 2] = (uint8)(M68K_NOP >> 8);
			buf[off + 3] = (uint8)(M68K_NOP & 0xff);
		}
	}

	for (uint32 a = 0; a < 0x10000; a += 4)
		WriteMacInt32(a, 0);
	for (uint32 a = 0; a < n; a++)
		WriteMacInt8(0x10000 + a, buf[a]);
	for (uint32 a = 0x100000; a < 0x100024; a += 4)
		WriteMacInt32(a, 0xFFFFFFFF);
	for (uint32 a = 0x300000; a < 0x310000; a += 4)
		WriteMacInt32(a, 0);
	cpu_engine_invalidate_code(0, ~0);

	M68kRegisters r;
	memset(&r, 0, sizeof(r));
	Execute68k(0x10000, &r);

	uint32 fail_reg = ReadMacInt32(0x100000);
	uint32 pass_reg = ReadMacInt32(0x100004);
	char msg[300];
	snprintf(msg, sizeof(msg), "opcode battery: %s (pass_reg=%08X fail_reg=%08X)",
	         path, pass_reg, fail_reg);
	CHECK_ENG(pass_reg == 1 && fail_reg == 0xFFFFFFFF, engine, msg);
}

void test_opcode_battery(const char *engine)
{
	static const char *skip_list[] = { "interrupt.bin", "rtd.bin" };
	/* m68k-rs upstream intentionally diverges from legacy Musashi BCD/CHK2 fixtures. */
	static const char *m68k_rs_skip[] = { "abcd.bin", "sbcd.bin", "chk2.bin", "cmp2.bin" };
	static const char *dirs[] = { "mc68000", "mc68040" };
	const char *base = musashi_test_dir();
	int total = 0, ran = 0;

	for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
		char dirpath[512];
		snprintf(dirpath, sizeof(dirpath), "%s/%s", base, dirs[d]);
		DIR *dp = opendir(dirpath);
		if (!dp) {
			printf("  [CPU-TEST] opcode battery: %s not found -- skipping\n", dirpath);
			continue;
		}
		std::vector<std::string> names;
		struct dirent *ent;
		while ((ent = readdir(dp)) != NULL) {
			std::string n = ent->d_name;
			if (n.size() > 4 && n.compare(n.size() - 4, 4, ".bin") == 0)
				names.push_back(n);
		}
		closedir(dp);
		std::sort(names.begin(), names.end());

		for (size_t i = 0; i < names.size(); i++) {
			bool skip = false;
			for (size_t s = 0; s < sizeof(skip_list) / sizeof(skip_list[0]); s++)
				if (names[i] == skip_list[s])
					skip = true;
			if (!skip && strcmp(engine, "m68k_rs") == 0) {
				for (size_t s = 0; s < sizeof(m68k_rs_skip) / sizeof(m68k_rs_skip[0]); s++)
					if (names[i] == m68k_rs_skip[s])
						skip = true;
			}
			if (skip)
				continue;

			total++;
			std::string path = std::string(dirpath) + "/" + names[i];
			char label[360];
			snprintf(label, sizeof(label), "[%s] %s", engine, path.c_str());
			run_isolated(label, [engine, path]() {
				run_one_opcode_image(engine, path.c_str());
			});
			ran++;
		}
	}
	printf("  [CPU-TEST] opcode battery: %d/%d image(s) run (%s)\n", ran, total, engine);
}
