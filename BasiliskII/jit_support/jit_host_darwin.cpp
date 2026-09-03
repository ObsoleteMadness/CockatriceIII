/*
 *  jit_host_darwin.cpp - macOS implementation of the shared JIT-host support
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

void jit_host_begin_write(void)
{
	s_write_window_depth++;
	if (s_write_window_depth == 1) {
		pthread_jit_write_protect_np(0);
	}
}

void jit_host_end_write(void)
{
	if (s_write_window_depth <= 0) {
		fprintf(stderr, "jit_host: write window underflow\n");
		s_write_window_depth = 0;
		return;
	}
	s_write_window_depth--;
	if (s_write_window_depth == 0) {
		pthread_jit_write_protect_np(1);
	}
}

#else /* Intel Mac: no W^X enforcement on JIT memory */

void jit_host_begin_write(void) {}
void jit_host_end_write(void) {}

#endif

void jit_host_flush_icache(void *start, void *stop)
{
	sys_icache_invalidate(start, (char *)stop - (char *)start);
}
