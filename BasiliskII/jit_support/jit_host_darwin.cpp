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

/* Ranges written inside this thread's open write window, flushed once when
 * the window closes. sys_icache_invalidate is expensive on Apple Silicon and
 * the UAE backend calls it for every 4-byte branch patch, many of them inside
 * compile_block's window: profiled, that was 60% of emulation-thread time.
 * Deferring is safe because W^X denies this thread execute on MAP_JIT pages
 * until the window closes, so nothing can fetch the stale instructions. */
enum { PENDING_RANGES = 32, PENDING_MERGE_GAP = 4096 };
struct pending_range { char *lo, *hi; };
static thread_local pending_range s_pending[PENDING_RANGES];
static thread_local int s_pending_count = 0;

static void flush_pending(void)
{
	for (int i = 0; i < s_pending_count; i++)
		sys_icache_invalidate(s_pending[i].lo, s_pending[i].hi - s_pending[i].lo);
	s_pending_count = 0;
}

/* Adds [lo, hi) to the pending list, merging with a nearby range. */
static void add_pending(char *lo, char *hi)
{
	for (int i = 0; i < s_pending_count; i++) {
		pending_range *r = &s_pending[i];
		if (lo <= r->hi + PENDING_MERGE_GAP && hi + PENDING_MERGE_GAP >= r->lo) {
			if (lo < r->lo)
				r->lo = lo;
			if (hi > r->hi)
				r->hi = hi;
			return;
		}
	}
	if (s_pending_count == PENDING_RANGES)
		flush_pending();
	s_pending[s_pending_count].lo = lo;
	s_pending[s_pending_count].hi = hi;
	s_pending_count++;
}

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
	if (s_write_window_depth != 0)
		return;
	flush_pending();
	/* Last closer: deny stores, allow execute — the required dispatch state. */
	if (jit_wx_supported())
		pthread_jit_write_protect_np(1);
}

void jit_host_flush_icache(void *start, void *stop)
{
	if (s_write_window_depth > 0)
		add_pending((char *)start, (char *)stop);
	else
		sys_icache_invalidate(start, (char *)stop - (char *)start);
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

void jit_host_flush_icache(void *start, void *stop)
{
	sys_icache_invalidate(start, (char *)stop - (char *)start);
}

#endif
