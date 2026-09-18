/*
 *  memory_musashi.cpp - Musashi 680x0 core memory callbacks
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *  CockatriceIII Multi-Engine Architecture (C) 2026
 *
 *  Musashi fetches and stores through ReadMacInt8 / WriteMacInt8 so every
 *  engine (Musashi, UAE, m68k-rs) shares one flat Host_Mem_Base window
 *  and the same SCC MMIO + ROM write-protect rules in cpu_emulation.h.
 */

#include <stdio.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "m68k.h"

extern "C" {

/*
 * Reads an 8-bit byte from guest memory for the Musashi 680x0 CPU core.
 *
 * Arguments:
 *   address: 32-bit Macintosh address.
 *
 * Returns:
 *   8-bit unsigned integer value.
 */
unsigned int m68k_read_memory_8(unsigned int address)
{
	return ReadMacInt8(address);
}

/*
 * Reads a 16-bit big-endian word from guest memory for the Musashi CPU core.
 *
 * Arguments:
 *   address: 32-bit Macintosh address.
 *
 * Returns:
 *   16-bit host-endian unsigned integer value.
 */
unsigned int m68k_read_memory_16(unsigned int address)
{
	return ReadMacInt16(address);
}

/*
 * Reads a 32-bit big-endian longword from guest memory for the Musashi CPU core.
 *
 * Arguments:
 *   address: 32-bit Macintosh address.
 *
 * Returns:
 *   32-bit host-endian unsigned integer value.
 */
unsigned int m68k_read_memory_32(unsigned int address)
{
	return ReadMacInt32(address);
}

/*
 * Writes an 8-bit byte to guest memory from the Musashi CPU core.
 *
 * Arguments:
 *   address: 32-bit Macintosh address.
 *   value: 8-bit value to store.
 */
void m68k_write_memory_8(unsigned int address, unsigned int value)
{
#if SCC_DEBUG
	/* Log writes to the full Mac I/O space to find SERD/ltlk SCC addresses. */
	if (address >= 0x50000000 && address < 0x60000000) {
		printf("SCC: IO-TRACE WR addr=%08x scc=%d val=%02x\n",
			address, is_scc_addr(address) ? 1 : 0, value & 0xff);
		fflush(stdout);
	}
#endif
	WriteMacInt8(address, value);
}

/*
 * Writes a 16-bit word to guest memory in big-endian order from the Musashi CPU core.
 *
 * Arguments:
 *   address: 32-bit Macintosh address.
 *   value: 16-bit value to store.
 */
void m68k_write_memory_16(unsigned int address, unsigned int value)
{
	WriteMacInt16(address, value);
}

/*
 * Writes a 32-bit longword to guest memory in big-endian order from the Musashi CPU core.
 *
 * Arguments:
 *   address: 32-bit Macintosh address.
 *   value: 32-bit value to store.
 */
void m68k_write_memory_32(unsigned int address, unsigned int value)
{
	WriteMacInt32(address, value);
}

/*
 * Reads an 8-bit opcode byte for the Musashi instruction disassembler.
 *
 * Arguments:
 *   address: 32-bit Macintosh address.
 *
 * Returns:
 *   8-bit unsigned integer value.
 */
unsigned int m68k_read_disassembler_8(unsigned int address)
{
	return m68k_read_memory_8(address);
}

/*
 * Reads a 16-bit opcode word for the Musashi instruction disassembler.
 *
 * Arguments:
 *   address: 32-bit Macintosh address.
 *
 * Returns:
 *   16-bit host-endian unsigned integer value.
 */
unsigned int m68k_read_disassembler_16(unsigned int address)
{
	return m68k_read_memory_16(address);
}

/*
 * Reads a 32-bit opcode longword for the Musashi instruction disassembler.
 *
 * Arguments:
 *   address: 32-bit Macintosh address.
 *
 * Returns:
 *   32-bit host-endian unsigned integer value.
 */
unsigned int m68k_read_disassembler_32(unsigned int address)
{
	return m68k_read_memory_32(address);
}

}
