#ifndef _REGLOCK_H
#define _REGLOCK_H

/*
 * Pin Emu68's SIMD context lanes (SR, insn count, CACR image) so GCC cannot
 * allocate them as scratch. Clang and Darwin do not support global SIMD
 * register asm variables; the JIT pinning is done in M68K_LoadContext instead.
 */
#if !defined(__APPLE__) && defined(__GNUC__) && !defined(__clang__)
register __uint128_t reserved_reg_q19 asm("q19");
register __uint128_t reserved_reg_q20 asm("q20");
register __uint128_t reserved_reg_q21 asm("q21");
#endif

#endif /* _REGLOCK_H */
