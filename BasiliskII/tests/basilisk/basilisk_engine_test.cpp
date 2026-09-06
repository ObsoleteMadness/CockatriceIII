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

	int count = GetRegisteredCPUEngineCount();
	CHECK(count >= 3, "At least 3 CPU engines registered");

	const CPUEngine *musashi = GetCPUEngine("musashi");
	CHECK(musashi != NULL && strcmp(musashi->id, "musashi") == 0, "Musashi CPU engine found");
	const CPUEngine *m68k_rs = GetCPUEngine("m68k_rs");
	CHECK(m68k_rs != NULL && strcmp(m68k_rs->id, "m68k_rs") == 0, "m68k-rs CPU engine found");
	const CPUEngine *uae = GetCPUEngine("uae");
	CHECK(uae != NULL && strcmp(uae->id, "uae") == 0, "Amiberry/UAE CPU engine found");

	if (musashi)
		CHECK(musashi->is_jit == false, "Musashi correctly flagged as non-JIT interpreter");
	if (m68k_rs)
		CHECK(m68k_rs->is_jit == false, "m68k-rs correctly flagged as non-JIT interpreter");

	CHECK(SetActiveCPUEngine("musashi") == true, "SetActiveCPUEngine('musashi') succeeded");
	CHECK(GetActiveCPUEngine() == musashi, "Active engine is Musashi");
	CHECK(SetActiveCPUEngine("m68k_rs") == true, "SetActiveCPUEngine('m68k_rs') succeeded");
	CHECK(GetActiveCPUEngine() == m68k_rs, "Active engine is m68k-rs");
	CHECK(SetActiveCPUEngine("uae") == true, "SetActiveCPUEngine('uae') succeeded");
	CHECK(GetActiveCPUEngine() == uae, "Active engine is Amiberry/UAE");
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
