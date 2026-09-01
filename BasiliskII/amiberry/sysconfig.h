/*
 *  sysconfig.h - Hosted Amiberry 680x0 CPU feature flags for Cockatrice III
 *
 *  Enables the CPUEMU tables and MMU accessors that newcpu.cpp type-checks.
 *  Runtime still uses generic interpreter or JIT; Amiga chipset, debugger,
 *  and savestate stay off.
 */

#ifndef AMIBERRY_SYSCONFIG_H
#define AMIBERRY_SYSCONFIG_H

#include <limits.h>

#ifndef MAX_DPATH
#ifdef PATH_MAX
#define MAX_DPATH PATH_MAX
#else
#define MAX_DPATH 1024
#endif
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#ifndef UAE
#define UAE 1
#endif

#ifndef AMIBERRY
#define AMIBERRY 1
#endif

#define AMIBERRY_VERSION_MAJOR 8
#define AMIBERRY_VERSION_MINOR 3
#define AMIBERRY_VERSION_PATCH 0

#ifdef __APPLE__
#ifndef AMIBERRY_MACOS
#define AMIBERRY_MACOS
#endif
#endif

#define CPUEMU_0 1
#define CPUEMU_11 1
#define CPUEMU_13 1
#define CPUEMU_20 1
#define CPUEMU_21 1
#define CPUEMU_22 1
#define CPUEMU_23 1
#define CPUEMU_24 1
#define CPUEMU_31 1
#define CPUEMU_32 1
#define CPUEMU_33 1
#define CPUEMU_34 1
#define CPUEMU_35 1
#define CPUEMU_40 1
#define CPUEMU_50 1

#define FPUEMU 1
#define FPU_UAE 1
#define MMUEMU 1
#define FULLMMU 1
#define JIT 1
#define USE_JIT_FPU 1
#define WITH_SOFTFLOAT 1
#define NOFLAGS_SUPPORT_GENCOMP 1
#define NATMEM_OFFSET natmem_offset
/* Basilisk Musashi no longer exports mem_banks; Amiberry keeps its own table. */
#define dummy_bank amiberry_dummy_bank
#define mem_banks amiberry_mem_banks

#ifndef HAVE_SYS_TYPES_H
#define HAVE_SYS_TYPES_H 1
#endif

typedef unsigned int uae_atomic;

#ifndef MAX_LINEWIDTH
#define MAX_LINEWIDTH 10000
#endif

#ifndef uaestrlen
#define uaestrlen strlen
#endif
#ifndef uaetcslen
#define uaetcslen strlen
#endif

#ifndef _tcsdup
#define _tcsdup strdup
#endif
#ifndef _tcslen
#define _tcslen strlen
#endif
#ifndef _tcscpy
#define _tcscpy strcpy
#endif
#ifndef _tcscmp
#define _tcscmp strcmp
#endif
#ifndef _tcsicmp
#define _tcsicmp strcasecmp
#endif
#ifndef _tcsncmp
#define _tcsncmp strncmp
#endif
#ifndef _istspace
#define _istspace isspace
#endif
#ifndef _istxdigit
#define _istxdigit isxdigit
#endif
#ifndef _stprintf
#define _stprintf sprintf
#endif
#ifndef _sntprintf
#define _sntprintf snprintf
#endif
#ifndef _vsntprintf
#define _vsntprintf vsnprintf
#endif
#ifndef _tfopen
#define _tfopen fopen
#endif
#ifndef _tremove
#define _tremove remove
#endif

#endif /* AMIBERRY_SYSCONFIG_H */
