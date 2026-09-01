/*
 *  A64.h - Hosted TARGET wrap of upstream A64 emitters
 *
 *  Darwin/Win32 EL0 cannot execute EL1 encodings. Rename the bare-metal
 *  emitters out of the way, then provide userspace stand-ins. Instruction
 *  fetch still goes through I32()/INSN_TO_LE() from upstream.
 *
 *  Uses a path include (not include_next) so this also works when forced
 *  via -include: include_next from a -include file restarts the search and
 *  would hit this wrap again instead of upstream.
 */

#ifndef EMU68_HOSTED_A64_H
#define EMU68_HOSTED_A64_H

#define wfi wfi_el1
#define wfe wfe_el1
#define svc svc_el1
#define msr_imm msr_imm_el1
#if defined(__APPLE__)
#define brk brk_el1
#endif

#ifdef __cplusplus
#include <stdint.h>
#include <stddef.h>
#define constexpr static inline
#endif

#include "../upstream/include/A64.h"

#ifdef __cplusplus
#undef constexpr
#undef __constexpr
#define __constexpr static inline
#endif

#undef wfi
#undef wfe
#undef svc
#undef msr_imm
#if defined(__APPLE__)
#undef brk
#endif

#ifndef __constexpr
#define __constexpr static inline
#endif

/*
 * WFI/WFE are EL1 wait hints. Emit nop so leftover call sites do not SIGILL.
 * STOP still ends the translation unit from LINE4's INSN_TO_LE(0xffffffff)
 * path on upstream; the run loop polls INTF between units.
 */
__constexpr uint32_t wfi(void) { return nop(); }
__constexpr uint32_t wfe(void) { return nop(); }

/*
 * DAIF MSR immediates mask ARM IRQs from 680x0 IPL. Hosted IRQs are Basilisk
 * VIA/SCC flags polled in the run loop, so these are nops.
 */
__constexpr uint32_t msr_imm(uint8_t op1, uint8_t op2, uint8_t imm)
{
	(void)op1;
	(void)op2;
	(void)imm;
	return nop();
}

/*
 * Bare-metal SVC dumps unimplemented opcodes. Darwin SVC is a real syscall
 * (SIGSYS). Skip the four-word payload used at every current call site:
 * svc(0x100), svc(0x101), svc(0x103), <ptr>, 48.
 */
__constexpr uint32_t svc(uint16_t code)
{
	if (code == 0x100)
		return b(5);
	return nop();
}

#if defined(__APPLE__)
__constexpr uint32_t emu68_a64_brk(uint16_t imm16) { return brk_el1(imm16); }
#define brk emu68_a64_brk
#endif

/*
 * UMOV Xd, Vn.D[index] and INS Vd.D[index], Xn. Upstream A64.h has no helpers
 * for these; LoadContext uses them for HOST_MEM_BASE in v22.d[0].
 */
__constexpr uint32_t umov64_d(uint8_t xd, uint8_t vn, uint8_t index)
{
	uint32_t imm5 = ((uint32_t)(index & 1) << 4) | 8;
	return I32(0x4E003C00 | (imm5 << 16) | ((vn & 31) << 5) | (xd & 31));
}

__constexpr uint32_t ins64_d(uint8_t vd, uint8_t index, uint8_t xn)
{
	/* INS Vd.D[i], Xn: Rd=Vd, Rn=Xn. umov64_d uses the opposite field order. */
	uint32_t imm5 = ((uint32_t)(index & 1) << 4) | 8;
	return I32(0x4E001C00 | (imm5 << 16) | ((xn & 31) << 5) | (vd & 31));
}

struct TranslatorContext;
int emu68_hosted_rewrite_mem(struct TranslatorContext *ctx, uint32_t insn);

/*
 * Guest 8/16/32-bit loads/stores go through HOST_MEM_BASE. One original
 * word can expand to about ten; keep the overflow check on the expanded size.
 */
#undef EMIT
#define EMIT(ctx, ...)                                                                \
    ({ do                                                                             \
    {                                                                                 \
        const uint32_t __emit_args__[] = {__VA_ARGS__};                               \
        const uint32_t count = sizeof(__emit_args__) / sizeof(__emit_args__[0]);      \
        if ((ctx)->tc_CodePtr + count * 10 > (ctx)->tc_CodeEnd) {                     \
            kprintf("[EMIT] Need more space for code - %d instructions!", ((ctx)->tc_CodePtr + count - (ctx)->tc_CodeEnd)); \
        }                                                                             \
        for (size_t i = 0; i < count; i++)                                            \
        {                                                                             \
            if (!emu68_hosted_rewrite_mem((ctx), __emit_args__[i]))                   \
                *((ctx)->tc_CodePtr)++ = __emit_args__[i];                            \
        }                                                                             \
    } while (0); (ctx)->tc_CodePtr; })

#endif /* EMU68_HOSTED_A64_H */
