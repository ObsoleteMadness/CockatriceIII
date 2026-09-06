/*
 * basilisk_disk_test.cpp - DiskInit/Open/Prime against a temp image via Sys_*
 *
 * DiskOpen calls Execute68kTrap(NewPtrSysClear / AddDrive). The harness plants
 * a Line-A handler that bump-allocates DrvSts and no-ops AddDrive so we can
 * exercise 512-byte read/write without a Mac heap.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include "test_harness.h"
#include "test_env.h"
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "disk.h"
#include "macos_util.h"
#include "main.h"

int main(void)
{
	test_install_crash_handler();
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== basilisk_disk_test ===\n");
	CHECK(activate_cpu_engine("musashi", false), "activate musashi");

	const char *img = "/tmp/cockatrice_disk_test.img";
	FILE *fp = fopen(img, "wb");
	assert(fp);
	uint8 sector[512];
	memset(sector, 0, sizeof(sector));
	for (int i = 0; i < 64; i++)
		fwrite(sector, 1, 512, fp);
	fclose(fp);

	test_prefs_clear();
	test_prefs_add("disk", img);
	test_install_disk_trap_stubs(0xB000, 0x2000);

	DiskInit();
	uint32 dce = 0x6000;
	uint32 pb = 0x6100;
	Mac_memset(dce, 0, 64);
	Mac_memset(pb, 0, 64);
	int16 err = DiskOpen(pb, dce);
	CHECK(err == noErr, "DiskOpen on temp image returns noErr");

	/* Drive number assigned by FindFreeDriveNumber(1) is 1 when the queue is empty. */
	const int drive = 1;
	uint32 buf = 0x8000;
	for (int i = 0; i < 512; i++)
		WriteMacInt8(buf + i, (uint8)(i ^ 0xA5));

	Mac_memset(pb, 0, 64);
	WriteMacInt16(pb + ioVRefNum, (uint16)drive);
	WriteMacInt16(pb + ioTrap, 0); /* not aRdCmd => write */
	WriteMacInt32(pb + ioBuffer, buf);
	WriteMacInt32(pb + ioReqCount, 512);
	WriteMacInt16(pb + ioPosMode, 0);
	WriteMacInt32(dce + dCtlPosition, 0);
	err = DiskPrime(pb, dce);
	{
		char msg[80];
		snprintf(msg, sizeof(msg), "DiskPrime write 512 bytes (err=%d)", (int)err);
		CHECK(err == noErr, msg);
	}
	CHECK(ReadMacInt32(pb + ioActCount) == 512, "DiskPrime write ioActCount == 512");

	uint32 rbuf = 0x8800;
	Mac_memset(rbuf, 0, 512);
	Mac_memset(pb, 0, 64);
	WriteMacInt16(pb + ioVRefNum, (uint16)drive);
	WriteMacInt16(pb + ioTrap, aRdCmd);
	WriteMacInt32(pb + ioBuffer, rbuf);
	WriteMacInt32(pb + ioReqCount, 512);
	WriteMacInt32(dce + dCtlPosition, 0);
	err = DiskPrime(pb, dce);
	CHECK(err == noErr, "DiskPrime read 512 bytes");
	bool match = true;
	for (int i = 0; i < 512; i++) {
		if (ReadMacInt8(rbuf + i) != (uint8)(i ^ 0xA5)) {
			match = false;
			break;
		}
	}
	CHECK(match, "DiskPrime write/read roundtrip 512 bytes");

	DiskExit();
	unlink(img);
	printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
