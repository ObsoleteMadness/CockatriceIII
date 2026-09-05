/*
 *  jit_host_darwin.cpp - macOS implementation of the shared JIT-host support
 *
 *  Apple Silicon: MAP_JIT pages are never writable and executable at once
 *  for a given thread. Toggle with pthread_jit_write_protect_np() — do not
 *  mprotect the region, and do not clear the APRR mask (libfixjit gist).
 *
 *  CockatriceIII (C) 2026
 */

#include "jit_host.h"

#include <libkern/OSCacheControl.h>

#if defined(__aarch64__) || defined(_M_ARM64)

#include <pthread.h>
#include <stdio.h>

/* pthread_jit_write_protect_np() toggles the W^X state per-thread, so the
 * nesting counter that gates it must be per-thread too: two threads can
 * each be mid-write on their own JIT mapping at the same time (e.g. a
 * hosted CPU plugin driving its own JIT while the main thread is inside
 * the block compiler), and a shared global counter would let their
 * begin/end pairs interleave and close one thread's window early. */
static thread_local int s_write_window_depth = 0;

/*
 * Returns whether this thread's MAP_JIT region uses APRR write-protect.
 *
 * Arguments: none.
 *
 * Returns:
 *   True on Apple Silicon (must toggle before store vs execute).
 */
static int jit_wx_supported(void)
{
	return pthread_jit_write_protect_supported_np();
}

void jit_host_begin_write(void)
{
	s_write_window_depth++;
	/* First opener: allow stores, deny execute on this thread. */
	if (s_write_window_depth == 1 && jit_wx_supported())
		pthread_jit_write_protect_np(0);
}

void jit_host_end_write(void)
{
	if (s_write_window_depth <= 0) {
		fprintf(stderr, "jit_host: write window underflow\n");
		s_write_window_depth = 0;
		return;
	}
	s_write_window_depth--;
	/* Last closer: deny stores, allow execute — the required dispatch state. */
	if (s_write_window_depth == 0 && jit_wx_supported())
		pthread_jit_write_protect_np(1);
}

void jit_host_ensure_execute(void)
{
	/* A live write window still owns the toggle; leave it alone. */
	if (s_write_window_depth != 0)
		return;
	if (jit_wx_supported())
		pthread_jit_write_protect_np(1);
}

#else /* Intel Mac: MAP_JIT APRR is not supported; RWX uses the unsigned-exec entitlement. */

void jit_host_begin_write(void) {}
void jit_host_end_write(void) {}
void jit_host_ensure_execute(void) {}

#endif

void jit_host_flush_icache(void *start, void *stop)
{
	sys_icache_invalidate(start, (char *)stop - (char *)start);
}
