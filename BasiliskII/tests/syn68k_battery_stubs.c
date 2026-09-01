/*
 * syn68k_battery_stubs.c - Minimal Basilisk hooks for standalone syn68k_battery
 */

#include <stdint.h>

uint32_t cpu_engine_last_pc = 0;

uint32_t syn68k_clean_address(uint32_t addr)
{
	return addr;
}
