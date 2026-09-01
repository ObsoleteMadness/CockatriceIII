/*
 *  emu68_support.c - Hosted replacements for the bits of support.c we link
 *
 *  Upstream support.c is freestanding (memcpy, sprintf, WFI hangs, EL1
 *  cache ops). This TARGET uses libc and Darwin/Win32 icache APIs.
 */

#include "support.h"
#include "M68k.h"
#include "emu68_hosted.h"

#include <math.h>
#include <stdio.h>
#include <stdarg.h>

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#endif

/*
 * Flushes instruction cache for a newly written JIT region.
 *
 * Parameters:
 *   addr   - Host address of the first byte.
 *   length - Number of bytes to make executable.
 */
void arm_flush_cache(uintptr_t addr, uint32_t length)
{
#if defined(__APPLE__)
	sys_icache_invalidate((void *)addr, length);
#else
	(void)addr;
	(void)length;
#endif
}

void arm_flush_dcache_for_jit(uintptr_t addr, uint32_t length)
{
	arm_flush_cache(addr, length);
}

void arm_flush_icache_for_jit(uintptr_t addr, uint32_t length)
{
	arm_flush_cache(addr, length);
}

void arm_icache_invalidate(uintptr_t addr, uint32_t length)
{
	arm_flush_cache(addr, length);
}

void arm_dcache_invalidate(uintptr_t addr, uint32_t length)
{
	(void)addr;
	(void)length;
}

/*
 * Materializes a host pointer for blr from translated code. AArch64 movz/movk
 * halfwords are little-endian (hw0 = bits 15:0).
 *
 * Parameters:
 *   ctx - Translator context.
 *   rd  - Destination X register.
 *   ptr - Host address of the C helper.
 */
void EMIT_LoadHostPointer(struct TranslatorContext *ctx, uint8_t rd, uintptr_t ptr)
{
	EMIT(ctx,
		mov64_immed_u16(rd, (uint16_t)(ptr >> 0), 0),
		movk64_immed_u16(rd, (uint16_t)(ptr >> 16), 1),
		movk64_immed_u16(rd, (uint16_t)(ptr >> 32), 2),
		movk64_immed_u16(rd, (uint16_t)(ptr >> 48), 3)
	);
}

/* Attempt to convert 32-bit number to bitmask encoding. Returns 0 on failure. */
uint32_t number_to_mask(uint32_t value)
{
	if (value == 0 || value == 0xFFFFFFFF)
		return 0;

	uint32_t pattern = value;
	int pattern_len = 32;

	for (int len = 16; len >= 2; len >>= 1) {
		uint32_t mask = (1U << len) - 1;
		uint32_t chunk = pattern & mask;
		uint32_t test_pattern = value;
		uint32_t stop = 0xffffffff;
		int is_repeating = 1;
		do {
			if ((test_pattern & mask) != chunk) {
				is_repeating = 0;
				break;
			}
			test_pattern >>= len;
			stop >>= len;
		} while (stop != 0);

		if (is_repeating) {
			pattern = chunk;
			pattern_len = len;
		} else {
			break;
		}
	}

	int ones_count = __builtin_popcount(pattern);
	if (ones_count == 0 || ones_count == pattern_len)
		return 0;

	uint32_t rotated = pattern;
	int rotation = 0;
	for (int r = 0; r < pattern_len; r++) {
		uint32_t temp = rotated;
		rotated = (rotated << 1) | (rotated >> (pattern_len - 1));
		rotated &= (1ULL << pattern_len) - 1;
		if (temp & 1 && !(temp & (1 << (pattern_len - 1)))) {
			int first_one = __builtin_ctz(temp);
			int last_one = 31 - __builtin_clz(temp);
			int width = 1 + last_one - first_one;
			rotation = r;
			if (width == ones_count)
				break;
			return 0;
		}
	}

	uint32_t imms_val = 0;
	switch (pattern_len) {
		case 32: imms_val = 0x00 | (ones_count); break;
		case 16: imms_val = 0x20 | (ones_count); break;
		case 8:  imms_val = 0x30 | (ones_count); break;
		case 4:  imms_val = 0x38 | (ones_count); break;
		case 2:  imms_val = 0x3C | (ones_count); break;
		default: return 0;
	}
	return (imms_val << 16) | rotation;
}

/*
 * Loads a 32-bit immediate into rd using a bitmask, a single mov, or mov+movk.
 *
 * Parameters:
 *   ctx   - Translator context.
 *   rd    - Destination W register.
 *   immed - Value to materialize.
 */
void EMIT_LoadImmediate(struct TranslatorContext *ctx, uint8_t rd, uint32_t immed)
{
	if (immed == 0) {
		EMIT(ctx, mov_reg(rd, WZR));
		return;
	}
	if (immed == 0xffffffff) {
		EMIT(ctx, mvn_reg(rd, WZR, LSL, 0));
		return;
	}

	uint32_t mask = number_to_mask(immed);
	if (mask) {
		EMIT(ctx, orr_immed(rd, 31, (mask >> 16) & 0x3f, mask & 0x3f));
		return;
	}

	if ((immed & 0xffff) == 0)
		EMIT(ctx, mov_immed_u16(rd, (immed >> 16) & 0xffff, 1));
	else if ((immed & 0xffff) == 0xffff)
		EMIT(ctx, movn_immed_u16(rd, (~immed >> 16) & 0xffff, 1));
	else if ((immed & 0xffff0000) == 0)
		EMIT(ctx, mov_immed_u16(rd, immed & 0xffff, 0));
	else if ((immed & 0xffff0000) == 0xffff0000)
		EMIT(ctx, movn_immed_u16(rd, ~immed & 0xffff, 0));
	else
		EMIT(ctx,
			mov_immed_u16(rd, immed & 0xffff, 0),
			movk_immed_u16(rd, (immed >> 16) & 0xffff, 1)
		);
}

/*
 * 10^exp for packed-BCD FPU conversion (LINEF DoubleToPacked).
 *
 * Parameters:
 *   exp - Decimal exponent in the range used by 80-bit packed BCD.
 *
 * Returns:
 *   10 raised to exp, via libc (upstream uses a 32-entry table).
 */
double my_pow10(int exp)
{
	return pow(10.0, (double)exp);
}

/*
 * Integer log10 for packed-BCD FPU conversion.
 *
 * Parameters:
 *   v - Positive magnitude of the converted double.
 *
 * Returns:
 *   Floor of log10(v), or 0 if v is not positive.
 */
int my_log10(double v)
{
	if (v <= 0.0)
		return 0;
	return (int)floor(log10(v));
}

/*
 * Formats into a stack buffer and feeds each character to putc_f. Used by
 * EMIT_InjectDebugStringV to embed a C string in the JIT stream.
 *
 * Parameters:
 *   putc_f    - Character sink (Translator writes into the code buffer).
 *   putc_data - Sink context.
 *   format    - printf-style format string.
 *   args      - Variadic arguments already started by the caller.
 */
void vkprintf_pc(putc_func putc_f, void *putc_data, const char *format, va_list args)
{
	char buf[512];
	int n = vsnprintf(buf, sizeof(buf), format, args);
	if (n < 0)
		return;
	if (n >= (int)sizeof(buf))
		n = (int)sizeof(buf) - 1;
	for (int i = 0; i < n; i++)
		putc_f(putc_data, buf[i]);
}
