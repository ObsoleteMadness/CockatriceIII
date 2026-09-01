/*
 *  emu68_exception.c - Hosted 680x0 exception frame builder
 *
 *  Upstream M68k_ExceptionEntry.c treats A7 as a host pointer into
 *  identity-mapped big-endian RAM and writes the frame with STR. On this
 *  TARGET A7 is a Macintosh physical address: frames go through cache_write_*
 *  (ReadMacInt/WriteMacInt on the shared Host_Mem_Base window, big-endian) and
 *  the handler PC is cache_read_32 from VBR + vector.
 *
 *  The JIT calls a naked trampoline (M68K_Exception in emu68_glue.cpp) that
 *  SaveContext's pinned registers first, so this C function may use the
 *  host ABI freely.
 */

#include <stdint.h>

#include "cache.h"
#include "M68k.h"
#include "emu68_hosted.h"

extern struct M68KState *__m68k_state;

/*
 * Builds a 680x0 exception stack frame in guest memory and loads the vector.
 *
 * Parameters:
 *   sr              - SR image from SaveContext (already merged with NZCV).
 *   type_and_format - Vector offset in bits 11..0, format in bits 15..12.
 *   ea              - Extra address for format 2/3 frames.
 *   fault           - Fault address for format 4 frames.
 *
 * Returns:
 *   Updated SR with S set and T0/T1 cleared. PC and A7 are written to
 *   __m68k_state for the trampoline's LoadContext.
 */
uint32_t emu68_exception_c(uint32_t sr, uint32_t type_and_format, uint32_t ea, uint32_t fault)
{
	uint32_t a7 = __m68k_state->A[7].u32;
	uint32_t pc = __m68k_state->PC;
	uint32_t format = type_and_format >> 12;
	uint32_t vector = type_and_format & 0x0fffu;

	/* Switch to ISP/MSP if the fault happened in user mode. */
	if ((sr & SR_S) == 0) {
		__m68k_state->USP.u32 = a7;
		if (sr & SR_M)
			a7 = __m68k_state->MSP.u32;
		else
			a7 = __m68k_state->ISP.u32;
	}

	/* Format 2/3: push the effective address before the 8-byte header. */
	if (format == 2 || format == 3) {
		a7 -= 4;
		cache_write_32(DCACHE, a7, ea, 0);
	} else if (format == 4) {
		a7 -= 8;
		cache_write_32(DCACHE, a7, fault, 0);
		cache_write_32(DCACHE, a7 + 4, ea, 0);
	}

	/*
	 * 68020 format-0 header: SR.w, PC.l, format/vector.w (8 bytes).
	 * Upstream XORs CCR C/V when they are the only set bits so the
	 * stacked SR matches Emu68's NZCV overlay convention.
	 */
	uint32_t stacked_sr = sr;
	if ((stacked_sr & 3) != 0 && (stacked_sr & 3) < 3)
		stacked_sr ^= 3;

	a7 -= 8;
	cache_write_16(DCACHE, a7, (uint16_t)stacked_sr, 0);
	cache_write_32(DCACHE, a7 + 2, pc, 0);
	cache_write_16(DCACHE, a7 + 6, (uint16_t)type_and_format, 0);

	sr |= SR_S;
	sr &= ~(SR_T0 | SR_T1);

	__m68k_state->A[7].u32 = a7;
	if (sr & SR_M)
		__m68k_state->MSP.u32 = a7;
	else
		__m68k_state->ISP.u32 = a7;

	/* Vector table is a table of Macintosh longs at VBR. */
	__m68k_state->PC = cache_read_32(ICACHE, __m68k_state->VBR + vector);
	__m68k_state->SR = (uint16_t)sr;
	return sr;
}
