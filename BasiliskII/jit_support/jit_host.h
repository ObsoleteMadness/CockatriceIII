/*
 *  jit_host.h - Shared JIT-host support: W^X toggling and icache coherency
 *
 *  One implementation per host platform (jit_host_darwin.cpp,
 *  jit_host_win32.cpp). Every JIT backend that emits and executes code at
 *  runtime (UAE's compemu ARM64 backend, m68k-rs's optional Cranelift trace
 *  JIT) calls into this instead of each keeping its own copy of these
 *  platform primitives.
 *
 *  Apple Silicon (see Apple "Porting just-in-time compilers to Apple silicon"
 *  and com.apple.security.cs.allow-jit):
 *    1. mmap one MAP_JIT region (Hardened Runtime allows only one).
 *    2. Sign with com.apple.security.cs.allow-jit.
 *    3. pthread_jit_write_protect_np(0) to write, (1) to execute.
 *    4. sys_icache_invalidate after stores, before execute.
 *  Do not disable the APRR latch (the saagarjha libfixjit gist). That
 *  workaround exists for apps that never call pthread_jit_write_protect_np.
 *
 *  CockatriceIII (C) 2026
 */

#ifndef COCKATRICE_JIT_HOST_H
#define COCKATRICE_JIT_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Enters a window in which the calling thread may write MAP_JIT pages.
 *
 * Nestable and thread-local: each thread tracks its own depth, since two
 * threads can each be mid-way through emitting or patching code on their
 * own mappings at the same time, and a shared counter would let one
 * thread's jit_host_end_write() close the window out from under the other.
 * A no-op pair on platforms that don't enforce W^X on JIT memory.
 */
void jit_host_begin_write(void);

/*
 * Leaves the matching write window. The last closer on this thread
 * re-enables MAP_JIT execute (write-protect on).
 */
void jit_host_end_write(void);

/*
 * Re-asserts MAP_JIT execute mode if no write window is open.
 *
 * Use before dispatching into translated code when another compiler on
 * this thread may have left write-protect off. Does nothing while a
 * write window is still held, so it cannot close someone else's emit.
 */
void jit_host_ensure_execute(void);

/*
 * Makes code written to [start, stop) visible to instruction fetch. Call
 * after emitting or patching executable JIT code, while still inside the
 * write window (i.e. before the matching jit_host_end_write()).
 */
void jit_host_flush_icache(void *start, void *stop);

#ifdef __cplusplus
}
#endif

#endif /* COCKATRICE_JIT_HOST_H */
