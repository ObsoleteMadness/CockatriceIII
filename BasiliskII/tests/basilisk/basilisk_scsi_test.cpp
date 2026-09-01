/*
 * basilisk_scsi_test.cpp - SCSI Manager, image attach, TIB DMA, 68k SCSIDispatch
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include "test_harness.h"
#include "test_env.h"
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "emul_op.h"
#include "scsi.h"
#include "main.h"

static void write_blank_image(const char *path, int sectors)
{
	FILE *fp = fopen(path, "wb");
	assert(fp != NULL);
	uint8 sector[512];
	memset(sector, 0, sizeof(sector));
	for (int i = 0; i < sectors; i++)
		fwrite(sector, 1, 512, fp);
	fclose(fp);
}

int main(void)
{
	test_install_crash_handler();
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== basilisk_scsi_test ===\n");
	CHECK(activate_cpu_engine("musashi", false), "activate musashi");

	SCSIInit();
	g_scsi_debug = false;

	const char *img_path = "/tmp/cockatrice_scsi_test.img";
	write_blank_image(img_path, 128);
	bool attached = SCSI_Attach(0, img_path);
	CHECK(attached, "SCSI_Attach to Target 0");
	CHECK(scsi_is_target_present(0), "Target 0 is present");
	CHECK(!scsi_is_target_present(1), "Target 1 is not present");

	CHECK(SCSIReset() == 0, "SCSIReset returns 0");
	CHECK(SCSIGet() == 0, "SCSIGet returns 0");
	CHECK(SCSIMgrBusy() != 0, "SCSIMgrBusy is true while bus is held");
	CHECK(SCSISelect(0) == 0, "SCSISelect(0) returns 0");

	uint8 inquiry_cdb[6] = {0x12, 0x00, 0x00, 0x00, 0x24, 0x00};
	CHECK(SCSICmd(6, inquiry_cdb) == 0, "SCSICmd(INQUIRY) returns 0");

	uint32 tib_addr = 0x7000;
	uint32 inq_buf = 0x7100;
	uint32 stat_addr = 0x7200;
	uint32 msg_addr = 0x7202;
	WriteMacInt16(tib_addr + 0, 2);
	WriteMacInt32(tib_addr + 2, inq_buf);
	WriteMacInt32(tib_addr + 6, 36);
	WriteMacInt16(tib_addr + 10, 7);
	WriteMacInt32(tib_addr + 12, 0);
	WriteMacInt32(tib_addr + 16, 0);
	CHECK(SCSIRead(tib_addr) == 0, "SCSIRead(INQUIRY TIB) returns 0");
	CHECK(SCSIComplete(1000, msg_addr, stat_addr) == 0, "SCSIComplete(INQUIRY) returns 0");
	CHECK(ReadMacInt16(stat_addr) == 0, "INQUIRY SCSI Status is GOOD");
	CHECK(ReadMacInt8(inq_buf) == 0x00, "INQUIRY returned direct access disk");

	WriteMacInt16(0x8000, M68K_EMUL_OP_SCSI_DISPATCH);
	WriteMacInt16(0x8002, 0x2E49);
	WriteMacInt16(0x8004, 0x4ED0);
	uint32 caller_addr = 0x8100;
	WriteMacInt16(caller_addr + 0, 0x4267);
	WriteMacInt16(caller_addr + 2, 0x4267);
	WriteMacInt16(caller_addr + 4, 0x4EB9);
	WriteMacInt32(caller_addr + 6, 0x8000);
	WriteMacInt16(caller_addr + 10, 0x301F);
	WriteMacInt16(caller_addr + 12, 0x4E75);
	struct M68kRegisters r;
	memset(&r, 0, sizeof(r));
	r.d[0] = 0xFFFF;
	Execute68k(caller_addr, &r);
	CHECK(r.d[0] == 0, "68k _SCSIDispatch(SCSIReset) returned 0");

	CHECK(SCSIGet() == 0, "SCSIGet before TestUnitReady");
	CHECK(SCSISelect(0) == 0, "SCSISelect(0) before TestUnitReady");
	uint8 tur_cdb[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	CHECK(SCSICmd(6, tur_cdb) == 0, "SCSICmd(TEST_UNIT_READY)");
	CHECK(SCSIComplete(1000, msg_addr, stat_addr) == 0, "SCSIComplete(TEST_UNIT_READY)");

	CHECK(SCSIGet() == 0, "SCSIGet before Write");
	CHECK(SCSISelect(0) == 0, "SCSISelect(0) before Write");
	uint8 write_cdb[10] = {0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
	CHECK(SCSICmd(10, write_cdb) == 0, "SCSICmd(WRITE(10))");
	uint32 write_buf = 0x9000;
	for (int i = 0; i < 512; i++)
		WriteMacInt8(write_buf + i, (uint8)(i ^ 0x5A));
	uint32 write_tib = 0x7300;
	WriteMacInt16(write_tib + 0, 2);
	WriteMacInt32(write_tib + 2, write_buf);
	WriteMacInt32(write_tib + 6, 512);
	WriteMacInt16(write_tib + 10, 7);
	WriteMacInt32(write_tib + 12, 0);
	WriteMacInt32(write_tib + 16, 0);
	CHECK(SCSIWrite(write_tib) == 0, "SCSIWrite(512 bytes)");
	CHECK(SCSIComplete(1000, msg_addr, stat_addr) == 0, "SCSIComplete(WRITE)");
	CHECK(ReadMacInt16(stat_addr) == 0, "WRITE SCSI Status is 0");

	CHECK(SCSIGet() == 0, "SCSIGet before Read");
	CHECK(SCSISelect(0) == 0, "SCSISelect(0) before Read");
	uint8 read_cdb[10] = {0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
	CHECK(SCSICmd(10, read_cdb) == 0, "SCSICmd(READ(10))");
	uint32 read_buf = 0x9800;
	Mac_memset(read_buf, 0, 512);
	uint32 read_tib = 0x7400;
	WriteMacInt16(read_tib + 0, 2);
	WriteMacInt32(read_tib + 2, read_buf);
	WriteMacInt32(read_tib + 6, 512);
	WriteMacInt16(read_tib + 10, 7);
	WriteMacInt32(read_tib + 12, 0);
	WriteMacInt32(read_tib + 16, 0);
	CHECK(SCSIRead(read_tib) == 0, "SCSIRead(512 bytes)");
	CHECK(SCSIComplete(1000, msg_addr, stat_addr) == 0, "SCSIComplete(READ)");
	CHECK(ReadMacInt16(stat_addr) == 0, "READ SCSI Status is 0");
	bool data_match = true;
	for (int i = 0; i < 512; i++) {
		if (ReadMacInt8(read_buf + i) != (uint8)(i ^ 0x5A)) {
			data_match = false;
			break;
		}
	}
	CHECK(data_match, "SCSI Write -> Read roundtrip 512 bytes");
	CHECK(SCSI_Detach(0), "SCSI_Detach(0)");
	CHECK(!scsi_is_target_present(0), "Target 0 detached");
	unlink(img_path);

	/* Advanced unaligned multi-block */
	SCSIInit();
	const char *img2 = "/tmp/cockatrice_scsi_adv_test.img";
	write_blank_image(img2, 64);
	CHECK(SCSI_Attach(0, img2), "Attach disk image for advanced SCSI tests");
	CHECK(SCSIGet() == 0, "SCSIGet for advance tests");
	CHECK(SCSISelect(0) == 0, "SCSISelect(0)");
	CHECK(SCSICmd(6, tur_cdb) == 0, "SCSICmd(TEST UNIT READY)");
	CHECK(SCSIComplete(1000, msg_addr, stat_addr) == 0, "SCSIComplete(TEST UNIT READY)");

	CHECK(SCSIGet() == 0, "SCSIGet before multi-sector write");
	CHECK(SCSISelect(0) == 0, "SCSISelect(0) before multi-sector write");
	uint8 write_cdb4[10] = {0x2A, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x04, 0x00};
	CHECK(SCSICmd(10, write_cdb4) == 0, "SCSICmd(WRITE(10) 4 blocks)");
	uint32 unaligned_buf = 0x9003;
	for (int i = 0; i < 2048; i++)
		WriteMacInt8(unaligned_buf + i, (uint8)((i * 37 + 13) & 0xFF));
	WriteMacInt16(tib_addr + 0, 2);
	WriteMacInt32(tib_addr + 2, unaligned_buf);
	WriteMacInt32(tib_addr + 6, 2048);
	WriteMacInt16(tib_addr + 10, 7);
	CHECK(SCSIWrite(tib_addr) == 0, "SCSIWrite(2048 bytes unaligned)");
	CHECK(SCSIComplete(1000, msg_addr, stat_addr) == 0, "SCSIComplete(WRITE 4 blocks)");
	CHECK(ReadMacInt16(stat_addr) == 0, "Multi-block WRITE status is GOOD");

	CHECK(SCSIGet() == 0, "SCSIGet before multi-sector read");
	CHECK(SCSISelect(0) == 0, "SCSISelect(0) before multi-sector read");
	uint8 read_cdb4[10] = {0x28, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x04, 0x00};
	CHECK(SCSICmd(10, read_cdb4) == 0, "SCSICmd(READ(10) 4 blocks)");
	uint32 read_dest = 0xA005;
	Mac_memset(read_dest, 0, 2048);
	WriteMacInt16(read_tib + 0, 2);
	WriteMacInt32(read_tib + 2, read_dest);
	WriteMacInt32(read_tib + 6, 2048);
	WriteMacInt16(read_tib + 10, 7);
	CHECK(SCSIRead(read_tib) == 0, "SCSIRead(2048 bytes unaligned)");
	CHECK(SCSIComplete(1000, msg_addr, stat_addr) == 0, "SCSIComplete(READ 4 blocks)");
	bool match = true;
	for (int i = 0; i < 2048; i++) {
		if (ReadMacInt8(read_dest + i) != (uint8)((i * 37 + 13) & 0xFF)) {
			match = false;
			break;
		}
	}
	CHECK(match, "Multi-block unaligned SCSI transfer 2048 bytes");
	SCSI_Detach(0);
	unlink(img2);

	printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
