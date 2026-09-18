/*
 *  sysdeps.h - System dependent definitions, shared by every supported host
 *
 *  Basilisk II (C) 1997-1999 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 *  Cockatrice III targets exactly six configurations -- {macOS, Windows,
 *  Linux} x {AMD64, ARM64} -- all 64-bit, all little-endian, and all able to
 *  perform unaligned accesses. That is narrow enough that one header covers
 *  every host; only the handful of truly per-host answers (which functions
 *  and headers exist, whether loff_t needs defining) live in the platform's
 *  own config.h, found ahead of this file on the include path.
 */

#ifndef SYSDEPS_H
#define SYSDEPS_H

#ifndef __STDC__
#error "Your compiler is not ANSI. Get a real one."
#endif

#include "config.h"
#include "user_strings_sdl.h"

#ifndef STDC_HEADERS
#error "You don't have ANSI C header files."
#endif

#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>

#ifdef _WIN32
/* Must precede any <windows.h>, or the two declare conflicting socket APIs. */
# include <winsock2.h>
#else
# include <netinet/in.h>
# include <pthread.h>
#endif

typedef uintptr_t uintptr;

/* Are the Mac and the host address space the same? */
#define REAL_ADDRESSING 0

/* Are we using a 68k emulator or the real thing? */
#define EMULATED_68K 1

/* Is the Mac ROM write protected? */
#define ROM_IS_WRITE_PROTECTED 1

/* ExtFS is supported */
#define SUPPORTS_EXTFS 1

/*
 * Data types.
 *
 * Spelled out with `long long` rather than derived from SIZEOF_LONG, because
 * that is the one width that genuinely differs across the matrix: Windows is
 * LLP64 (long is 4 bytes) while macOS and Linux are LP64 (long is 8). short,
 * int and long long are 2, 4 and 8 everywhere we build, so keying off them
 * gives one set of typedefs that is correct on all six configurations.
 */
typedef unsigned char uint8;
typedef signed char int8;
typedef unsigned short uint16;
typedef short int16;
typedef unsigned int uint32;
typedef int int32;
typedef unsigned long long uint64;
typedef long long int64;

#define VAL64(a) (a ## LL)
#define UVAL64(a) (a ## uLL)

/* Time data type for Time Manager emulation */
#ifdef HAVE_CLOCK_GETTIME
typedef struct timespec tm_time_t;
#else
typedef struct timeval tm_time_t;
#endif

/* Offset Mac->Unix time in seconds */
#define TIME_OFFSET 0x7c25b080

/* UAE CPU data types */
#define uae_s8 int8
#define uae_u8 uint8
#define uae_s16 int16
#define uae_u16 uint16
#define uae_s32 int32
#define uae_u32 uint32
#define uae_s64 int64
#define uae_u64 uint64
typedef uae_u32 uaecptr;

/* Alignment restrictions: true of both AMD64 and ARM64. */
#define CPU_CAN_ACCESS_UNALIGNED

/*
 * Fast byte swapping for little-endian hosts.
 *
 * A 68020+ allows unaligned word and long accesses, so the emulated machine
 * really does hand these odd addresses. Both halves of that have to be spelled
 * out for the compiler, or it is undefined behaviour even though AMD64 and
 * ARM64 permit the access:
 *
 *   - the parameter is void *, because merely *holding* a misaligned uae_u32 *
 *     is UB -- switching only the access to memcpy() left UBSan reporting the
 *     pointer itself;
 *   - the access is memcpy(), not a dereference.
 *
 * UBSan reported this on essentially every test in BasiliskII/tests, which
 * buried the findings the ROM-patch suites exist to surface.
 *
 * It costs nothing: every supported compiler lowers each of these to the same
 * single load/store plus byte-reverse it emitted for the direct dereference.
 */
static inline uae_u32 do_get_mem_long(void *a) { uae_u32 v; memcpy(&v, a, 4); return __builtin_bswap32(v); }
static inline uae_u32 do_get_mem_word(void *a) { uae_u16 v; memcpy(&v, a, 2); return __builtin_bswap16(v); }
static inline void do_put_mem_long(void *a, uae_u32 v) { v = __builtin_bswap32(v); memcpy(a, &v, 4); }
static inline void do_put_mem_word(void *a, uae_u32 v) { uae_u16 t = __builtin_bswap16((uae_u16)v); memcpy(a, &t, 2); }

#define do_get_mem_byte(a) ((uae_u32)*((uae_u8 *)(a)))
#define do_put_mem_byte(a, v) (*(uae_u8 *)(a) = (v))

#define call_mem_get_func(func, addr) ((*func)(addr))
#define call_mem_put_func(func, addr, v) ((*func)(addr, v))
#define __inline__ inline
#define CPU_EMU_SIZE 0
#undef NO_INLINE_MEMORY_ACCESS
#undef MD_HAVE_MEM_1_FUNCS
#define ENUMDECL typedef enum
#define ENUMNAME(name) name
#define write_log printf

#undef USE_MAPPED_MEMORY
#undef CAN_MAP_MEMORY

#define ASM_SYM_FOR_FUNC(a)

#ifndef REGPARAM
# define REGPARAM
#endif
#define REGPARAM2

#endif /* SYSDEPS_H */
