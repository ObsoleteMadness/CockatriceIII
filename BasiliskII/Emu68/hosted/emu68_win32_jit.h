/*
 *  emu68_win32_jit.h - Win32 ARM JIT W^X (hosted TARGET)
 *
 *  Glue implements these with VirtualProtect / FlushInstructionCache.
 *  Darwin uses emu68_darwin_jit.h instead. Same contract: translator never
 *  calls OS protection APIs itself.
 */

#ifndef EMU68_WIN32_JIT_H
#define EMU68_WIN32_JIT_H

#if defined(_WIN32)

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void jit_write_enable(void);
void jit_write_disable(void);
void jit_flush_icache(void *addr, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */

#endif /* EMU68_WIN32_JIT_H */
