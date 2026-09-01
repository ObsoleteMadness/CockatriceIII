/*
 *  emu68_darwin_jit.h - Darwin / macOS Apple Silicon JIT Memory Manager
 *
 *  CockatriceIII Multi-Engine Architecture (C) 2026
 */

#ifndef EMU68_DARWIN_JIT_H
#define EMU68_DARWIN_JIT_H

#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef MAP_JIT
#define MAP_JIT 0x0800
#endif

#if defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
extern "C" {
    int  pthread_jit_write_protect_supported_np(void) __attribute__((weak_import));
    void pthread_jit_write_protect_np(int enabled) __attribute__((weak_import));
    void sys_icache_invalidate(void *start, size_t len);
}

static inline void jit_write_enable(void)
{
    if (pthread_jit_write_protect_np) {
        pthread_jit_write_protect_np(0); // 0 = Write enabled, Execute disabled
    }
}

static inline void jit_write_disable(void)
{
    if (pthread_jit_write_protect_np) {
        pthread_jit_write_protect_np(1); // 1 = Execute enabled, Write disabled
    }
}

static inline void jit_flush_icache(void *addr, size_t size)
{
    sys_icache_invalidate(addr, size);
}

#else

static inline void jit_write_enable(void) {}
static inline void jit_write_disable(void) {}
static inline void jit_flush_icache(void *addr, size_t size) {
    __builtin___clear_cache((char *)addr, (char *)addr + size);
}

#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Drops Darwin MAP_JIT write-protect so C helpers called from translated
 * code can mutate the TLSF JIT pool (tlsf_free, unit metadata). Pair with
 * emu68_hosted_jit_write_disable before returning to AArch64 JIT.
 */
void emu68_hosted_jit_write_enable(void);

/*
 * Restores Darwin MAP_JIT execute-only after a hosted helper has finished
 * writing the JIT pool. Must be called before the translated blr returns.
 */
void emu68_hosted_jit_write_disable(void);

/*
 * Runs a Basilisk II EmulOp (0x7101..MAX) or EXEC_RETURN (0x7100) from JIT.
 * Saves pinned 680x0 state, dispatches EmulOp()/TriggerExecutionReturn(), then
 * reloads pinned registers so the translated block can ret to the run loop.
 *
 * Parameters:
 *   opcode - Trap word at the current 680x0 PC (already advanced past by JIT).
 */
void emu68_hosted_emulop(uint32_t opcode);

/*
 * Dumps AArch64 instruction bytes around a crash PC, correlating with the last
 * translated JIT unit so the faulting instruction can be identified.
 *
 * Parameters:
 *   pc - Host program counter from the signal ucontext (faulting instruction).
 *   lr - Host link register from the signal ucontext.
 */
void emu68_jit_on_crash(uintptr_t pc, uintptr_t lr);

#ifdef __cplusplus
}
#endif

#endif /* EMU68_DARWIN_JIT_H */
