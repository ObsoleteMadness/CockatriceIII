/*
 *  jit_host.h - Shared JIT-host support: W^X toggling and icache coherency
 *
 *  One implementation per host platform (jit_host_darwin.cpp,
 *  jit_host_win32.cpp). Every JIT backend that emits and executes code at
 *  runtime (UAE's compemu ARM64 backend, m68k-rs's optional Cranelift trace
 *  JIT) calls into this instead of each keeping its own copy of these
 *  platform primitives.
 *
 *  CockatriceIII (C) 2026
 */

#ifndef COCKATRICE_JIT_HOST_H
#define COCKATRICE_JIT_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

/* Enter/leave a window in which the calling thread may write to JIT-cache
 * pages that are normally execute-only (e.g. Apple Silicon MAP_JIT / W^X).
 * Nestable and thread-local: each thread tracks its own depth, since two
 * threads can each be mid-way through emitting or patching code on their
 * own mappings at the same time, and a shared counter would let one
 * thread's jit_host_end_write() close the window out from under the other.
 * A no-op pair on platforms that don't enforce W^X on JIT memory. */
void jit_host_begin_write(void);
void jit_host_end_write(void);

/* Makes code written to [start, stop) visible to instruction fetch. Call
 * after emitting or patching executable JIT code, while still inside the
 * write window (i.e. before the matching jit_host_end_write()). */
void jit_host_flush_icache(void *start, void *stop);

#ifdef __cplusplus
}
#endif

#endif /* COCKATRICE_JIT_HOST_H */
