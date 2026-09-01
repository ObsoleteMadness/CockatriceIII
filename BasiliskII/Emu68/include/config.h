/*
    Copyright © 2019 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#ifndef _CONFIG_H
#define _CONFIG_H

/*
 * Hosted TARGET (Cockatrice Darwin/Win32), not an upstream CMake board.
 *
 * Upstream CMake TARGET is one of raspi64 / pbpro / rockpro64 / virt, all of
 * them EL1 firmware: -mbig-endian, -fno-pic, identity-mapped 68k addresses,
 * dual-map JIT bit 0x10_0000_0000, and start.c + MMU. VARIANT pistorm* and
 * src/boards (z2ram, emu68rom) are Amiga Zorro/PiStorm. src/overlays/*.dts
 * are Raspberry Pi dtbos (emu68.dtbo JIT size, fast-page-zero Kickstart
 * overlay at guest 0, unicam/emmc/sdhc/z2ram). None of those run in a
 * userspace process.
 *
 * virt is the closest analog (QEMU virt, VARIANT=none) but still maps 256MB
 * at VA 0 from EL1. Darwin PAGEZERO is 4GB, so this TARGET keeps 68k RAM in
 * the shared Host_Mem_Base window (aliased as emu68_host_mem_base) and never
 * enables RASPI / PISTORM* / overlays / boards.
 *
 * EMU68_HOSTED_TARGET replaces those board defines for hosted-only paths.
 */
#define EMU68_HOSTED_TARGET     1

#define CACHE_SET_COUNT         128
#define CACHE_WAY_COUNT         8

#define ARM_FEATURE_HAS_DIV     1
#define ARM_FEATURE_HAS_BITFLD  1
#define ARM_FEATURE_HAS_BITCNT  1
#define ARM_FEATURE_HAS_SWP     1
#define ARM_FEATURE_HAS_VDIV    1
#define ARM_FEATURE_HAS_SQRT    1

/*
 * Bare metal fills Features from start.c / dtbo. This TARGET does not link
 * start.c, so keep the compile-time Apple Silicon feature set from the header.
 */
#ifndef SET_FEATURES_AT_RUNTIME
#define SET_FEATURES_AT_RUNTIME 0
#endif

#ifndef SET_OPTIONS_AT_RUNTIME
#define SET_OPTIONS_AT_RUNTIME  0
#endif

/* Set 1 to propagate SO bit from XER to CRn */
#define PPC_SO_PROPAGATION      0

#define EMU68_USE_LRU           1
#define EMU68_LRU_WAY_COUNT     4
#define EMU68_LRU_SET_COUNT     128

#define EMU68_ARM_CACHE_SIZE    (4*1024*1024)
#define EMU68_M68K_INSN_DEPTH   256
/* 0 = little-endian host (Darwin/Win32). Upstream start.c uses this for SCTLR EE. */
#define EMU68_HOST_BIG_ENDIAN   0
#define EMU68_HAS_SETEND        1
#define EMU68_DEF_BRANCH_TAKEN  0
#define EMU68_DEF_BRANCH_AUTO   1
#define EMU68_DEF_BRANCH_AUTO_RANGE 128
#define EMU68_INSN_COUNTER      1
#define EMU68_MAX_LOOP_COUNT    8
#define EMU68_BRANCH_INLINE_DISTANCE 8191
#define EMU68_USE_RETURN_STACK  1
#define EMU68_WEAK_CFLUSH       1
#define EMU68_WEAK_CFLUSH_LIMIT 500

#define EMU68_WEAK_CFLUSH_SLOW  0
#define EMU68_PC_REG_HISTORY    0
#define EMU68_CCR_SCAN_DEPTH    20
#define EMU68_CCR_BREAK_AT_UNIT_END 1

#define EMU68_HASHSIZE          65536
#define EMU68_HASHMASK          (EMU68_HASHSIZE - 1)
#define EMU68_HASHSHIFT         2

#ifdef PISTORM_ANY_MODEL

/* Speed for bitbang RS232... */
#define PISTORM_BITBANG_SPEED   921600

#endif

#ifndef VERSION_STRING_DATE
#define VERSION_STRING_DATE ""
#endif

#define KERNEL_SYS_PAGES        16
#define KERNEL_JIT_PAGES        32
#define KERNEL_RSRVD_PAGES      (KERNEL_SYS_PAGES)

#define EMU68_LOG_FETCHES       0
#define EMU68_LOG_USES          0

#endif /* _CONFIG_H */
