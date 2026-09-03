/*
 *  cpu_emulation.h - Definitions for Basilisk II CPU emulation module (Musashi version)
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *  CockatriceIII Musashi Core Migration (C) 2026
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
 */

#ifndef CPU_EMULATION_H
#define CPU_EMULATION_H

#include <string.h>
#include <setjmp.h>
#include "sysdeps.h"

/*
 * Guest-fault checkpoints. sigsetjmp must run in the CPU loop itself;
 * a helper that returned would invalidate the jmp_buf.
 *
 *   memory_fault_jmp_buf *jb = memory_guard_enter();
 *   while (!quit) {
 *     if (MEMORY_FAULT_SETJMP(*jb) != 0) {
 *       // inject 680x0 vector 2 using memory_guest_fault_addr()
 *       continue;
 *     }
 *     run_slice();
 *   }
 *   memory_guard_leave();
 */
#ifdef _WIN32
typedef jmp_buf memory_fault_jmp_buf;
#define MEMORY_FAULT_SETJMP(buf) setjmp(buf)
#else
typedef sigjmp_buf memory_fault_jmp_buf;
#define MEMORY_FAULT_SETJMP(buf) sigsetjmp((buf), 1)
#endif

enum {
	MEMORY_PROT_READ = 1,
	MEMORY_PROT_WRITE = 2,
	MEMORY_PROT_EXEC = 4
};

/*
 *  Memory system
 */

// Global 4GB Flat Host Memory Window Base Pointer
extern uint8 *Host_Mem_Base;
extern bool TwentyFourBitAddressing;

// RAM and ROM pointers (allocated and set by main_*.cpp)
extern uint32 RAMBaseMac;		// RAM base (Mac address space), does not include Low Mem when != 0
extern uint8 *RAMBaseHost;		// RAM base (host address space)
extern uint32 RAMSize;			// Size of RAM

extern uint32 ROMBaseMac;		// ROM base (Mac address space)
extern uint8 *ROMBaseHost;		// ROM base (host address space)
extern uint32 ROMSize;			// Size of ROM

const uint32 MacFrameBaseMac = 0xa0000000;
extern uint8 *MacFrameBaseHost;	// Frame buffer base (host address space)
extern uint32 MacFrameSize;		// Size of frame buffer
extern int MacFrameLayout;		// Frame buffer layout (see defines below)

// Possible frame buffer layouts
enum {
	FLAYOUT_NONE,				// No frame buffer
	FLAYOUT_DIRECT,				// Frame buffer is in MacOS layout, no conversion needed
	FLAYOUT_HOST_555,			// 16 bit, RGB 555, host byte order
	FLAYOUT_HOST_565,			// 16 bit, RGB 565, host byte order
	FLAYOUT_HOST_888			// 32 bit, RGB 888, host byte order
};

// Memory system lifecycle functions
extern void memory_init(void);
extern void memory_exit(void);
extern void memory_set_flat_dummy_window(bool flat_dummy);
extern void memory_reconfigure_window(void);
extern void memory_commit_range(uint32 mac_addr, uint32 size, int prot);
extern void memory_map_framebuffer(void);
extern bool memory_is_mapped(uint32 addr, uint32 size);
extern int memory_get_mapped_ranges(uint32 *out_start, uint32 *out_end, int max_ranges);
extern void InitFrameBufferMapping(void);
extern uint8 *get_real_address(uint32 addr);
extern uint32 get_virtual_address(uint8 *addr);

extern memory_fault_jmp_buf *memory_guard_enter(void);
extern void memory_guard_leave(void);
extern void memory_guard_clear(void);
extern uint32 memory_guest_fault_addr(void);
extern int memory_try_handle_guest_fault(const void *si_addr);
extern void memory_raise_guest_fault(uint32 addr);

// SCC helper functions for memory-mapped I/O trapping
extern uint32 scc_bget(uint32 addr);
extern void scc_bput(uint32 addr, uint32 b);

/*
 * Checks if a Macintosh guest address falls in the Z8530 SCC MMIO range.
 *
 * Arguments:
 *   addr: 32-bit Macintosh address.
 *
 * Returns:
 *   true if address is an SCC MMIO access, false otherwise.
 */
static inline bool is_scc_addr(uint32 addr)
{
	if (TwentyFourBitAddressing) {
		/* In 24-bit mode the Z8530 SCC is mirrored across two 1MB read/write windows:
		 *   0x900000–0x9FFFFF  (SCC Read)
		 *   0xB00000–0xBFFFFF  (SCC Write)
		 * These correspond to the original map_banks(&scc_bank, 0x90, 0x10)
		 * and map_banks(&scc_bank, 0xb0, 0x10) bank assignments. */
		uint32 a24 = addr & 0x00ffffff;
		return ((a24 >= 0x00900000 && a24 < 0x00a00000) ||
		        (a24 >= 0x00b00000 && a24 < 0x00c00000));
	} else {
		/* In 32-bit mode the SCC occupies a full 16MB window at 0x50000000.
		 * This matches map_banks(&scc_bank, 0x5000, 0x100):
		 *   start = 0x5000 bank-units  → 0x5000 * 0x10000 = 0x50000000
		 *   size  = 0x100  bank-units  → 0x100  * 0x10000 = 0x01000000 (16MB)
		 * On a Quadra 800 (and similar 32-bit Macs) the ltlk driver accesses
		 * the Z8530 at 0x50F00000+, well above the erroneously-short 0x50100000
		 * limit that was previously used. */
		return (addr >= 0x50000000 && addr < 0x51000000);
	}
}

/*
 * Translates a 32-bit Macintosh guest address to a flat host pointer.
 *
 * Arguments:
 *   addr: 32-bit Macintosh address.
 *
 * Returns:
 *   Host pointer inside Host_Mem_Base window. Uncommitted holes are
 *   PROT_NONE and raise SIGSEGV / ACCESS_VIOLATION on dereference.
 */
static inline uint8 *Mac2HostAddr(uint32 addr)
{
	// Mask address to 24 bits if running in vintage 24-bit addressing mode
	if (TwentyFourBitAddressing)
		addr &= 0x00ffffff;
	return Host_Mem_Base + addr;
}

/*
 * Translates a host pointer inside the 4GB window back to a 32-bit Mac address.
 *
 * Arguments:
 *   addr: Host pointer inside Host_Mem_Base window.
 *
 * Returns:
 *   32-bit Macintosh guest address.
 */
static inline uint32 Host2MacAddr(uint8 *addr)
{
	// Validate pointer against 4GB flat host memory boundaries
	if (!Host_Mem_Base || addr < Host_Mem_Base || addr >= Host_Mem_Base + 0x100000000ULL)
		return 0;
	return (uint32)((uintptr)addr - (uintptr)Host_Mem_Base);
}

/*
 * Reads an 8-bit byte from Macintosh guest address space.
 *
 * Arguments:
 *   addr: 32-bit Macintosh address.
 *
 * Returns:
 *   8-bit unsigned integer value.
 */
static inline uint32 ReadMacInt8(uint32 addr)
{
	// Route MMIO addresses to SCC serial controller
	if (is_scc_addr(addr))
		return scc_bget(addr);
	return (uint32)*(uint8 *)Mac2HostAddr(addr);
}

/*
 * Reads a 16-bit big-endian word from Macintosh guest address space.
 *
 * Arguments:
 *   addr: 32-bit Macintosh address.
 *
 * Returns:
 *   16-bit host-endian unsigned integer value.
 */
static inline uint32 ReadMacInt16(uint32 addr)
{
	// Route MMIO addresses to SCC serial controller
	if (is_scc_addr(addr))
		return (scc_bget(addr) << 8) | scc_bget(addr + 1);
	return do_get_mem_word((uint16 *)Mac2HostAddr(addr));
}

/*
 * Reads a 32-bit big-endian longword from Macintosh guest address space.
 *
 * Arguments:
 *   addr: 32-bit Macintosh address.
 *
 * Returns:
 *   32-bit host-endian unsigned integer value.
 */
static inline uint32 ReadMacInt32(uint32 addr)
{
	// Route MMIO addresses to SCC serial controller
	if (is_scc_addr(addr))
		return (ReadMacInt16(addr) << 16) | ReadMacInt16(addr + 2);
	return do_get_mem_long((uint32 *)Mac2HostAddr(addr));
}

/*
 * Writes an 8-bit byte to Macintosh guest address space.
 *
 * Arguments:
 *   addr: 32-bit Macintosh address.
 *   b: 8-bit unsigned integer value to store.
 */
static inline void WriteMacInt8(uint32 addr, uint32 b)
{
	// Route MMIO addresses to SCC serial controller
	if (is_scc_addr(addr)) {
		scc_bput(addr, b);
		return;
	}
	// Protect ROM region from guest writes
	if (!TwentyFourBitAddressing && addr >= ROMBaseMac && addr < ROMBaseMac + ROMSize)
		return;
	*(uint8 *)Mac2HostAddr(addr) = (uint8)b;
}

/*
 * Writes a 16-bit word to Macintosh guest address space in big-endian order.
 *
 * Arguments:
 *   addr: 32-bit Macintosh address.
 *   w: 16-bit unsigned integer value to store.
 */
static inline void WriteMacInt16(uint32 addr, uint32 w)
{
	// Route MMIO addresses to SCC serial controller
	if (is_scc_addr(addr)) {
		scc_bput(addr, w >> 8);
		scc_bput(addr + 1, w & 0xff);
		return;
	}
	// Protect ROM region from guest writes
	if (!TwentyFourBitAddressing && addr >= ROMBaseMac && addr < ROMBaseMac + ROMSize)
		return;
	do_put_mem_word((uint16 *)Mac2HostAddr(addr), (uint16)w);
}

/*
 * Writes a 32-bit longword to Macintosh guest address space in big-endian order.
 *
 * Arguments:
 *   addr: 32-bit Macintosh address.
 *   l: 32-bit unsigned integer value to store.
 */
static inline void WriteMacInt32(uint32 addr, uint32 l)
{
	// Route MMIO addresses to SCC serial controller
	if (is_scc_addr(addr)) {
		WriteMacInt16(addr, l >> 16);
		WriteMacInt16(addr + 2, l & 0xffff);
		return;
	}
	// Protect ROM region from guest writes
	if (!TwentyFourBitAddressing && addr >= ROMBaseMac && addr < ROMBaseMac + ROMSize)
		return;
	do_put_mem_long((uint32 *)Mac2HostAddr(addr), l);
}

static inline void *Mac_memset(uint32 addr, int c, size_t n) {return memset(Mac2HostAddr(addr), c, n);}
static inline void *Mac2Host_memcpy(void *dest, uint32 src, size_t n) {return memcpy(dest, Mac2HostAddr(src), n);}
static inline void *Host2Mac_memcpy(uint32 dest, const void *src, size_t n) {return memcpy(Mac2HostAddr(dest), src, n);}
static inline void *Mac2Mac_memcpy(uint32 dest, uint32 src, size_t n) {return memcpy(Mac2HostAddr(dest), Mac2HostAddr(src), n);}


/*
 *  680x0 emulation
 */

// Initialization
extern bool Init680x0(void);
extern void Exit680x0(void);
extern bool UseJIT;
extern bool UseJITFPU;
extern uint32 JITCacheSize;

// 680x0 emulation functions
struct M68kRegisters;

#ifdef __cplusplus
extern "C" {
#endif

extern void Start680x0(void);									// Reset and start 680x0
extern void Reset680x0(void);									// Reset running 680x0
/* Execute68k/Execute68kTrap: r->d[0..7] and r->a[0..6] are in/out; r->a[7] and r->sr
 * are unused (historic Amiga/UAE contract). The engine keeps the live A7/SR. */
extern void Execute68k(uint32 addr, struct M68kRegisters *r);		// Execute 68k code from EMUL_OP routine
extern void Execute68kTrap(uint16 trap, struct M68kRegisters *r);	// Execute MacOS 68k trap from EMUL_OP routine

// Interrupt functions
extern void TriggerInterrupt(void);								// Trigger interrupt level 1 (InterruptFlag must be set first)
extern void TriggerNMI(void);									// Trigger interrupt level 7
extern int intlev(void);

#ifdef __cplusplus
}
#endif

#endif
