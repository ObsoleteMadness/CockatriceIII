/*
 *  emu68_hosted.h - Cockatrice hosted TARGET for Emu68
 *
 *  A macOS/Win32 ARM process occupies the same slot as an upstream CMake
 *  TARGET (raspi64, virt). It is not an ExpansionBoard and not a dtbo overlay.
 *  Do not enable RASPI, PISTORM*, src/boards, or src/overlays; those are EL1
 *  Pi/Amiga firmware. virt still identity-maps guest RAM from EL1.
 *
 *  Endianness is config.h EMU68_HOST_BIG_ENDIAN (0 on this TARGET). Darwin's
 *  4GB PAGEZERO means Macintosh addresses cannot be identity-mapped host
 *  pointers; emu68_host_mem_base aliases the shared Host_Mem_Base window.
 *
 *  Basilisk EmulOp 0x7100..M68K_EMUL_OP_MAX is intercepted in the run loop
 *  before translation (host-call, not MOVEQ, not a 680x0 illegal vector).
 *  680x0 exceptions write frames through cache_write_* into Mac memory.
 */

#ifndef EMU68_HOSTED_H
#define EMU68_HOSTED_H

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

struct TranslatorContext;

extern uint8_t *emu68_host_mem_base;

#define HOST_MEM_BASE_ASM "v22.d[0]"

void emu68_hosted_hang(void) __attribute__((noreturn));
void emu68_hosted_jit_write_enable(void);
void emu68_hosted_jit_write_disable(void);
int emu68_hosted_try_skip_el1(uintptr_t pc, uint32_t insn);
int emu68_hosted_is_skippable_el1(uintptr_t pc, uint32_t insn);

uint32_t emu68_exception_c(uint32_t sr, uint32_t type_and_format, uint32_t ea, uint32_t fault);
void emu68_emulop_dispatch(uint32_t opcode);
void emu68_hosted_emulop(uint32_t opcode);

void EMIT_LoadHostPointer(struct TranslatorContext *ctx, uint8_t rd, uintptr_t ptr);
void emu68_hosted_emulop(uint32_t opcode);

/*
 * If insn is an 8/16/32-bit integer load/store of a guest address, emits
 * HOST_MEM_BASE + UXTW(Rn) plus endian fixups and returns 1. Returns 0 when
 * the caller should write insn unchanged (64-bit, ARM SP, SIMD, ALU).
 */
int emu68_hosted_rewrite_mem(struct TranslatorContext *ctx, uint32_t insn);

#ifdef __cplusplus
}
#endif

#endif /* EMU68_HOSTED_H */
