/*
 *  memory_musashi.cpp - Memory bank management and Musashi memory interface
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

#include <stdio.h>
#include <string.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "rom_patches.h"
#include "video.h"
#include "prefs.h"
#include "scc.h"
#include "m68k.h"

addrbank *mem_banks[65536];

#ifdef WORDS_BIGENDIAN
# define swap_words(X) (X)
#else
# define swap_words(X) (((X) >> 16) | ((X) << 16))
#endif

static uintptr RAMBaseDiff;	  // RAMBaseHost - RAMBaseMac
static uintptr ROMBaseDiff;	  // ROMBaseHost - ROMBaseMac
static uintptr FrameBaseDiff; // MacFrameBaseHost - MacFrameBaseMac

/*
 * Dummy bank (unmapped memory)
 */
static uint32 dummy_lget(uint32 addr) { (void)addr; return 0; }
static uint32 dummy_wget(uint32 addr) { (void)addr; return 0; }
static uint32 dummy_bget(uint32 addr) { (void)addr; return 0; }
static void dummy_lput(uint32 addr, uint32 l) { (void)addr; (void)l; }
static void dummy_wput(uint32 addr, uint32 w) { (void)addr; (void)w; }
static void dummy_bput(uint32 addr, uint32 b) { (void)addr; (void)b; }
static uint8 *dummy_xlate(uint32 addr) { (void)addr; return NULL; }

/*
 * Mac RAM (32-bit addressing)
 */
static uint32 ram_lget(uint32 addr)
{
	uint32 *m = (uint32 *)(RAMBaseDiff + addr);
	return do_get_mem_long(m);
}
static uint32 ram_wget(uint32 addr)
{
	uint16 *m = (uint16 *)(RAMBaseDiff + addr);
	return do_get_mem_word(m);
}
static uint32 ram_bget(uint32 addr)
{
	return (uint32)*(uint8 *)(RAMBaseDiff + addr);
}
static void ram_lput(uint32 addr, uint32 l)
{
	uint32 *m = (uint32 *)(RAMBaseDiff + addr);
	do_put_mem_long(m, l);
}
static void ram_wput(uint32 addr, uint32 w)
{
	uint16 *m = (uint16 *)(RAMBaseDiff + addr);
	do_put_mem_word(m, (uint16)w);
}
static void ram_bput(uint32 addr, uint32 b)
{
	*(uint8 *)(RAMBaseDiff + addr) = (uint8)b;
}
static uint8 *ram_xlate(uint32 addr)
{
	return (uint8 *)(RAMBaseDiff + addr);
}

/*
 * Mac RAM (24-bit addressing)
 */
static uint32 ram24_lget(uint32 addr)
{
	uint32 *m = (uint32 *)(RAMBaseDiff + (addr & 0x00ffffff));
	return do_get_mem_long(m);
}
static uint32 ram24_wget(uint32 addr)
{
	uint16 *m = (uint16 *)(RAMBaseDiff + (addr & 0x00ffffff));
	return do_get_mem_word(m);
}
static uint32 ram24_bget(uint32 addr)
{
	return (uint32)*(uint8 *)(RAMBaseDiff + (addr & 0x00ffffff));
}
static void ram24_lput(uint32 addr, uint32 l)
{
	uint32 *m = (uint32 *)(RAMBaseDiff + (addr & 0x00ffffff));
	do_put_mem_long(m, l);
}
static void ram24_wput(uint32 addr, uint32 w)
{
	uint16 *m = (uint16 *)(RAMBaseDiff + (addr & 0x00ffffff));
	do_put_mem_word(m, (uint16)w);
}
static void ram24_bput(uint32 addr, uint32 b)
{
	*(uint8 *)(RAMBaseDiff + (addr & 0x00ffffff)) = (uint8)b;
}
static uint8 *ram24_xlate(uint32 addr)
{
	return (uint8 *)(RAMBaseDiff + (addr & 0x00ffffff));
}

/*
 * Mac ROM (32-bit addressing)
 */
static uint32 rom_lget(uint32 addr)
{
	uint32 *m = (uint32 *)(ROMBaseDiff + addr);
	return do_get_mem_long(m);
}
static uint32 rom_wget(uint32 addr)
{
	uint16 *m = (uint16 *)(ROMBaseDiff + addr);
	return do_get_mem_word(m);
}
static uint32 rom_bget(uint32 addr)
{
	return (uint32)*(uint8 *)(ROMBaseDiff + addr);
}
static void rom_lput(uint32 addr, uint32 l) { (void)addr; (void)l; }
static void rom_wput(uint32 addr, uint32 w) { (void)addr; (void)w; }
static void rom_bput(uint32 addr, uint32 b) { (void)addr; (void)b; }
static uint8 *rom_xlate(uint32 addr)
{
	return (uint8 *)(ROMBaseDiff + addr);
}

/*
 * Mac ROM (24-bit addressing)
 */
static uint32 rom24_lget(uint32 addr)
{
	uint32 *m = (uint32 *)(ROMBaseDiff + (addr & 0x00ffffff));
	return do_get_mem_long(m);
}
static uint32 rom24_wget(uint32 addr)
{
	uint16 *m = (uint16 *)(ROMBaseDiff + (addr & 0x00ffffff));
	return do_get_mem_word(m);
}
static uint32 rom24_bget(uint32 addr)
{
	return (uint32)*(uint8 *)(ROMBaseDiff + (addr & 0x00ffffff));
}
static uint8 *rom24_xlate(uint32 addr)
{
	return (uint8 *)(ROMBaseDiff + (addr & 0x00ffffff));
}

/*
 * Frame buffer bank handlers
 */
static uint32 frame_direct_lget(uint32 addr)
{
	uint32 *m = (uint32 *)(FrameBaseDiff + addr);
	return do_get_mem_long(m);
}
static uint32 frame_direct_wget(uint32 addr)
{
	uint16 *m = (uint16 *)(FrameBaseDiff + addr);
	return do_get_mem_word(m);
}
static uint32 frame_direct_bget(uint32 addr)
{
	return (uint32)*(uint8 *)(FrameBaseDiff + addr);
}
static void frame_direct_lput(uint32 addr, uint32 l)
{
	uint32 *m = (uint32 *)(FrameBaseDiff + addr);
	do_put_mem_long(m, l);
}
static void frame_direct_wput(uint32 addr, uint32 w)
{
	uint16 *m = (uint16 *)(FrameBaseDiff + addr);
	do_put_mem_word(m, (uint16)w);
}
static void frame_direct_bput(uint32 addr, uint32 b)
{
	*(uint8 *)(FrameBaseDiff + addr) = (uint8)b;
}

static uint32 frame_host_555_lget(uint32 addr)
{
	uint32 *m = (uint32 *)(FrameBaseDiff + addr);
	uint32 l = *m;
	return swap_words(l);
}
static uint32 frame_host_555_wget(uint32 addr)
{
	uint16 *m = (uint16 *)(FrameBaseDiff + addr);
	return *m;
}
static void frame_host_555_lput(uint32 addr, uint32 l)
{
	uint32 *m = (uint32 *)(FrameBaseDiff + addr);
	*m = swap_words(l);
}
static void frame_host_555_wput(uint32 addr, uint32 w)
{
	uint16 *m = (uint16 *)(FrameBaseDiff + addr);
	*m = (uint16)w;
}

static uint32 frame_host_565_lget(uint32 addr)
{
	uint32 *m = (uint32 *)(FrameBaseDiff + addr);
	uint32 l = *m;
	l = (l & 0x001f001f) | ((l >> 1) & 0x7fe07fe0);
	return swap_words(l);
}
static uint32 frame_host_565_wget(uint32 addr)
{
	uint16 *m = (uint16 *)(FrameBaseDiff + addr);
	uint16 w = *m;
	return (w & 0x1f) | ((w >> 1) & 0x7fe0);
}
static void frame_host_565_lput(uint32 addr, uint32 l)
{
	uint32 *m = (uint32 *)(FrameBaseDiff + addr);
	l = (l & 0x001f001f) | ((l << 1) & 0xffc0ffc0);
	*m = swap_words(l);
}
static void frame_host_565_wput(uint32 addr, uint32 w)
{
	uint16 *m = (uint16 *)(FrameBaseDiff + addr);
	*m = (w & 0x1f) | ((w << 1) & 0xffc0);
}

static uint32 frame_host_888_lget(uint32 addr)
{
	uint32 *m = (uint32 *)(FrameBaseDiff + addr);
	return *m;
}
static void frame_host_888_lput(uint32 addr, uint32 l)
{
	uint32 *m = (uint32 *)(MacFrameBaseHost + addr - MacFrameBaseMac);
	*m = l;
}

static uint8 *frame_xlate(uint32 addr)
{
	return (uint8 *)(FrameBaseDiff + addr);
}

/*
 * SCC (Serial Communications Controller)
 */
static uint32 scc_bget(uint32 addr)
{
	uint32 a24 = addr & 0x00ffffff;
	if (TwentyFourBitAddressing) {
		return SCC_Access(0, false, (a24 >> 1) & 3);
	} else {
		return SCC_Access(0, false, (addr >> 1) & 3);
	}
}
static uint32 scc_wget(uint32 addr)
{
	return (scc_bget(addr) << 8) | scc_bget(addr + 1);
}
static uint32 scc_lget(uint32 addr)
{
	return (scc_wget(addr) << 16) | scc_wget(addr + 2);
}
static void scc_bput(uint32 addr, uint32 b)
{
	uint32 a24 = addr & 0x00ffffff;
	if (TwentyFourBitAddressing) {
		SCC_Access((uint8)b, true, (a24 >> 1) & 3);
	} else {
		SCC_Access((uint8)b, true, (addr >> 1) & 3);
	}
}
static void scc_wput(uint32 addr, uint32 w)
{
	scc_bput(addr, w >> 8);
	scc_bput(addr + 1, w & 0xff);
}
static void scc_lput(uint32 addr, uint32 l)
{
	scc_wput(addr, l >> 16);
	scc_wput(addr + 2, l & 0xffff);
}
static uint8 *scc_xlate(uint32 addr)
{
	(void)addr;
	return NULL;
}

/* Address bank structures */
addrbank dummy_bank = {
	dummy_lget, dummy_wget, dummy_bget,
	dummy_lput, dummy_wput, dummy_bput,
	dummy_xlate, NULL
};

addrbank ram_bank = {
	ram_lget, ram_wget, ram_bget,
	ram_lput, ram_wput, ram_bput,
	ram_xlate, NULL
};

addrbank ram24_bank = {
	ram24_lget, ram24_wget, ram24_bget,
	ram24_lput, ram24_wput, ram24_bput,
	ram24_xlate, NULL
};

addrbank rom_bank = {
	rom_lget, rom_wget, rom_bget,
	rom_lput, rom_wput, rom_bput,
	rom_xlate, NULL
};

addrbank rom24_bank = {
	rom24_lget, rom24_wget, rom24_bget,
	rom_lput, rom_wput, rom_bput,
	rom24_xlate, NULL
};

addrbank frame_direct_bank = {
	frame_direct_lget, frame_direct_wget, frame_direct_bget,
	frame_direct_lput, frame_direct_wput, frame_direct_bput,
	frame_xlate, NULL
};

addrbank frame_host_555_bank = {
	frame_host_555_lget, frame_host_555_wget, frame_direct_bget,
	frame_host_555_lput, frame_host_555_wput, frame_direct_bput,
	frame_xlate, NULL
};

addrbank frame_host_565_bank = {
	frame_host_565_lget, frame_host_565_wget, frame_direct_bget,
	frame_host_565_lput, frame_host_565_wput, frame_direct_bput,
	frame_xlate, NULL
};

addrbank frame_host_888_bank = {
	frame_host_888_lget, frame_direct_wget, frame_direct_bget,
	frame_host_888_lput, frame_direct_wput, frame_direct_bput,
	frame_xlate, NULL
};

addrbank scc_bank = {
	scc_lget, scc_wget, scc_bget,
	scc_lput, scc_wput, scc_bput,
	scc_xlate, NULL
};

void map_banks(addrbank *bank, int start, int size)
{
	int bnr;
	unsigned long int hioffs = 0, endhioffs = 0x100;

	if (start >= 0x100) {
		for (bnr = start; bnr < start + size; bnr++)
			put_mem_bank(bnr << 16, bank);
		return;
	}
	if (TwentyFourBitAddressing)
		endhioffs = 0x10000;
	for (hioffs = 0; hioffs < endhioffs; hioffs += 0x100) {
		for (bnr = start; bnr < start + size; bnr++)
			put_mem_bank((bnr + hioffs) << 16, bank);
	}
}

void memory_init(void)
{
	for (long i = 0; i < 65536; i++)
		put_mem_bank((uint32)(i << 16), &dummy_bank);

	// Limit RAM size to not overlap ROM
	uint32 ram_size = (RAMSize > ROMBaseMac) ? ROMBaseMac : RAMSize;

	if (TwentyFourBitAddressing) {
		RAMBaseDiff = (uintptr)RAMBaseHost - (uintptr)(RAMBaseMac & 0x00ffffff);
		ROMBaseDiff = (uintptr)ROMBaseHost - (uintptr)(ROMBaseMac & 0x00ffffff);
		FrameBaseDiff = (uintptr)MacFrameBaseHost - (uintptr)(MacFrameBaseMac & 0x00ffffff);

		// Map RAM and ROM with 24-bit mirror expansion
		map_banks(&ram24_bank, (RAMBaseMac & 0x00ffffff) >> 16, ram_size >> 16);
		map_banks(&rom24_bank, (ROMBaseMac & 0x00ffffff) >> 16, ROMSize >> 16);
	} else {
		RAMBaseDiff = (uintptr)RAMBaseHost - (uintptr)RAMBaseMac;
		ROMBaseDiff = (uintptr)ROMBaseHost - (uintptr)ROMBaseMac;
		FrameBaseDiff = (uintptr)MacFrameBaseHost - (uintptr)MacFrameBaseMac;

		// Map RAM and ROM
		map_banks(&ram_bank, RAMBaseMac >> 16, ram_size >> 16);
		map_banks(&rom_bank, ROMBaseMac >> 16, ROMSize >> 16);
	}

	// Map frame buffer
	switch (MacFrameLayout) {
		case FLAYOUT_DIRECT:
			map_banks(&frame_direct_bank, MacFrameBaseMac >> 16, (MacFrameSize >> 16) + 1);
			break;
		case FLAYOUT_HOST_555:
			map_banks(&frame_host_555_bank, MacFrameBaseMac >> 16, (MacFrameSize >> 16) + 1);
			break;
		case FLAYOUT_HOST_565:
			map_banks(&frame_host_565_bank, MacFrameBaseMac >> 16, (MacFrameSize >> 16) + 1);
			break;
		case FLAYOUT_HOST_888:
			map_banks(&frame_host_888_bank, MacFrameBaseMac >> 16, (MacFrameSize >> 16) + 1);
			break;
	}

	// Map SCC
	if (PrefsFindBool("ltoudp")) {
		if (TwentyFourBitAddressing) {
			map_banks(&scc_bank, 0x90, 0x10);
			map_banks(&scc_bank, 0xb0, 0x10);
		} else {
			map_banks(&scc_bank, 0x5000, 0x100);
		}
	}
}

uint8 *get_real_address(uint32 addr)
{
	return get_mem_bank(addr).xlateaddr(addr);
}

uint32 get_virtual_address(uint8 *addr)
{
	if (addr >= RAMBaseHost && addr < RAMBaseHost + RAMSize)
		return RAMBaseMac + (uint32)((uintptr)addr - (uintptr)RAMBaseHost);
	if (addr >= ROMBaseHost && addr < ROMBaseHost + ROMSize)
		return ROMBaseMac + (uint32)((uintptr)addr - (uintptr)ROMBaseHost);
	if (addr >= MacFrameBaseHost && addr < MacFrameBaseHost + MacFrameSize)
		return MacFrameBaseMac + (uint32)((uintptr)addr - (uintptr)MacFrameBaseHost);
	return 0;
}

/*
 * Musashi C Memory Callback Functions
 */
extern "C" {

unsigned int m68k_read_memory_8(unsigned int address)
{
	return get_mem_bank(address).bget(address);
}

unsigned int m68k_read_memory_16(unsigned int address)
{
	return get_mem_bank(address).wget(address);
}

unsigned int m68k_read_memory_32(unsigned int address)
{
	return get_mem_bank(address).lget(address);
}

void m68k_write_memory_8(unsigned int address, unsigned int value)
{
	get_mem_bank(address).bput(address, (uint8)value);
}

void m68k_write_memory_16(unsigned int address, unsigned int value)
{
	get_mem_bank(address).wput(address, (uint16)value);
}

void m68k_write_memory_32(unsigned int address, unsigned int value)
{
	get_mem_bank(address).lput(address, (uint32)value);
}

unsigned int m68k_read_disassembler_8(unsigned int address)
{
	return m68k_read_memory_8(address);
}

unsigned int m68k_read_disassembler_16(unsigned int address)
{
	return m68k_read_memory_16(address);
}

unsigned int m68k_read_disassembler_32(unsigned int address)
{
	return m68k_read_memory_32(address);
}

}
