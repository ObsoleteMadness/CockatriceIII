/*
 * cpu_test_main.cpp - Runs the 680x0 suite on musashi, UAE, and m68k-rs
 *
 * Usage: cpu_tests [--engine musashi|m68k_rs|uae|all]
 *
 * Each hang-prone test runs in run_isolated() (30s default). Known CPU
 * failures are still reported; they are not skipped except interrupt.bin/rtd.bin.
 */

#include <stdio.h>
#include <string.h>
#include "test_harness.h"
#include "test_env.h"
#include "cpu_tests.h"
#include "cpu_engine.h"

int main(int argc, char **argv)
{
	test_install_crash_handler();
	setvbuf(stdout, NULL, _IONBF, 0);

	const char *filter = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--engine") == 0 && i + 1 < argc)
			filter = argv[++i];
		else if (strncmp(argv[i], "--engine=", 9) == 0)
			filter = argv[i] + 9;
	}

	printf("=== CockatriceIII CPU Test Suite ===\n");

	for (int i = 0; i < kTestEngineConfigCount; i++) {
		const TestEngineConfig *cfg = &kTestEngineConfigs[i];
		if (!test_engine_matches(filter, cfg))
			continue;

		printf("\n=== CPU engine: %s (id=%s jit=%s jitfpu=%s) ===\n",
		       cfg->label, cfg->id, cfg->jit ? "true" : "false",
		       cfg->jitfpu ? "true" : "false");

		char init_msg[160];
		snprintf(init_msg, sizeof(init_msg), "activate_cpu_engine('%s', jit=%s) succeeded",
		         cfg->id, cfg->jit ? "true" : "false");
		bool ok = activate_cpu_engine(cfg->id, cfg->jit, cfg->jitfpu);
		CHECK(ok, init_msg);
		if (!ok)
			continue;

		char label[160];
		snprintf(label, sizeof(label), "[%s] emulop/execute68k", cfg->label);
		run_isolated(label, [cfg]() { test_emulop_and_execute68k(cfg->label); });

		snprintf(label, sizeof(label), "[%s] instructions", cfg->label);
		run_isolated(label, [cfg]() { test_cpu_instruction_suite(cfg->label); });

		snprintf(label, sizeof(label), "[%s] jsr/stack", cfg->label);
		run_isolated(label, [cfg]() { test_jsr_stack_integrity(cfg->label); });

		test_opcode_battery(cfg->label);

		snprintf(label, sizeof(label), "[%s] rom-boot-reset", cfg->label);
		run_isolated(label, [cfg]() { test_rom_boot_after_reset(cfg->label); });

		snprintf(label, sizeof(label), "[%s] illegal-0x773F", cfg->label);
		run_isolated(label, [cfg]() { test_illegal_opcode_exception(cfg->label); });

		snprintf(label, sizeof(label), "[%s] movem/lea pc", cfg->label);
		run_isolated(label, [cfg]() { test_movem_lea_pc_tracking(cfg->label); });

		snprintf(label, sizeof(label), "[%s] jmp-table RAM", cfg->label);
		run_isolated(label, [cfg]() { test_jmp_table_low_ram(cfg->label); });

		snprintf(label, sizeof(label), "[%s] jmp-table ROM", cfg->label);
		run_isolated(label, [cfg]() { test_jmp_table_rom_range(cfg->label); });

		snprintf(label, sizeof(label), "[%s] test-fail-exec-return", cfg->label);
		run_isolated(label, [cfg]() { test_execute68k_test_fail_no_pass_reg(cfg->label); });

		if (!cfg->jit) {
			snprintf(label, sizeof(label), "[%s] exception traps", cfg->label);
			run_isolated(label, [cfg]() { test_exception_traps(cfg->label); });
		}
		if (strcmp(cfg->id, "musashi") == 0 || strcmp(cfg->id, "uae") == 0 ||
		    strcmp(cfg->id, "m68k_rs") == 0) {
			snprintf(label, sizeof(label), "[%s] fpu", cfg->label);
			run_isolated(label, [cfg]() { test_fpu_execution(cfg->label); });
		}

		if (strcmp(cfg->id, "uae") == 0) {
			snprintf(label, sizeof(label), "[%s] uae-cputest", cfg->label);
			run_isolated(label, [cfg]() { test_uae_cputest_smoke(cfg->label); }, 120);
		}

		test_rom_snippets(cfg->label);
		test_rom_patch_apply(cfg->label);
		test_rom_patch_execute(cfg->label);
		test_interrupt_stress(cfg->label);
	}

	printf("\nCPU suite results: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
