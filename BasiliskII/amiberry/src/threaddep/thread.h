/*
 *  thread.h - Hosted atomics for Amiberry CPU (no SDL3)
 *
 *  Replaces Amiberry's SDL3 thread wrapper in this trimmed CPU tree.
 */

#ifndef WINUAE_HOSTED_THREADDEP_THREAD_H
#define WINUAE_HOSTED_THREADDEP_THREAD_H

#include "uae/types.h"

static inline uae_atomic atomic_and(volatile uae_atomic *p, uae_u32 v)
{
	return __atomic_and_fetch(p, v, __ATOMIC_SEQ_CST);
}

static inline uae_atomic atomic_or(volatile uae_atomic *p, uae_u32 v)
{
	return __atomic_or_fetch(p, v, __ATOMIC_SEQ_CST);
}

static inline uae_atomic atomic_inc(volatile uae_atomic *p)
{
	return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
}

static inline uae_atomic atomic_dec(volatile uae_atomic *p)
{
	return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST);
}

static inline void atomic_set(volatile uae_atomic *p, uae_u32 v)
{
	__atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}

static inline uae_u32 atomic_read(volatile uae_atomic *p)
{
	return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}

#endif
