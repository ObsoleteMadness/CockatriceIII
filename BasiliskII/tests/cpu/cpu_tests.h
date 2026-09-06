/*
 * cpu_tests.h - Multi-engine 680x0 tests (Musashi battery, snippets, FPU)
 */

#ifndef CPU_TESTS_H
#define CPU_TESTS_H

#include <string.h>
#include "test_harness.h"
#include "test_env.h"
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "main.h"
#include "xpram.h"

void test_emulop_and_execute68k(const char *engine);
void test_cpu_instruction_suite(const char *engine);
void test_jsr_stack_integrity(const char *engine);
void test_opcode_battery(const char *engine);
void test_rom_boot_after_reset(const char *engine);
void test_illegal_opcode_exception(const char *engine);
void test_movem_lea_pc_tracking(const char *engine);
void test_jmp_table_low_ram(const char *engine);
void test_jmp_table_rom_range(const char *engine);
void test_execute68k_test_fail_no_pass_reg(const char *engine);
void test_exception_traps(const char *engine);
void test_fpu_execution(const char *engine);
void test_rom_snippets(const char *engine);
void test_rom_patch_apply(const char *engine);
void test_rom_patch_execute(const char *engine);
void test_interrupt_stress(const char *engine);
void test_uae_cputest_smoke(const char *engine);

#endif
