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
#include "sysdeps.h"

/*
 *  Memory system
 */

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

// Banked Memory Definitions
typedef uint32 (*mem_get_func)(uint32 addr);
typedef void (*mem_put_func)(uint32 addr, uint32 val);
typedef uint8 *(*xlate_func)(uint32 addr);
typedef int (*check_func)(uint32 addr, uint32 size);

typedef struct {
	mem_get_func lget, wget, bget;
	mem_put_func lput, wput, bput;
	xlate_func xlateaddr;
	check_func check;
} addrbank;

extern addrbank *mem_banks[65536];
#define bankindex(addr) (((uint32)(addr)) >> 16)
#define get_mem_bank(addr) (*mem_banks[bankindex(addr)])
#define put_mem_bank(addr, b) (mem_banks[bankindex(addr)] = (b))

extern void memory_init(void);
extern void map_banks(addrbank *bank, int first, int count);
extern uint8 *get_real_address(uint32 addr);
extern uint32 get_virtual_address(uint8 *addr);

static inline uint32 ReadMacInt32(uint32 addr) {return get_mem_bank(addr).lget(addr);}
static inline uint32 ReadMacInt16(uint32 addr) {return get_mem_bank(addr).wget(addr);}
static inline uint32 ReadMacInt8(uint32 addr) {return get_mem_bank(addr).bget(addr);}
static inline void WriteMacInt32(uint32 addr, uint32 l) {get_mem_bank(addr).lput(addr, l);}
static inline void WriteMacInt16(uint32 addr, uint32 w) {get_mem_bank(addr).wput(addr, w);}
static inline void WriteMacInt8(uint32 addr, uint32 b) {get_mem_bank(addr).bput(addr, b);}
static inline uint8 *Mac2HostAddr(uint32 addr) {return get_real_address(addr);}
static inline uint32 Host2MacAddr(uint8 *addr) {return get_virtual_address(addr);}

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
extern void InitFrameBufferMapping(void);

const bool UseJIT = false;

// 680x0 emulation functions
struct M68kRegisters;
extern void Start680x0(void);									// Reset and start 680x0
extern void Reset680x0(void);									// Reset running 680x0
extern "C" void Execute68k(uint32 addr, M68kRegisters *r);		// Execute 68k code from EMUL_OP routine
extern "C" void Execute68kTrap(uint16 trap, M68kRegisters *r);	// Execute MacOS 68k trap from EMUL_OP routine

// Interrupt functions
extern void TriggerInterrupt(void);								// Trigger interrupt level 1 (InterruptFlag must be set first)
extern void TriggerNMI(void);									// Trigger interrupt level 7
extern int intlev(void);

#endif
