/*
 * basilisk_memory_test.cpp - RAM/ROM banking, dummy-backed holes
 */

#include <stdio.h>
#include "test_harness.h"
#include "test_env.h"
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "main.h"

int main(void)
{
	test_install_crash_handler();
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== basilisk_memory_test ===\n");

	RAMSize = 1024 * 1024;
	RAMBaseMac = 0x00000000;
	ROMSize = 1024 * 1024;
	ROMBaseMac = 0x40800000;
	memory_init();
	Mac_memset(RAMBaseMac, 0, RAMSize);
	Mac_memset(ROMBaseMac, 0, ROMSize);

	WriteMacInt32(0x1000, 0x12345678);
	CHECK(ReadMacInt32(0x1000) == 0x12345678, "32-bit RAM Write/Read");
	CHECK(ReadMacInt16(0x1000) == 0x1234, "16-bit RAM Read Big-Endian High");
	CHECK(ReadMacInt16(0x1002) == 0x5678, "16-bit RAM Read Big-Endian Low");
	CHECK(ReadMacInt8(0x1000) == 0x12, "8-bit RAM Read Byte 0");
	CHECK(ReadMacInt8(0x1003) == 0x78, "8-bit RAM Read Byte 3");

	ROMBaseHost[0x100] = 0xDE;
	ROMBaseHost[0x101] = 0xAD;
	ROMBaseHost[0x102] = 0xBE;
	ROMBaseHost[0x103] = 0xEF;
	CHECK(ReadMacInt32(ROMBaseMac + 0x100) == 0xdeadbeef, "32-bit ROM Read");
	WriteMacInt32(ROMBaseMac + 0x100, 0x11223344);
	CHECK(ReadMacInt32(ROMBaseMac + 0x100) == 0xdeadbeef, "ROM Write Protection");

	CHECK(memory_is_mapped(0x1000, 4), "RAM is committed");
	CHECK(memory_is_mapped(ROMBaseMac + 0x100, 4), "ROM is committed");
	CHECK(memory_is_mapped(0x20000000, 4), "I/O range at 0x20000000 is dummy-backed");
	CHECK(ReadMacInt32(0x20000000) == 0, "Dummy-backed I/O range reads as zero");
	CHECK(memory_is_mapped(MacFrameBaseMac, 4), "NuBus framebuffer slot is dummy-backed before VideoInit");

	MacFrameSize = 4096;
	MacFrameLayout = FLAYOUT_DIRECT;
	memory_map_framebuffer();
	CHECK(memory_is_mapped(MacFrameBaseMac, 4), "Framebuffer bytes are committed");
	CHECK(memory_is_mapped(MacFrameBaseMac + 0x01000000, 4), "Rest of NuBus slot is still dummy-backed");
	MacFrameSize = 0;
	MacFrameLayout = FLAYOUT_NONE;

	printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
