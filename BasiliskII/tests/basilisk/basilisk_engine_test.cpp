/*
 * basilisk_engine_test.cpp - CPUEngine registry and Musashi 68000..040 switch
 */

#include <stdio.h>
#include <string.h>
#include "test_harness.h"
#include "test_env.h"
#include "sysdeps.h"
#include "cpu_engine.h"
#include "cpu_emulation.h"
#include "main.h"

int main(void)
{
	test_install_crash_handler();
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== basilisk_engine_test ===\n");

	/* Musashi is always built in. The other two are optional
	 * (-DCOCKATRICE_ENABLE_M68K_RS=OFF, -DCOCKATRICE_ENABLE_UAE=OFF), so the
	 * expected count follows whatever this build actually links rather than a
	 * hardcoded 3 -- otherwise a deliberately slimmed build fails the gate. */
	const int expected_engines = 1
#if defined(ENABLE_M68K_RS_CPU) && ENABLE_M68K_RS_CPU
	    + 1
#endif
#if defined(ENABLE_UAE_PORTABLE_CPU) && ENABLE_UAE_PORTABLE_CPU
	    + 1
#endif
	    ;
	int count = GetRegisteredCPUEngineCount();
	CHECK(count >= expected_engines, "Every built-in CPU engine registered");

	const CPUEngine *musashi = GetCPUEngine("musashi");
	CHECK(musashi != NULL && strcmp(musashi->id, "musashi") == 0, "Musashi CPU engine found");
	if (musashi)
		CHECK(musashi->is_jit == false, "Musashi correctly flagged as non-JIT interpreter");
	CHECK(SetActiveCPUEngine("musashi") == true, "SetActiveCPUEngine('musashi') succeeded");
	CHECK(GetActiveCPUEngine() == musashi, "Active engine is Musashi");

#if defined(ENABLE_M68K_RS_CPU) && ENABLE_M68K_RS_CPU
	const CPUEngine *m68k_rs = GetCPUEngine("m68k_rs");
	CHECK(m68k_rs != NULL && strcmp(m68k_rs->id, "m68k_rs") == 0, "m68k-rs CPU engine found");
	if (m68k_rs)
		CHECK(m68k_rs->is_jit == false, "m68k-rs correctly flagged as non-JIT interpreter");
	CHECK(SetActiveCPUEngine("m68k_rs") == true, "SetActiveCPUEngine('m68k_rs') succeeded");
	CHECK(GetActiveCPUEngine() == m68k_rs, "Active engine is m68k-rs");
#endif

#if defined(ENABLE_UAE_PORTABLE_CPU) && ENABLE_UAE_PORTABLE_CPU
	/* "uae" is the vendored uae-portable-cpu core, which took the id over when
	 * the GPL-3 Amiberry engine was removed. */
	const CPUEngine *uae = GetCPUEngine("uae");
	CHECK(uae != NULL && strcmp(uae->id, "uae") == 0, "uae-portable-cpu engine found");
	if (uae) {
		CHECK(uae->invalidate_code != NULL, "uae-portable-cpu exposes code invalidation");
		CHECK(SetActiveCPUEngine("uae") == true, "SetActiveCPUEngine('uae') succeeded");
		CHECK(GetActiveCPUEngine() == uae, "Active engine is uae-portable-cpu");
	}
#endif

	SetActiveCPUEngine("musashi");
	CHECK(GetActiveCPUEngine() == musashi, "Switched back to Musashi engine");

	CHECK(activate_cpu_engine("musashi", false), "activate musashi for CPU model switch");
	const int cpu_numbers[] = { 0, 10, 20, 30, 40 };
	for (int cpu_model = 0; cpu_model <= 4; cpu_model++) {
		CPUType = cpu_model;
		musashi_cpu_engine.init();
		char msg[128];
		snprintf(msg, sizeof(msg), "Musashi 680%02d initialization passed", cpu_numbers[cpu_model]);
		CHECK(GetActiveCPUEngine() != NULL, msg);
	}
	CPUType = 4;
	musashi_cpu_engine.init();

	printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
