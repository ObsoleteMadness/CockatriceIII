/*
 *  emu68_hosted.c - Hosted TARGET runtime helpers (hang, EL1 skip classify)
 *
 *  Cockatrice III platform adapter. Not part of upstream Emu68.
 */

#include "emu68_hosted.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Aborts instead of emitting WFI when the translator overruns its code
 * buffer. A WFI loop is an EL1 wait; on Darwin it SIGILLs or hangs the
 * process with no way for Basilisk to recover.
 */
void emu68_hosted_hang(void)
{
	fprintf(stderr, "[Emu68 hosted] fatal translator condition; aborting (no WFI on userspace TARGET)\n");
	fflush(stderr);
	abort();
}

/*
 * Classifies leftover privileged AArch64 encodings that a SIGILL handler may
 * skip. Matches WFI, WFE, and MSR/MRS DAIF immediates that bare-metal Emu68
 * emits around IPL changes.
 *
 * Parameters:
 *   pc   - Unused except for API symmetry with the crash dump.
 *   insn - Little-endian AArch64 instruction word.
 *
 * Returns:
 *   1 if skipping is safe (the op is a no-op on this TARGET), else 0.
 */
int emu68_hosted_is_skippable_el1(uintptr_t pc, uint32_t insn)
{
	(void)pc;
	/* HINT encodings: WFE = hint #2, WFI = hint #3, SEV/SEVL nearby. */
	if (insn == 0xD503205Fu || insn == 0xD503207Fu)
		return 1;
	/* MSR DAIFSet/DAIFClr: op0=0, CRn=2, Rt=31, op1=3, CRm in {6,7}. */
	if ((insn & 0xFFFFFFE0u) == 0xD50340E0u)
		return 1;
	if ((insn & 0xFFFFF01Fu) == 0xD503401Fu && ((insn >> 8) & 0xF) >= 6)
		return 1;
	return 0;
}
