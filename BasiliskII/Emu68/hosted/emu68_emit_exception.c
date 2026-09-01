/*
 *  emu68_emit_exception.c - JIT sequence that raises a 680x0 exception
 *
 *  Upstream M68k_Exception.c pushes an 8-byte ARM frame and sets the dual-map
 *  JIT high bit (orr64 0x10_0000_0000) before blr to M68K_Exception. Darwin
 *  requires 16-byte SP and has no dual map. The C handler is emu68_exception_c
 *  (guest A7 via cache_write_*), entered through the naked trampoline.
 */

#include <stdint.h>
#include <stdarg.h>

#include "support.h"
#include "M68k.h"
#include "RegisterAllocator.h"
#include "emu68_hosted.h"

extern void M68K_Exception(void);

void EMIT_Exception(struct TranslatorContext *ctx, uint16_t exception, uint8_t format, ...)
{
	va_list args;
	uint8_t sr = RA_ModifyCC(ctx);
	uint32_t ea = 0;
	uint32_t fault = 0;

	va_start(args, format);
	EMIT(ctx, mov_immed_u16(1, (uint16_t)((format << 12) | (exception & 0x0fff)), 0));

	if (format == 0)
		EMIT(ctx, str64_offset_preindex(31, 30, -16));

	if (format == 2 || format == 3) {
		ea = va_arg(args, uint32_t);
		EMIT_LoadImmediate(ctx, 0, ea);
		EMIT(ctx, stp64_preindex(31, 0, 30, -16));
	} else if (format == 4) {
		fault = va_arg(args, uint32_t);
		ea = va_arg(args, uint32_t);
		EMIT_LoadImmediate(ctx, 0, fault);
		EMIT(ctx, sub64_immed(31, 31, 32));
		EMIT(ctx, str64_offset(31, 0, 0));
		EMIT_LoadImmediate(ctx, 2, ea);
		EMIT(ctx, str64_offset(31, 2, 8));
		EMIT(ctx, str64_offset(31, 30, 16));
	}

	EMIT(ctx, mov_reg(0, sr));
	EMIT_LoadHostPointer(ctx, 2, (uintptr_t)(void *)M68K_Exception);
	EMIT(ctx, blr(2), mov_reg(sr, 0));

	if (format == 2 || format == 3) {
		EMIT(ctx, ldr64_offset(31, 30, 8), add64_immed(31, 31, 16));
	} else if (format == 4) {
		EMIT(ctx, ldr64_offset(31, 30, 16), add64_immed(31, 31, 32));
	} else {
		EMIT(ctx, ldr64_offset_postindex(31, 30, 16));
	}

	va_end(args);
}
