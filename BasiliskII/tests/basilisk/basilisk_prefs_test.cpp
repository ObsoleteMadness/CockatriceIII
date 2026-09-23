/*
 * basilisk_prefs_test.cpp - CockatriceIII_Prefs line syntax (PrefsParseLine)
 *
 * The prefs store itself is stubbed in the test environment, so this drives
 * the line parser directly: the part that decides what keyword and value a
 * line in a user's prefs file actually means.
 */

#include <stdio.h>
#include <string.h>
#include "test_harness.h"
#include "test_env.h"
#include "sysdeps.h"
#include "prefs.h"

/*
 * Parses one line and checks the result.
 *
 * Arguments:
 *   text: Line as it would come from fgets() (may include CR/LF).
 *   want_key: Expected keyword, or NULL when the line should be skipped.
 *   want_value: Expected value (ignored when want_key is NULL).
 *   what: Description printed with the check.
 */
static void expect_line(const char *text, const char *want_key, const char *want_value,
                        const char *what)
{
	char buf[256];
	char *key = NULL, *value = NULL;

	snprintf(buf, sizeof(buf), "%s", text);
	bool ok = PrefsParseLine(buf, &key, &value);
	if (want_key == NULL) {
		CHECK(!ok, what);
		return;
	}
	CHECK(ok && strcmp(key, want_key) == 0 && strcmp(value, want_value) == 0, what);
	if (ok && (strcmp(key, want_key) != 0 || strcmp(value, want_value) != 0))
		printf("    got keyword [%s] value [%s]\n", key, value);
}

int main(void)
{
	test_install_crash_handler();
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== basilisk_prefs_test ===\n");

	expect_line("rom Quadra800.rom\n", "rom", "Quadra800.rom", "plain keyword and value");
	expect_line("ramsize\t67108864\n", "ramsize", "67108864", "tab between keyword and value");
	expect_line("jit true   # use the JIT\n", "jit", "true",
	            "trailing # comment is dropped, so a boolean still reads as true");
	expect_line("cpu_emulator uae ; musashi, uae or m68k_rs\n", "cpu_emulator", "uae",
	            "trailing ; comment is dropped");
	expect_line("jitdirect true#not a comment\n", "jitdirect", "true#not a comment",
	            "# with no space before it is part of the value, not a comment");
	expect_line("disk /Volumes/Mac#2.hda\n", "disk", "/Volumes/Mac#2.hda",
	            "# inside a path is kept");
	expect_line("scsi0 /Users/me/Mac Disks/HD.hda\n", "scsi0", "/Users/me/Mac Disks/HD.hda",
	            "value with spaces is kept whole");
	expect_line("scsi1 /Users/me/Mac Disks/CD.iso  # the CD\n", "scsi1", "/Users/me/Mac Disks/CD.iso",
	            "value with spaces, then a comment");
	expect_line("fpu true\r\n", "fpu", "true", "CRLF line ending is ignored");
	expect_line("screen win/1152/870   \n", "screen", "win/1152/870", "trailing whitespace is ignored");
	expect_line("   ether slirp\n", "ether", "slirp", "leading whitespace is ignored");

	expect_line("\n", NULL, NULL, "blank line is skipped");
	expect_line("   \t \r\n", NULL, NULL, "whitespace-only line is skipped");
	expect_line("# a comment\n", NULL, NULL, "# comment line is skipped");
	expect_line("; a comment\n", NULL, NULL, "; comment line is skipped");
	expect_line("  # indented comment\n", NULL, NULL, "indented comment line is skipped");
	expect_line("nogui\n", NULL, NULL, "keyword without a value is skipped");
	expect_line("nogui   # no value\n", NULL, NULL, "keyword with only a comment is skipped");

	printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
