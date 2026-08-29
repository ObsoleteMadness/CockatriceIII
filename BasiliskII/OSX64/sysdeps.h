/*
 *  sysdeps.h - System dependent definitions for macOS ARM64
 *
 *  Basilisk II (C) 1997-1999 Christian Bauer
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
#include <netinet/in.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>

typedef uintptr_t uintptr;

/* Are the Mac and the host address space the same? */
#define REAL_ADDRESSING 0

/* Are we using a 68k emulator or the real thing? */
#define EMULATED_68K 1

/* Is the Mac ROM write protected? */
#define ROM_IS_WRITE_PROTECTED 1

/* ExtFS is supported */
#define SUPPORTS_EXTFS 1

/* Data types */
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

/* Alignment restrictions */
#define CPU_CAN_ACCESS_UNALIGNED

/* Fast byte swapping for little-endian ARM64 / modern compilers */
static inline uae_u32 do_get_mem_long(uae_u32 *a) { return __builtin_bswap32(*a); }
static inline uae_u32 do_get_mem_word(uae_u16 *a) { return __builtin_bswap16(*a); }
static inline void do_put_mem_long(uae_u32 *a, uae_u32 v) { *a = __builtin_bswap32(v); }
static inline void do_put_mem_word(uae_u16 *a, uae_u32 v) { *a = __builtin_bswap16(v); }

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
