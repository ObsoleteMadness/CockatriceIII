/*
 *  jit_host_win32.cpp - Windows implementation of the shared JIT-host support
 *
 *  Not currently wired into any Windows build in this tree (mingw/Makefile
 *  builds no JIT sources today) and unbuilt/untested in this environment.
 *  Provided so a future Windows JIT build has this half of the platform
 *  split ready to compile against, matching jit_host_darwin.cpp.
 *
 *  CockatriceIII (C) 2026
 */

#include "jit_host.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* Windows doesn't enforce W^X on this codebase's JIT cache: it's allocated
 * read/write/execute up front via VirtualAlloc (see uae_vm_alloc), so there
 * is no write-protect state to toggle here. Kept as a no-op pair rather than
 * special-cased away in callers, so a future change that does need one (e.g.
 * CFG / Hardware-enforced Stack Protection) has a real hook to fill in. */
void jit_host_begin_write(void) {}
void jit_host_end_write(void) {}

void jit_host_flush_icache(void *start, void *stop)
{
	FlushInstructionCache(GetCurrentProcess(), start, (SIZE_T)((char *)stop - (char *)start));
}
