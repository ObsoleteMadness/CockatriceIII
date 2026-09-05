/*
 *  rom_patches.cpp - ROM patches
 *
 *  Basilisk II (C) 1997-1999 Christian Bauer
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
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <string.h>
#include <vector>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "emul_op.h"
#include "macos_util.h"
#include "slot_rom.h"
#include "sony.h"
#include "disk.h"
#include "cdrom.h"
#include "video.h"
#include "extfs.h"
#include "prefs.h"
#include "rom_patches.h"

#define DEBUG 0
#include "debug.h"


// Global variables
uint32 UniversalInfo;		// ROM offset of UniversalInfo
uint32 PutScrapPatch;		// Mac address of PutScrap() patch
uint32 ROMBreakpoint = 0;	// ROM offset of breakpoint (0 = disabled, 0x2310 = CritError)
bool PrintROMInfo = false;	// Flag: print ROM information in PatchROM()

static uint32 sony_offset;				// ROM offset of .Sony driver
static uint32 serd_offset;				// ROM offset of SERD resource (serial drivers)
static uint32 microseconds_offset;		// ROM offset of Microseconds() replacement routine

// Prototypes
uint16 ROMVersion;


/*
 *  Patch application log
 *
 *  Every patch PatchROM() attempts appends one record here, whether or not its
 *  site was found. Two consumers:
 *
 *    - Console output. A patch that silently misses used to be invisible until
 *      the machine bombed tens of thousands of instructions later; now the miss
 *      is one line at patch time.
 *    - Offline tests. BasiliskII/tests asserts the whole log against a golden
 *      manifest for dist/Quadra800.rom, so a patch that stops matching a ROM is
 *      a unit-test diff rather than a boot bomb.
 *
 *  See docs/rom-patches-vs-supermario.md for what each patch corresponds to in
 *  the Apple Mac OS ROM sources.
 */

static std::vector<PatchRecord> patch_log;

/*
 *  Returns the patch log for the most recent PatchROM() call.
 */

const std::vector<PatchRecord> &GetPatchLog(void)
{
	return patch_log;
}

/*
 *  Record one patch attempt and report it on the console.
 *
 *  Arguments:
 *    name:       stable identifier for this patch; matched by the golden
 *                manifest, so renaming one requires regenerating it.
 *    source_ref: Mac OS ROM source that justifies the patch, or NULL where no
 *                source exists (Sound Mgr, serial, AppleTalk).
 *    offset:     ROM offset the patch was applied at, or 0 if the site was not
 *                found.
 *    required:   true if a miss should fail the whole patch pass. This function
 *                only records and reports; the caller still decides to return
 *                false, because the recovery differs from site to site.
 *
 *  Returns:
 *    offset, so call sites can wrap a locator without adding a statement.
 */

static uint32 log_patch(const char *name, const char *source_ref, uint32 offset, bool required)
{
	PatchRecord rec;
	rec.name = name;
	rec.source_ref = source_ref;
	rec.offset = offset;
	rec.required = required;
	rec.applied = (offset != 0);
	patch_log.push_back(rec);

	if (offset)
		printf("[ROM-PATCH] %-34s @ %06x\n", name, offset);
	else
		printf("[ROM-PATCH] %-34s MISSED (%s)\n", name, required ? "REQUIRED" : "optional");
	return offset;
}


/*
 *  Search ROM for byte string, return ROM offset (or 0)
 */

static uint32 find_rom_data(uint32 start, uint32 end, const uint8 *data, uint32 data_len)
{
	uint32 ofs = start;
	while (ofs < end) {
		if (!memcmp((void *)(ROMBaseHost + ofs), data, data_len))
			return ofs;
		ofs++;
	}
	return 0;
}


/*
 *  Search ROM resource by type/ID, return ROM offset of resource data
 */

static uint32 rsrc_ptr = 0;

static uint32 find_rom_resource(uint32 s_type, int16 s_id, bool cont = false)
{
	uint32 lp = ROMBaseMac + ReadMacInt32(ROMBaseMac + 0x1a);
	uint32 x = ReadMacInt32(lp);

	if (!cont)
		rsrc_ptr = x;
	else
		rsrc_ptr = ReadMacInt32(ROMBaseMac + rsrc_ptr + 8);

	for (;;) {
		lp = ROMBaseMac + rsrc_ptr;
		uint32 data = ReadMacInt32(lp + 12);
		uint32 type = ReadMacInt32(lp + 16);
		int16 id = ReadMacInt16(lp + 20);

		if (type == s_type && id == s_id)
			return data;

		rsrc_ptr = ReadMacInt32(lp + 8);
		if (!rsrc_ptr)
			break;
	}
	return 0;
}


/*
 *  Search offset of A-Trap routine in ROM
 */

static uint32 find_rom_trap(uint16 trap)
{
	uint8 *bp = (uint8 *)(ROMBaseHost + ReadMacInt32(ROMBaseMac + 0x22));
	uint16 rom_trap = 0xa800;
	uint32 ofs = 0;

again:
	for (int i=0; i<0x400; i++) {
		bool unimplemented = false;
		uint8 b = *bp++;
		if (b == 0x80)			// Unimplemented trap
			unimplemented = true;
		else if (b == 0xff) {	// Absolute address
			ofs = (bp[0] << 24) | (bp[1] << 16) | (bp[2] << 8) | bp[3];
			bp += 4;
		} else if (b & 0x80) {	// 1 byte offset
			int16 add = (b & 0x7f) << 1;
			if (!add)
				return 0;
			ofs += add;
		} else {				// 2 byte offset
			int16 add = ((b << 8) | *bp++) << 1;
			if (!add)
				return 0;
			ofs += add;
		}
		if (rom_trap == trap)
			return unimplemented ? 0 : ofs;
		rom_trap++;
	}
	rom_trap = 0xa000;
	goto again;
}


/*
 *  Print ROM information to stream,
 */

static void list_rom_resources(void)
{
	printf("ROM Resources:\n");
	printf("Offset\t Type\tID\tSize\tName\n");
	printf("------------------------------------------------\n");

	uint32 lp = ROMBaseMac + ReadMacInt32(ROMBaseMac + 0x1a);
	uint32 rsrc_ptr = ReadMacInt32(lp);

	for (;;) {
		lp = ROMBaseMac + rsrc_ptr;
		uint32 data = ReadMacInt32(lp + 12);

		char name[32];
		int name_len = ReadMacInt8(lp + 23), i;
		for (i=0; i<name_len; i++)
			name[i] = ReadMacInt8(lp + 24 + i);
		name[i] = 0;

		printf("%08x %c%c%c%c\t%d\t%d\t%s\n", data, ReadMacInt8(lp + 16), ReadMacInt8(lp + 17), ReadMacInt8(lp + 18), ReadMacInt8(lp + 19), ReadMacInt16(lp + 20), ReadMacInt32(ROMBaseMac + data - 8), name);

		rsrc_ptr = ReadMacInt32(lp + 8);
		if (!rsrc_ptr)
			break;
	}
	printf("\n");
}

// Mapping of Model IDs to Model names
struct mac_desc {
	char *name;
	int32 id;
};

static mac_desc MacDesc[] = {
	{"Classic"				, 1},
	{"Mac XL"				, 2},
	{"Mac 512KE"			, 3},
	{"Mac Plus"				, 4},
	{"Mac SE"				, 5},
	{"Mac II"				, 6},
	{"Mac IIx"				, 7},
	{"Mac IIcx"				, 8},
	{"Mac SE/030"			, 9},
	{"Mac Portable"			, 10},
	{"Mac IIci"				, 11},
	{"Mac IIfx"				, 13},
	{"Mac Classic"			, 17},
	{"Mac IIsi"				, 18},
	{"Mac LC"				, 19},
	{"Quadra 900"			, 20},
	{"PowerBook 170"		, 21},
	{"Quadra 700"			, 22},
	{"Classic II"			, 23},
	{"PowerBook 100"		, 24},
	{"PowerBook 140"		, 25},
	{"Quadra 950"			, 26},
	{"Mac LCIII/Performa 450", 27},
	{"PowerBook Duo 210"	, 29},
	{"Centris 650"			, 30},
	{"PowerBook Duo 230"	, 32},
	{"PowerBook 180"		, 33},
	{"PowerBook 160"		, 34},
	{"Quadra 800"			, 35},
	{"Quadra 650"			, 36},
	{"Mac LCII"				, 37},
	{"PowerBook Duo 250"	, 38},
	{"Mac IIvi"				, 44},
	{"Mac IIvm/Performa 600", 45},
	{"Mac IIvx"				, 48},
	{"Color Classic/Performa 250", 49},
	{"PowerBook 165c"		, 50},
	{"Centris 610"			, 52},
	{"Quadra 610"			, 53},
	{"PowerBook 145"		, 54},
	{"Mac LC520"			, 56},
	{"Quadra/Centris 660AV"	, 60},
	{"Performa 46x"			, 62},
	{"PowerBook 180c"		, 71},
	{"PowerBook 520/520c/540/540c", 72},
	{"PowerBook Duo 270c"	, 77},
	{"Quadra 840AV"			, 78},
	{"Performa 550"			, 80},
	{"PowerBook 165"		, 84},
	{"PowerBook 190"		, 85},
	{"Mac TV"				, 88},
	{"Mac LC475/Performa 47x", 89},
	{"Mac LC575"			, 92},
	{"Quadra 605"			, 94},
	{"Quadra 630"			, 98},
	{"Mac LC580"			, 99},
	{"PowerBook Duo 280"	, 102},
	{"PowerBook Duo 280c"	, 103},
	{"PowerBook 150"		, 115},
	{"unknown", -1}
};

static void print_universal_info(uint32 info)
{
	uint8 id = ReadMacInt8(info + 18);
	uint16 hwcfg = ReadMacInt16(info + 16);
	uint16 rom85 = ReadMacInt16(info + 20);

	// Find model name
	char *name = "unknown";
	for (int i=0; MacDesc[i].id >= 0; i++)
		if (MacDesc[i].id == id + 6) {
			name = MacDesc[i].name;
			break;
		}

	printf("%08x %02x\t%04x\t%04x\t%s\n", info - ROMBaseMac, id, hwcfg, rom85, name);
}

static void list_universal_infos(void)
{
	uint32 ofs = 0x3000;
	for (int i=0; i<0x2000; i+=2, ofs+=2)
		if (ReadMacInt32(ROMBaseMac + ofs) == 0xdc000505) {
			ofs -= 16;
			uint32 q;
			for (q=ofs; q > 0 && ReadMacInt32(ROMBaseMac + q) != ofs - q; q-=4) ;
			if (q > 0) {
				printf("Universal Table at %08x:\n", q);
				printf("Offset\t ID\tHWCfg\tROM85\tModel\n");
				printf("------------------------------------------------\n");
				while (ofs = ReadMacInt32(ROMBaseMac + q)) {
					print_universal_info(ROMBaseMac + ofs + q);
					q += 4;
				}
			}
			break;
		}
	printf("\n");
}

static void print_rom_info(void)
{
	printf("\nROM Info:\n");
	printf("Checksum    : %08x\n", ReadMacInt32(ROMBaseMac));
	printf("Version     : %04x\n", ROMVersion);
	printf("Sub Version : %04x\n\n", ReadMacInt16(ROMBaseMac + 18));
	printf("Resource Map: %08x\n", ReadMacInt32(ROMBaseMac + 26));
	printf("Trap Tables : %08x\n\n", ReadMacInt32(ROMBaseMac + 34));
#if 1
	if (ROMVersion == ROM_VERSION_32) {
		list_rom_resources();
		list_universal_infos();
	}
#endif
}


/*
 *  Resolve an A-trap to its ROM offset, logging the outcome.
 *
 *  find_rom_trap() returns 0 both for "trap is unimplemented" and "trap not
 *  found", and offset 0 is the ROM header -- so an unchecked miss writes the
 *  replacement routine over the checksum instead of skipping it. Every caller
 *  goes through here and treats 0 as fatal.
 *
 *  Arguments:
 *    trap:       A-trap number, e.g. 0xa058 for _InsTime.
 *    name:       log label, conventionally "RoutineName ($ATRAP)".
 *    source_ref: Mac OS ROM source for the routine being replaced.
 *
 *  Returns:
 *    ROM offset of the trap routine, or 0 if it could not be resolved. Callers
 *    must return false from the patch pass when this is 0.
 */

static uint32 require_rom_trap(uint16 trap, const char *name, const char *source_ref)
{
	return log_patch(name, source_ref, find_rom_trap(trap), true);
}


/*
 *  Locate a ROM resource, logging the outcome.
 *
 *  Same hazard as require_rom_trap(): find_rom_resource() reports "absent" as
 *  offset 0, and callers memcpy whole driver images to the returned offset.
 *
 *  Arguments:
 *    type:       resource type, e.g. 'DRVR'.
 *    id:         resource ID.
 *    name:       log label.
 *    source_ref: Mac OS ROM source, or NULL if none exists.
 *    required:   whether a miss should fail the patch pass.
 *
 *  Returns:
 *    ROM offset of the resource data, or 0 if absent.
 */

static uint32 locate_rom_resource(uint32 type, int16 id, const char *name,
                                  const char *source_ref, bool required)
{
	return log_patch(name, source_ref, find_rom_resource(type, id), required);
}


/*
 *  Driver stubs
 */

static const uint8 sony_driver[] = {	// Replacement for .Sony driver
	// Driver header
	SonyDriverFlags >> 8, SonyDriverFlags & 0xff, 0, 0, 0, 0, 0, 0,
	0x00, 0x18,							// Open() offset
	0x00, 0x1c,							// Prime() offset
	0x00, 0x20,							// Control() offset
	0x00, 0x2c,							// Status() offset
	0x00, 0x52,							// Close() offset
	0x05, 0x2e, 0x53, 0x6f, 0x6e, 0x79,	// ".Sony"

	// Open()
	M68K_EMUL_OP_SONY_OPEN >> 8, M68K_EMUL_OP_SONY_OPEN & 0xff,
	0x4e, 0x75,							//  rts

	// Prime()
	M68K_EMUL_OP_SONY_PRIME >> 8, M68K_EMUL_OP_SONY_PRIME & 0xff,
	0x60, 0x0e,							//  bra		IOReturn

	// Control()
	M68K_EMUL_OP_SONY_CONTROL >> 8, M68K_EMUL_OP_SONY_CONTROL & 0xff,
	0x0c, 0x68, 0x00, 0x01, 0x00, 0x1a,	//  cmp.w	#1,$1a(a0)
	0x66, 0x04,							//  bne		IOReturn
	0x4e, 0x75,							//  rts

	// Status()
	M68K_EMUL_OP_SONY_STATUS >> 8, M68K_EMUL_OP_SONY_STATUS & 0xff,

	// IOReturn
	0x32, 0x28, 0x00, 0x06,				//  move.w	6(a0),d1
	0x08, 0x01, 0x00, 0x09,				//  btst		#9,d1
	0x67, 0x0c,							//  beq		1
	0x4a, 0x40,							//  tst.w	d0
	0x6f, 0x02,							//  ble		2
	0x42, 0x40,							//  clr.w	d0
	0x31, 0x40, 0x00, 0x10,				//2 move.w	d0,$10(a0)
	0x4e, 0x75,							//  rts
	0x4a, 0x40,							//1 tst.w	d0
	0x6f, 0x04,							//  ble		3
	0x42, 0x40,							//  clr.w	d0
	0x4e, 0x75,							//  rts
	0x2f, 0x38, 0x08, 0xfc,				//3 move.l	$8fc,-(sp)
	0x4e, 0x75,							//  rts

	// Close()
	0x70, 0xe8,							//  moveq	#-24,d0
	0x4e, 0x75							//  rts
};

static const uint8 disk_driver[] = {	// Generic disk driver
	// Driver header
	DiskDriverFlags >> 8, DiskDriverFlags & 0xff, 0, 0, 0, 0, 0, 0,
	0x00, 0x18,							// Open() offset
	0x00, 0x1c,							// Prime() offset
	0x00, 0x20,							// Control() offset
	0x00, 0x2c,							// Status() offset
	0x00, 0x52,							// Close() offset
	0x05, 0x2e, 0x44, 0x69, 0x73, 0x6b,	// ".Disk"

	// Open()
	M68K_EMUL_OP_DISK_OPEN >> 8, M68K_EMUL_OP_DISK_OPEN & 0xff,
	0x4e, 0x75,							//  rts

	// Prime()
	M68K_EMUL_OP_DISK_PRIME >> 8, M68K_EMUL_OP_DISK_PRIME & 0xff,
	0x60, 0x0e,							//  bra		IOReturn

	// Control()
	M68K_EMUL_OP_DISK_CONTROL >> 8, M68K_EMUL_OP_DISK_CONTROL & 0xff,
	0x0c, 0x68, 0x00, 0x01, 0x00, 0x1a,	//  cmp.w	#1,$1a(a0)
	0x66, 0x04,							//  bne		IOReturn
	0x4e, 0x75,							//  rts

	// Status()
	M68K_EMUL_OP_DISK_STATUS >> 8, M68K_EMUL_OP_DISK_STATUS & 0xff,

	// IOReturn
	0x32, 0x28, 0x00, 0x06,				//  move.w	6(a0),d1
	0x08, 0x01, 0x00, 0x09,				//  btst		#9,d1
	0x67, 0x0c,							//  beq		1
	0x4a, 0x40,							//  tst.w	d0
	0x6f, 0x02,							//  ble		2
	0x42, 0x40,							//  clr.w	d0
	0x31, 0x40, 0x00, 0x10,				//2 move.w	d0,$10(a0)
	0x4e, 0x75,							//  rts
	0x4a, 0x40,							//1 tst.w	d0
	0x6f, 0x04,							//  ble		3
	0x42, 0x40,							//  clr.w	d0
	0x4e, 0x75,							//  rts
	0x2f, 0x38, 0x08, 0xfc,				//3 move.l	$8fc,-(sp)
	0x4e, 0x75,							//  rts

	// Close()
	0x70, 0xe8,							//  moveq	#-24,d0
	0x4e, 0x75							//  rts
};

#if 0
static const uint8 cdrom_driver[] = {	// CD-ROM driver
	// Driver header
	CDROMDriverFlags >> 8, CDROMDriverFlags & 0xff, 0, 0, 0, 0, 0, 0,
	0x00, 0x1c,							// Open() offset
	0x00, 0x20,							// Prime() offset
	0x00, 0x24,							// Control() offset
	0x00, 0x30,							// Status() offset
	0x00, 0x56,							// Close() offset
	0x08, 0x2e, 0x41, 0x70, 0x70, 0x6c, 0x65, 0x43, 0x44, 0x00,	// ".AppleCD"

	// Open()
	M68K_EMUL_OP_CDROM_OPEN >> 8, M68K_EMUL_OP_CDROM_OPEN & 0xff,
	0x4e, 0x75,							//  rts

	// Prime()
	M68K_EMUL_OP_CDROM_PRIME >> 8, M68K_EMUL_OP_CDROM_PRIME & 0xff,
	0x60, 0x0e,							//  bra		IOReturn

	// Control()
	M68K_EMUL_OP_CDROM_CONTROL >> 8, M68K_EMUL_OP_CDROM_CONTROL & 0xff,
	0x0c, 0x68, 0x00, 0x01, 0x00, 0x1a,	//  cmp.w	#1,$1a(a0)
	0x66, 0x04,							//  bne		IOReturn
	0x4e, 0x75,							//  rts

	// Status()
	M68K_EMUL_OP_CDROM_STATUS >> 8, M68K_EMUL_OP_CDROM_STATUS & 0xff,

	// IOReturn
	0x32, 0x28, 0x00, 0x06,				//  move.w	6(a0),d1
	0x08, 0x01, 0x00, 0x09,				//  btst		#9,d1
	0x67, 0x0c,							//  beq		1
	0x4a, 0x40,							//  tst.w	d0
	0x6f, 0x02,							//  ble		2
	0x42, 0x40,							//  clr.w	d0
	0x31, 0x40, 0x00, 0x10,				//2 move.w	d0,$10(a0)
	0x4e, 0x75,							//  rts
	0x4a, 0x40,							//1 tst.w	d0
	0x6f, 0x04,							//  ble		3
	0x42, 0x40,							//  clr.w	d0
	0x4e, 0x75,							//  rts
	0x2f, 0x38, 0x08, 0xfc,				//3 move.l	$8fc,-(sp)
	0x4e, 0x75,							//  rts

	// Close()
	0x70, 0xe8,							//  moveq	#-24,d0
	0x4e, 0x75							//  rts
};
#endif

static const uint8 ain_driver[] = {	// .AIn driver header
	// Driver header
	0x4d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x18,							// Open() offset
	0x00, 0x1e,							// Prime() offset
	0x00, 0x24,							// Control() offset
	0x00, 0x32,							// Status() offset
	0x00, 0x38,							// Close() offset
	0x04, 0x2e, 0x41, 0x49, 0x6e, 0x09,	// ".AIn",9

	// Open()
	0x70, 0x00,							//  moveq	#0,d0
	M68K_EMUL_OP_SERIAL_OPEN >> 8, M68K_EMUL_OP_SERIAL_OPEN & 0xff,
	0x4e, 0x75,							//	rts

	// Prime()
	0x70, 0x00,							//  moveq	#0,d0
	M68K_EMUL_OP_SERIAL_PRIME >> 8, M68K_EMUL_OP_SERIAL_PRIME & 0xff,
	0x60, 0x1a,							//	bra		IOReturn

	// Control()
	0x70, 0x00,							//  moveq	#0,d0
	M68K_EMUL_OP_SERIAL_CONTROL >> 8, M68K_EMUL_OP_SERIAL_CONTROL & 0xff,
	0x0c, 0x68, 0x00, 0x01, 0x00, 0x1a,	//	cmp.w	#1,$1a(a0)
	0x66, 0x0e,							//	bne		IOReturn
	0x4e, 0x75,							//	rts

	// Status()
	0x70, 0x00,							//  moveq	#0,d0
	M68K_EMUL_OP_SERIAL_STATUS >> 8, M68K_EMUL_OP_SERIAL_STATUS & 0xff,
	0x60, 0x06,							//  bra IOReturn

	// Close()
	0x70, 0x00,							//  moveq	#0,d0
	M68K_EMUL_OP_SERIAL_CLOSE >> 8, M68K_EMUL_OP_SERIAL_CLOSE & 0xff,
	0x4e, 0x75,							//	rts

	// IOReturn
	0x32, 0x28, 0x00, 0x06,				//	move.w	6(a0),d1
	0x08, 0x01, 0x00, 0x09,				//	btst	#9,d1
	0x67, 0x0c,							//	beq		1
	0x4a, 0x40,							//	tst.w	d0
	0x6f, 0x02,							//	ble		2
	0x42, 0x40,							//	clr.w	d0
	0x31, 0x40, 0x00, 0x10,				//2	move.w	d0,$10(a0)
	0x4e, 0x75,							//	rts
	0x4a, 0x40,							//1	tst.w	d0
	0x6f, 0x04,							//	ble		3
	0x42, 0x40,							//	clr.w	d0
	0x4e, 0x75,							//	rts
	0x2f, 0x38, 0x08, 0xfc,				//3	move.l	$8fc,-(a7)
	0x4e, 0x75,							//	rts
};

static const uint8 aout_driver[] = {	// .AOut driver header
	// Driver header
	0x4e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x1a,							// Open() offset
	0x00, 0x20,							// Prime() offset
	0x00, 0x26,							// Control() offset
	0x00, 0x34,							// Status() offset
	0x00, 0x3a,							// Close() offset
	0x05, 0x2e, 0x41, 0x4f, 0x75, 0x74, 0x09, 0x00,		// ".AOut",9

	// Open()
	0x70, 0x01,							//  moveq	#1,d0
	M68K_EMUL_OP_SERIAL_OPEN >> 8, M68K_EMUL_OP_SERIAL_OPEN & 0xff,
	0x4e, 0x75,							//	rts

	// Prime()
	0x70, 0x01,							//  moveq	#1,d0
	M68K_EMUL_OP_SERIAL_PRIME >> 8, M68K_EMUL_OP_SERIAL_PRIME & 0xff,
	0x60, 0x1a,							//	bra		IOReturn

	// Control()
	0x70, 0x01,							//  moveq	#1,d0
	M68K_EMUL_OP_SERIAL_CONTROL >> 8, M68K_EMUL_OP_SERIAL_CONTROL & 0xff,
	0x0c, 0x68, 0x00, 0x01, 0x00, 0x1a,	//	cmp.w	#1,$1a(a0)
	0x66, 0x0e,							//	bne		IOReturn
	0x4e, 0x75,							//	rts

	// Status()
	0x70, 0x01,							//  moveq	#1,d0
	M68K_EMUL_OP_SERIAL_STATUS >> 8, M68K_EMUL_OP_SERIAL_STATUS & 0xff,
	0x60, 0x06,							//  bra IOReturn

	// Close()
	0x70, 0x01,							//  moveq	#1,d0
	M68K_EMUL_OP_SERIAL_CLOSE >> 8, M68K_EMUL_OP_SERIAL_CLOSE & 0xff,
	0x4e, 0x75,							//	rts

	// IOReturn
	0x32, 0x28, 0x00, 0x06,				//	move.w	6(a0),d1
	0x08, 0x01, 0x00, 0x09,				//	btst	#9,d1
	0x67, 0x0c,							//	beq		1
	0x4a, 0x40,							//	tst.w	d0
	0x6f, 0x02,							//	ble		2
	0x42, 0x40,							//	clr.w	d0
	0x31, 0x40, 0x00, 0x10,				//2	move.w	d0,$10(a0)
	0x4e, 0x75,							//	rts
	0x4a, 0x40,							//1	tst.w	d0
	0x6f, 0x04,							//	ble		3
	0x42, 0x40,							//	clr.w	d0
	0x4e, 0x75,							//	rts
	0x2f, 0x38, 0x08, 0xfc,				//3	move.l	$8fc,-(a7)
	0x4e, 0x75,							//	rts
};

static const uint8 bin_driver[] = {	// .BIn driver header
	// Driver header
	0x4d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x18,							// Open() offset
	0x00, 0x1e,							// Prime() offset
	0x00, 0x24,							// Control() offset
	0x00, 0x32,							// Status() offset
	0x00, 0x38,							// Close() offset
	0x04, 0x2e, 0x42, 0x49, 0x6e, 0x09,	// ".BIn",9

	// Open()
	0x70, 0x02,							//  moveq	#2,d0
	M68K_EMUL_OP_SERIAL_OPEN >> 8, M68K_EMUL_OP_SERIAL_OPEN & 0xff,
	0x4e, 0x75,							//	rts

	// Prime()
	0x70, 0x02,							//  moveq	#2,d0
	M68K_EMUL_OP_SERIAL_PRIME >> 8, M68K_EMUL_OP_SERIAL_PRIME & 0xff,
	0x60, 0x1a,							//	bra		IOReturn

	// Control()
	0x70, 0x02,							//  moveq	#2,d0
	M68K_EMUL_OP_SERIAL_CONTROL >> 8, M68K_EMUL_OP_SERIAL_CONTROL & 0xff,
	0x0c, 0x68, 0x00, 0x01, 0x00, 0x1a,	//	cmp.w	#1,$1a(a0)
	0x66, 0x0e,							//	bne		IOReturn
	0x4e, 0x75,							//	rts

	// Status()
	0x70, 0x02,							//  moveq	#2,d0
	M68K_EMUL_OP_SERIAL_STATUS >> 8, M68K_EMUL_OP_SERIAL_STATUS & 0xff,
	0x60, 0x06,							//  bra IOReturn

	// Close()
	0x70, 0x02,							//  moveq	#2,d0
	M68K_EMUL_OP_SERIAL_CLOSE >> 8, M68K_EMUL_OP_SERIAL_CLOSE & 0xff,
	0x4e, 0x75,							//	rts

	// IOReturn
	0x32, 0x28, 0x00, 0x06,				//	move.w	6(a0),d1
	0x08, 0x01, 0x00, 0x09,				//	btst	#9,d1
	0x67, 0x0c,							//	beq		1
	0x4a, 0x40,							//	tst.w	d0
	0x6f, 0x02,							//	ble		2
	0x42, 0x40,							//	clr.w	d0
	0x31, 0x40, 0x00, 0x10,				//2	move.w	d0,$10(a0)
	0x4e, 0x75,							//	rts
	0x4a, 0x40,							//1	tst.w	d0
	0x6f, 0x04,							//	ble		3
	0x42, 0x40,							//	clr.w	d0
	0x4e, 0x75,							//	rts
	0x2f, 0x38, 0x08, 0xfc,				//3	move.l	$8fc,-(a7)
	0x4e, 0x75,							//	rts
};

static const uint8 bout_driver[] = {	// .BOut driver header
	// Driver header
	0x4e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x1a,							// Open() offset
	0x00, 0x20,							// Prime() offset
	0x00, 0x26,							// Control() offset
	0x00, 0x34,							// Status() offset
	0x00, 0x3a,							// Close() offset
	0x05, 0x2e, 0x42, 0x4f, 0x75, 0x74, 0x09, 0x00,		// ".BOut",9

	// Open()
	0x70, 0x03,							//  moveq	#3,d0
	M68K_EMUL_OP_SERIAL_OPEN >> 8, M68K_EMUL_OP_SERIAL_OPEN & 0xff,
	0x4e, 0x75,							//	rts

	// Prime()
	0x70, 0x03,							//  moveq	#3,d0
	M68K_EMUL_OP_SERIAL_PRIME >> 8, M68K_EMUL_OP_SERIAL_PRIME & 0xff,
	0x60, 0x1a,							//	bra		IOReturn

	// Control()
	0x70, 0x03,							//  moveq	#3,d0
	M68K_EMUL_OP_SERIAL_CONTROL >> 8, M68K_EMUL_OP_SERIAL_CONTROL & 0xff,
	0x0c, 0x68, 0x00, 0x01, 0x00, 0x1a,	//	cmp.w	#1,$1a(a0)
	0x66, 0x0e,							//	bne		IOReturn
	0x4e, 0x75,							//	rts

	// Status()
	0x70, 0x03,							//  moveq	#3,d0
	M68K_EMUL_OP_SERIAL_STATUS >> 8, M68K_EMUL_OP_SERIAL_STATUS & 0xff,
	0x60, 0x06,							//  bra IOReturn

	// Close()
	0x70, 0x03,							//  moveq	#3,d0
	M68K_EMUL_OP_SERIAL_CLOSE >> 8, M68K_EMUL_OP_SERIAL_CLOSE & 0xff,
	0x4e, 0x75,							//	rts

	// IOReturn
	0x32, 0x28, 0x00, 0x06,				//	move.w	6(a0),d1
	0x08, 0x01, 0x00, 0x09,				//	btst	#9,d1
	0x67, 0x0c,							//	beq		1
	0x4a, 0x40,							//	tst.w	d0
	0x6f, 0x02,							//	ble		2
	0x42, 0x40,							//	clr.w	d0
	0x31, 0x40, 0x00, 0x10,				//2	move.w	d0,$10(a0)
	0x4e, 0x75,							//	rts
	0x4a, 0x40,							//1	tst.w	d0
	0x6f, 0x04,							//	ble		3
	0x42, 0x40,							//	clr.w	d0
	0x4e, 0x75,							//	rts
	0x2f, 0x38, 0x08, 0xfc,				//3	move.l	$8fc,-(a7)
	0x4e, 0x75,							//	rts
};


/*
 *  ADBOp() patch
 */

static const uint8 adbop_patch[] = {	// Call ADBOp() completion procedure
										// The completion procedure may call ADBOp() again!
	0x40, 0xe7,				//	move	sr,-(sp)
	0x00, 0x7c, 0x07, 0x00,	//	ori		#$0700,sr
	M68K_EMUL_OP_ADBOP >> 8, M68K_EMUL_OP_ADBOP & 0xff,
	0x48, 0xe7, 0x70, 0xf0,	//	movem.l	d1-d3/a0-a3,-(sp)
	0x26, 0x48,				//	move.l	a0,a3
	0x4a, 0xab, 0x00, 0x04,	//	tst.l	4(a3)
	0x67, 0x00, 0x00, 0x18,	//	beq		1
	0x20, 0x53,				//	move.l	(a3),a0
	0x22, 0x6b, 0x00, 0x04,	//	move.l	4(a3),a1
	0x24, 0x6b, 0x00, 0x08,	//	move.l	8(a3),a2
	0x26, 0x78, 0x0c, 0xf8,	//	move.l	$cf8,a3
	0x4e, 0x91,				//	jsr		(a1)
	0x70, 0x00,				//	moveq	#0,d0
	0x60, 0x00, 0x00, 0x04,	//	bra		2
	0x70, 0xff,				//1	moveq	#-1,d0
	0x4c, 0xdf, 0x0f, 0x0e,	//2	movem.l	(sp)+,d1-d3/a0-a3
	0x46, 0xdf,				//	move	(sp)+,sr
	0x4e, 0x75				//	rts
};


/*
 *  Install .Sony, disk and CD-ROM drivers
 */

void InstallDrivers(uint32 pb)
{
	D(bug("InstallDrivers\n"));
	M68kRegisters r;

	// Install Microseconds() replacement routine
	r.a[0] = ROMBaseMac + microseconds_offset;
	r.d[0] = 0xa093;
	Execute68kTrap(0xa247, &r);		// SetOSTrapAddress()

	// Install disk driver
	r.a[0] = ROMBaseMac + sony_offset + 0x100;
	r.d[0] = (uint32)DiskRefNum;
	Execute68kTrap(0xa43d, &r);		// DrvrInstallRsrvMem()
	r.a[0] = ReadMacInt32(ReadMacInt32(0x11c) + ~DiskRefNum * 4);	// Get driver handle from Unit Table
	Execute68kTrap(0xa029, &r);		// HLock()
	uint32 dce = ReadMacInt32(r.a[0]);
	WriteMacInt32(dce + dCtlDriver, ROMBaseMac + sony_offset + 0x100);
	WriteMacInt16(dce + dCtlFlags, DiskDriverFlags);

	// Open disk driver
	WriteMacInt32(pb + ioNamePtr, ROMBaseMac + sony_offset + 0x112);
	r.a[0] = pb;
	Execute68kTrap(0xa000, &r);		// Open()

#if 0
	// Install CD-ROM driver unless nocdrom option given
	if (!PrefsFindBool("nocdrom")) {

		// Install CD-ROM driver
		r.a[0] = ROMBaseMac + sony_offset + 0x200;
		r.d[0] = (uint32)CDROMRefNum;
		Execute68kTrap(0xa43d, &r);		// DrvrInstallRsrvMem()
		r.a[0] = ReadMacInt32(ReadMacInt32(0x11c) + ~CDROMRefNum * 4);	// Get driver handle from Unit Table
		Execute68kTrap(0xa029, &r);		// HLock()
		dce = ReadMacInt32(r.a[0]);
		WriteMacInt32(dce + dCtlDriver, ROMBaseMac + sony_offset + 0x200);
		WriteMacInt16(dce + dCtlFlags, CDROMDriverFlags);

		// Open CD-ROM driver
		WriteMacInt32(pb + ioNamePtr, ROMBaseMac + sony_offset + 0x212);
		r.a[0] = pb;
		Execute68kTrap(0xa000, &r);		// Open()
	}
#endif
}


/*
 *  Install serial drivers
 */

void InstallSERD(void)
{
	D(bug("InstallSERD (skipped, using native hardware SCC emulation)\n"));
}


/*
 *  Install patches after MacOS startup
 */

void PatchAfterStartup(void)
{
#if SUPPORTS_EXTFS
	// Install external file system
	InstallExtFS();
#endif
}


/*
 *  Check ROM version, returns false if the ROM version is not supported
 *
 *  Sets ROMVersion from the version word at ROMBase+8. PatchROM() has code for
 *  exactly two of these, so anything else is rejected here rather than being
 *  accepted and then patched with the wrong offset table.
 *
 *  This used to return ROM_VERSION_CLASSIC (0x0276, and therefore truthy) for
 *  every unrecognised image, so an unknown ROM was accepted and run through
 *  patch_rom_classic() -- which is raw magic offsets with no verification.
 *
 *  Note that the version word does not uniquely identify a ROM image; see the
 *  comment on the ROM_VERSION_* enum in rom_patches.h.
 *
 *  Returns:
 *    true if PatchROM() knows how to patch this ROM.
 */

bool CheckROM(void)
{
	// Read version
	ROMVersion = ntohs(*(uint16 *)(ROMBaseHost + 8));

	switch (ROMVersion) {
		case ROM_VERSION_32:		// 32-bit clean Mac II / Quadra ROMs
		case ROM_VERSION_CLASSIC:	// SE/Classic ROMs
			return true;
		default:
			return false;
	}
}


/*
 *  Install ROM patches, returns false if ROM version is not supported
 */

// ROM patches for Mac Classic/SE ROMs (version $0276)
static bool patch_rom_classic(void)
{
	uint16 *wp;
	uint32 base;
printf("Patching for a Mac Classic/SE (version $0276)\n");

	// Don't jump into debugger (VIA line)
	wp = (uint16 *)(ROMBaseHost + 0x1c40);
	*wp = htons(0x601e);

	// Don't complain about incorrect ROM checksum
	wp = (uint16 *)(ROMBaseHost + 0x1c6c);
	*wp = htons(0x7c00);

	// Don't initialize IWM
	wp = (uint16 *)(ROMBaseHost + 0x50);
	*wp++ = htons(M68K_NOP);
	*wp = htons(M68K_NOP);

	// Skip startup sound
	wp = (uint16 *)(ROMBaseHost + 0x6a);
	*wp++ = htons(M68K_NOP);
	*wp = htons(M68K_NOP);

	// Don't loop in ADB init
	wp = (uint16 *)(ROMBaseHost + 0x3364);
	*wp = htons(M68K_NOP);

	// Patch ClkNoMem
	wp = (uint16 *)(ROMBaseHost + 0xa2c0);
	*wp++ = htons(M68K_EMUL_OP_CLKNOMEM);
	*wp = htons(0x4ed5);			// jmp	(a5)

	// Skip main memory test (not that it wouldn't pass, but it's faster that way)
	wp = (uint16 *)(ROMBaseHost + 0x11e);
	*wp++ = htons(M68K_NOP);
	*wp = htons(M68K_NOP);

	// Install our own drivers
	wp = (uint16 *)(ROMBaseHost + 0x3f82a);
	*wp++ = htons(M68K_EMUL_OP_INSTALL_DRIVERS);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp = htons(M68K_NOP);

#if 1
	// Don't look for SCSI devices
	wp = (uint16 *)(ROMBaseHost + 0xd5a);
	*wp = htons(0x601e);
#endif

	// Replace .Sony driver
	sony_offset = 0x34680;
	D(bug("sony %08lx\n", sony_offset));
	memcpy(ROMBaseHost + sony_offset, sony_driver, sizeof(sony_driver));

	// Install .Disk and .AppleCD drivers
	memcpy(ROMBaseHost + sony_offset + 0x100, disk_driver, sizeof(disk_driver));
	//memcpy(ROMBaseHost + sony_offset + 0x200, cdrom_driver, sizeof(cdrom_driver));

	// Copy icons to ROM
	SonyDiskIconAddr = ROMBaseMac + sony_offset + 0x400;
	memcpy(ROMBaseHost + sony_offset + 0x400, SonyDiskIcon, sizeof(SonyDiskIcon));
	SonyDriveIconAddr = ROMBaseMac + sony_offset + 0x600;
	memcpy(ROMBaseHost + sony_offset + 0x600, SonyDriveIcon, sizeof(SonyDriveIcon));
	DiskIconAddr = ROMBaseMac + sony_offset + 0x800;
	memcpy(ROMBaseHost + sony_offset + 0x800, DiskIcon, sizeof(DiskIcon));
	//CDROMIconAddr = ROMBaseMac + sony_offset + 0xa00;
	//memcpy(ROMBaseHost + sony_offset + 0xa00, CDROMIcon, sizeof(CDROMIcon));

	// Install SERD patch and serial drivers
	if (!PrefsFindBool("ltoudp")) {
		serd_offset = 0x31bae;
		D(bug("serd %08lx\n", serd_offset));
		wp = (uint16 *)(ROMBaseHost + serd_offset + 12);
		*wp++ = htons(M68K_EMUL_OP_SERD);
		*wp = htons(M68K_RTS);
		memcpy(ROMBaseHost + serd_offset + 0x100, ain_driver, sizeof(ain_driver));
		memcpy(ROMBaseHost + serd_offset + 0x200, aout_driver, sizeof(aout_driver));
		memcpy(ROMBaseHost + serd_offset + 0x300, bin_driver, sizeof(bin_driver));
		memcpy(ROMBaseHost + serd_offset + 0x400, bout_driver, sizeof(bout_driver));
	}


	// Replace ADBOp()
	memcpy(ROMBaseHost + 0x3880, adbop_patch, sizeof(adbop_patch));

	// Replace Time Manager
	wp = (uint16 *)(ROMBaseHost + 0x1a95c);
	*wp++ = htons(M68K_EMUL_OP_INSTIME);
	*wp = htons(M68K_RTS);
	wp = (uint16 *)(ROMBaseHost + 0x1a96a);
	*wp++ = htons(0x40e7);		// move	sr,-(sp)
	*wp++ = htons(0x007c);		// ori	#$0700,sr
	*wp++ = htons(0x0700);
	*wp++ = htons(M68K_EMUL_OP_RMVTIME);
	*wp++ = htons(0x46df);		// move	(sp)+,sr
	*wp = htons(M68K_RTS);
	wp = (uint16 *)(ROMBaseHost + 0x1a984);
	*wp++ = htons(0x40e7);		// move	sr,-(sp)
	*wp++ = htons(0x007c);		// ori	#$0700,sr
	*wp++ = htons(0x0700);
	*wp++ = htons(M68K_EMUL_OP_PRIMETIME);
	*wp++ = htons(0x46df);		// move	(sp)+,sr
	*wp++ = htons(M68K_RTS);
	microseconds_offset = (uint8 *)wp - ROMBaseHost;
	*wp++ = htons(M68K_EMUL_OP_MICROSECONDS);
	*wp = htons(M68K_RTS);


	// Replace SCSIDispatch()
	wp = (uint16 *)(ROMBaseHost + 0x1a206);
	*wp++ = htons(M68K_EMUL_OP_SCSI_DISPATCH);
	*wp++ = htons(0x2e49);		// move.l	a1,a7
	*wp = htons(M68K_JMP_A0);


	// Modify vCheckLoad() so we can patch resources
	wp = (uint16 *)(ROMBaseHost + 0xe740);
	*wp++ = htons(M68K_JMP);
	*wp++ = htons((ROMBaseMac + sony_offset + 0x300) >> 16);
	*wp = htons((ROMBaseMac + sony_offset + 0x300) & 0xffff);
	wp = (uint16 *)(ROMBaseHost + sony_offset + 0x300);
	*wp++ = htons(0x2f03);		// move.l	d3,-(sp) (save type)
	*wp++ = htons(0x2078);		// move.l	$07f0,a0
	*wp++ = htons(0x07f0);
	*wp++ = htons(M68K_JSR_A0);
	*wp++ = htons(0x221f);		// move.l	(sp)+,d1 (restore type)
	*wp++ = htons(M68K_EMUL_OP_CHECKLOAD);
	*wp = htons(M68K_RTS);

	// Install PutScrap() patch for clipboard data exchange (the patch is activated by EMUL_OP_INSTALL_DRIVERS)
	PutScrapPatch = ROMBaseMac + sony_offset + 0xc00;
	base = ROMBaseMac + 0x12794;
	wp = (uint16 *)(ROMBaseHost + sony_offset + 0xc00);
	*wp++ = htons(M68K_EMUL_OP_PUT_SCRAP);
	*wp++ = htons(M68K_JMP);
	*wp++ = htons(base >> 16);
	*wp = htons(base & 0xffff);

#if 0
	// Boot from internal EDisk
	wp = (uint16 *)(ROMBaseHost + 0x3f83c);
	*wp = htons(M68K_NOP);
#endif

	// Patch VIA interrupt handler
	wp = (uint16 *)(ROMBaseHost + 0x2b3a);	// Level 1 handler
	*wp++ = htons(0x5888);		// addq.l	#4,a0
	*wp++ = htons(0x5888);		// addq.l	#4,a0
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp = htons(M68K_NOP);

	wp = (uint16 *)(ROMBaseHost + 0x2be4);	// 60Hz handler (handles everything)
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_EMUL_OP_IRQ);
	*wp++ = htons(0x4a80);		// tst.l	d0
	*wp = htons(0x67f4);		// beq		0x402be2
	return true;
}

// ROM patches for 32-bit clean Mac-II ROMs (version $067c)
static bool patch_rom_32(void)
{
	uint32 *lp;
	uint16 *wp;
	uint8 *bp;
	uint32 base;

	printf("Patching a 32-bit clean ROM (version $067c or higher)\n");

	// Find UniversalInfo. The scanned bytes are the tail of a ProductInfo
	// record; UniversalInfo is the record base, 0x10 earlier. Record layout:
	// $SM/Internal/Asm/UniversalEqu.a:519-557, tables in $SM/OS/UniversalTables.a.
	static const uint8 universal_dat[] = {0xdc, 0x00, 0x05, 0x05, 0x3f, 0xff, 0x01, 0x00};
	base = find_rom_data(0x3400, 0x3c00, universal_dat, sizeof(universal_dat));
	if (log_patch("UniversalInfo", "$SM/Internal/Asm/UniversalEqu.a:519", base, true) == 0) return false;
	UniversalInfo = base - 0x10;
	D(bug("universal %08lx\n", UniversalInfo));

	// Patch UniversalInfo (disable NuBus slots). ProductInfo.NuBusInfoPtr is at
	// +12; byte 0 is the slot count/flags, 1..15 the per-slot entries.
	bp = ROMBaseHost + UniversalInfo + ReadMacInt32(ROMBaseMac + UniversalInfo + 12);	// nuBusInfoPtr
	bp[0] = 0x03;
	for (int i=1; i<16; i++)
		bp[i] = 0x08;
	log_patch("UniversalInfo NuBus disable", "$SM/Internal/Asm/UniversalEqu.a:522 NuBusInfoPtr", UniversalInfo + 12, true);

	// Set model ID from preferences. ProductInfo.ProductKind at +18 is the
	// boxFlag value; the names are $SM/Internal/Asm/InternalOnlyEqu.a:625+.
	bp = ROMBaseHost + UniversalInfo + 18;		// productKind
	*bp = PrefsFindInt32("modelid");
	log_patch("UniversalInfo ProductKind", "$SM/Internal/Asm/UniversalEqu.a:526 ProductKind", UniversalInfo + 18, true);

	// Make FPU optional. ProductInfo.DefaultRSRCs at +22 selects the default ROM
	// resource configuration.
	if (FPUType == 0) {
		bp = ROMBaseHost + UniversalInfo + 22;	// defaultRSRCs
		*bp = 4;	// FPU optional
		log_patch("UniversalInfo DefaultRSRCs", "$SM/Internal/Asm/UniversalEqu.a:529 DefaultRSRCs", UniversalInfo + 22, false);
	}

	// Install special reset opcode and jump (skip hardware detection and tests).
	// This replaces the whole StartInit.a hardware bring-up; the individual
	// suppressions below cover routines reached on other paths.
	wp = (uint16 *)(ROMBaseHost + 0x8c);
	*wp++ = htons(M68K_EMUL_OP_RESET);
	*wp++ = htons(M68K_JMP);
	*wp++ = htons((ROMBaseMac + 0xba) >> 16);
	*wp = htons((ROMBaseMac + 0xba) & 0xffff);
	log_patch("RESET trampoline", "$SM/OS/StartMgr/StartInit.a:1139 MyROM", 0x8c, true);

	// Don't GetHardwareInfo
	wp = (uint16 *)(ROMBaseHost + 0xc2);
	*wp++ = htons(M68K_NOP);
	*wp = htons(M68K_NOP);
	log_patch("GetHardwareInfo", "$SM/OS/StartMgr/StartInit.a:1331", 0xc2, true);

	// Don't init VIAs
	wp = (uint16 *)(ROMBaseHost + 0xc6);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp = htons(M68K_NOP);
	log_patch("InitVIAs", "$SM/OS/StartMgr/StartInit.a:1352 / $SM/OS/Universal.a", 0xc6, true);

	// Fake CPU type test
	wp = (uint16 *)(ROMBaseHost + 0x7c0);
	*wp++ = htons(0x7e00 + CPUType);
	*wp = htons(M68K_RTS);
	log_patch("WhichCPU", "$SM/OS/StartMgr/StartInit.a:1367 WhichCPU", 0x7c0, true);

	// Don't clear end of BootGlobs upto end of RAM (address xxxx0000)
	static const uint8 clear_globs_dat[] = {0x42, 0x9a, 0x36, 0x0a, 0x66, 0xfa};
	base = find_rom_data(0xa00, 0xb00, clear_globs_dat, sizeof(clear_globs_dat));
	log_patch("BootGlobs clear loop", "$SM/OS/StartMgr/StartInit.a", base, false);
	D(bug("clear_globs %08lx\n", base));
	if (base) {		// ROM15/20/22/23/26/27/32
		wp = (uint16 *)(ROMBaseHost + base + 2);
		*wp++ = htons(M68K_NOP);
		*wp = htons(M68K_NOP);
	}

	// Patch InitMMU (no MMU present, don't choke on unknown CPU types).
	// InitMMU is $SM/OS/MMU/MMUTables.a, called from StartInit.a:1376.
	if (ROMSize <= 0x80000) {
		static const uint8 init_mmu_dat[] = {0x0c, 0x47, 0x00, 0x03, 0x62, 0x00, 0xfe};
		base = find_rom_data(0x4000, 0x50000, init_mmu_dat, sizeof(init_mmu_dat));
	} else {
		static const uint8 init_mmu_dat[] = {0x0c, 0x47, 0x00, 0x04, 0x62, 0x00, 0xfd};
		base = find_rom_data(0x80000, 0x90000, init_mmu_dat, sizeof(init_mmu_dat));
	}
	if (log_patch("InitMMU CPU-type check", "$SM/OS/StartMgr/StartInit.a:1376 / $SM/OS/MMU/MMUTables.a", base, true) == 0) return false;
	D(bug("init_mmu %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	wp++;
	*wp++ = htons(0x7000);			// moveq #0,d0
	*wp = htons(M68K_NOP);

	// Patch InitMMU (no RBV present)
	static const uint8 init_mmu2_dat[] = {0x08, 0x06, 0x00, 0x0d, 0x67};
	if (ROMSize <= 0x80000) {
		base = find_rom_data(0x4000, 0x50000, init_mmu2_dat, sizeof(init_mmu2_dat));
	} else {
		base = find_rom_data(0x80000, 0x90000, init_mmu2_dat, sizeof(init_mmu2_dat));
	}
	log_patch("InitMMU RBV check", "$SM/OS/MMU/MMU.a", base, false);
	D(bug("init_mmu2 %08lx\n", base));
	if (base) {		// ROM11/10/13/26
		bp = (uint8 *)(ROMBaseHost + base + 4);
		*bp = 0x60;						// bra
	}

	// Patch InitMMU (don't init MMU)
	static const uint8 init_mmu3_dat[] = {0x0c, 0x2e, 0x00, 0x01, 0xff, 0xe6, 0x66, 0x0c, 0x4c, 0xed, 0x03, 0x87, 0xff, 0xe8};
	if (ROMSize <= 0x80000) {
		base = find_rom_data(0x4000, 0x50000, init_mmu3_dat, sizeof(init_mmu3_dat));
	} else {
		base = find_rom_data(0x80000, 0x90000, init_mmu3_dat, sizeof(init_mmu3_dat));
	}
	if (log_patch("InitMMU enable", "$SM/OS/MMU/MMUTables.a", base, true) == 0) return false;
	D(bug("init_mmu3 %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base + 6);
	*wp = htons(M68K_NOP);

	// Replace XPRAM routines. ReadXPram is $SM/OS/Clock.a, reached through the
	// clock/PRAM primitives in $SM/OS/IoPrimitives/ClockPRAMPrimitives.a.
	static const uint8 read_xpram_dat[] = {0x26, 0x4e, 0x41, 0xf9, 0x50, 0xf0, 0x00, 0x00, 0x08, 0x90, 0x00, 0x02};
	base = find_rom_data(0x40000, 0x50000, read_xpram_dat, sizeof(read_xpram_dat));
	log_patch("ReadXPRAM (ROM10)", "$SM/OS/Clock.a", base, false);
	D(bug("read_xpram %08lx\n", base));
	if (base) {			// ROM10
		wp = (uint16 *)(ROMBaseHost + base);
		*wp++ = htons(M68K_EMUL_OP_READ_XPRAM);
		*wp = htons(0x4ed6);		// jmp	(a6)
	}
	static const uint8 read_xpram2_dat[] = {0x26, 0x4e, 0x08, 0x92, 0x00, 0x02, 0xea, 0x59, 0x02, 0x01, 0x00, 0x07, 0x00, 0x01, 0x00, 0xb8};
	base = find_rom_data(0x40000, 0x50000, read_xpram2_dat, sizeof(read_xpram2_dat));
	log_patch("ReadXPRAM (ROM11)", "$SM/OS/Clock.a", base, false);
	D(bug("read_xpram2 %08lx\n", base));
	if (base) {			// ROM11
		wp = (uint16 *)(ROMBaseHost + base);
		*wp++ = htons(M68K_EMUL_OP_READ_XPRAM);
		*wp = htons(0x4ed6);		// jmp	(a6)
	}
	if (ROMSize > 0x80000) {
		static const uint8 read_xpram3_dat[] = {0x48, 0xe7, 0xe0, 0x60, 0x02, 0x01, 0x00, 0x70, 0x0c, 0x01, 0x00, 0x20};
		base = find_rom_data(0x80000, 0x90000, read_xpram3_dat, sizeof(read_xpram3_dat));
		log_patch("ReadXPRAM (ROM15)", "$SM/OS/Clock.a", base, false);
		D(bug("read_xpram3 %08lx\n", base));
		if (base) {		// ROM15
			wp = (uint16 *)(ROMBaseHost + base);
			*wp++ = htons(M68K_EMUL_OP_READ_XPRAM2);
			*wp = htons(M68K_RTS);
		}
	}

	// Patch ClkNoMem ($A053). Trap wiring: $SM/OS/DispTable.a; implementation
	// $SM/OS/Clock.a. On ROM23/26/27/32 the trap entry is a jmp (a5) stub, so
	// re-locate the real body by signature.
	base = find_rom_trap(0xa053);
	wp = (uint16 *)(ROMBaseHost + base);
	if (ntohs(*wp) == 0x4ed5) {	// ROM23/26/27/32
		static const uint8 clk_no_mem_dat[] = {0x40, 0xc2, 0x00, 0x7c, 0x07, 0x00, 0x48, 0x42};
		base = find_rom_data(0xb0000, 0xb8000, clk_no_mem_dat, sizeof(clk_no_mem_dat));
	}
	if (log_patch("ClkNoMem ($A053)", "$SM/OS/Clock.a", base, true) == 0) return false;
	D(bug("clk_no_mem %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp++ = htons(M68K_EMUL_OP_CLKNOMEM);
	*wp = htons(0x4ed5);			// jmp	(a5)

	// Patch BootGlobs
	wp = (uint16 *)(ROMBaseHost + 0x10e);
	*wp++ = htons(M68K_EMUL_OP_PATCH_BOOT_GLOBS);
	*wp = htons(M68K_NOP);
	log_patch("BootGlobs", "$SM/Internal/Asm/BootEqu.a", 0x10e, true);

	// Don't init SCC. InitSCC is exported from StartInit.a:1146, called at :1553.
	static const uint8 init_scc_dat[] = {0x08, 0x38, 0x00, 0x01, 0x0d, 0xd1, 0x67, 0x04};
	base = find_rom_data(0xa00, 0xa80, init_scc_dat, sizeof(init_scc_dat));
	if (log_patch("InitSCC", "$SM/OS/StartMgr/StartInit.a:1553", base, true) == 0) return false;
	D(bug("init_scc %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp = htons(M68K_RTS);

	// Don't access 0x50f1a101
	wp = (uint16 *)(ROMBaseHost + 0x4232);
	if (ntohs(wp[1]) == 0x50f1 && ntohs(wp[2]) == 0xa101) {	// ROM32
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp = htons(M68K_NOP);
	}

	// Don't init IWM. InitIWM is $SM/Drivers/Sony/SonyMFM.a, called at
	// StartInit.a:1556.
	wp = (uint16 *)(ROMBaseHost + 0x9c0);
	*wp = htons(M68K_RTS);
	log_patch("InitIWM", "$SM/OS/StartMgr/StartInit.a:1556", 0x9c0, true);


	// Don't init SCSI. InitSCSIMgr is $SM/OS/SCSIMgr/SCSIMgrInit.a:161,
	// called at StartInit.a:1758.
	wp = (uint16 *)(ROMBaseHost + 0x9a0);
	*wp = htons(M68K_RTS);
	log_patch("InitSCSIMgr", "$SM/OS/SCSIMgr/SCSIMgrInit.a:161", 0x9a0, true);


	// Don't init ASC
	static const uint8 init_asc_dat[] = {0x26, 0x68, 0x00, 0x30, 0x12, 0x00, 0xeb, 0x01};
	base = find_rom_data(0x4000, 0x5000, init_asc_dat, sizeof(init_asc_dat));
	log_patch("InitSndHW (ASC)", "$SM/OS/StartMgr/StartInit.a:1563 InitSndHW", base, false);
	D(bug("init_asc %08lx\n", base));
	if (base) {		// ROM15/22/23/26/27/32
		wp = (uint16 *)(ROMBaseHost + base);
		*wp = htons(0x4ed6);		// jmp	(a6)
	}

	// Don't EnableExtCache. $SM/OS/HwPriv.a, called at StartInit.a:1604.
	wp = (uint16 *)(ROMBaseHost + 0x190);
	*wp++ = htons(M68K_NOP);
	*wp = htons(M68K_NOP);
	log_patch("EnableExtCache", "$SM/OS/StartMgr/StartInit.a:1604 / $SM/OS/HwPriv.a", 0x190, true);

	// Don't DisableIntSources. $SM/OS/InterruptHandlers.a, called at
	// StartInit.a:1606 and again at :1633.
	wp = (uint16 *)(ROMBaseHost + 0x9f4c);
	*wp = htons(M68K_RTS);
	log_patch("DisableIntSources", "$SM/OS/InterruptHandlers.a", 0x9f4c, true);

	// Fake CPU speed test (SetupTimeK)
	wp = (uint16 *)(ROMBaseHost + 0x800);
	*wp++ = htons(0x31fc);			// move.w	#xxx,TimeDBRA
	*wp++ = htons(100);
	*wp++ = htons(0x0d00);
	*wp++ = htons(0x31fc);			// move.w	#xxx,TimeSCCDBRA
	*wp++ = htons(100);
	*wp++ = htons(0x0d02);
	*wp++ = htons(0x31fc);			// move.w	#xxx,TimeSCSIDBRA
	*wp++ = htons(100);
	*wp++ = htons(0x0b24);
	*wp++ = htons(0x31fc);			// move.w	#xxx,TimeRAMDBRA
	*wp++ = htons(100);
	*wp++ = htons(0x0cea);
	*wp = htons(M68K_RTS);
	// SetUpTimeK is StartInit.a:2032, driven by TimingTable at :2151 (TimeDBRA,
	// TimeSCCDB, TimeSCSIDB). Leaving these zero is the UAE Type 4 zero-divide.
	log_patch("SetUpTimeK", "$SM/OS/StartMgr/StartInit.a:2032", 0x800, true);

#if REAL_ADDRESSING
	// Move system zone to start of Mac RAM
	lp = (uint32 *)(ROMBaseHost + 0x50a);
	*lp++ = htonl(RAMBaseMac);
	*lp = htonl(RAMBaseMac + 0x1800);
#endif

#if !ROM_IS_WRITE_PROTECTED
#if defined(AMIGA)
	// Set fake handle at 0x0000 to scratch memory area (so broken Mac programs won't write into Mac ROM)
	extern uint32 ScratchMem;
	wp = (uint16 *)(ROMBaseHost + 0xccaa);
	*wp++ = htons(0x203c);			// move.l	#ScratchMem,d0
	*wp++ = htons(ScratchMem >> 16);
	*wp = htons(ScratchMem);
#else
#error System specific handling for writable ROM is required here
#endif
#endif

#if REAL_ADDRESSING && defined(AMIGA)
	// Don't overwrite SysBase under AmigaOS
	wp = (uint16 *)(ROMBaseHost + 0xccb4);
	*wp++ = htons(M68K_NOP);
	*wp = htons(M68K_NOP);
#endif

	// Don't write to VIA in InitTimeMgr. $SM/OS/TimeMgr/TimeMgr.a, called at
	// StartInit.a:1689.
	wp = (uint16 *)(ROMBaseHost + 0xb0e2);
	*wp++ = htons(0x4cdf);			// movem.l	(sp)+,d0-d5/a0-a4
	*wp++ = htons(0x1f3f);
	*wp = htons(M68K_RTS);
	log_patch("InitTimeMgr VIA write", "$SM/OS/TimeMgr/TimeMgr.a", 0xb0e2, true);

	// Don't read ModelID from 0x5ffffffc
	static const uint8 model_id_dat[] = {0x20, 0x7c, 0x5f, 0xff, 0xff, 0xfc, 0x72, 0x07, 0xc2, 0x90};
	base = find_rom_data(0x40000, 0x50000, model_id_dat, sizeof(model_id_dat));
	log_patch("ModelID read (ROM20)", "$SM/OS/Universal.a GetCPUIDReg", base, false);
	D(bug("model_id %08lx\n", base));
	if (base) {		// ROM20
		wp = (uint16 *)(ROMBaseHost + base + 8);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp = htons(M68K_NOP);
	}

	// Don't read ModelID from 0x5ffffffc
	static const uint8 model_id2_dat[] = {0x45, 0xf9, 0x5f, 0xff, 0xff, 0xfc, 0x20, 0x12};
	base = find_rom_data(0x4000, 0x5000, model_id2_dat, sizeof(model_id2_dat));
	log_patch("ModelID read (ROM27/32)", "$SM/OS/Universal.a GetCPUIDReg", base, false);
	D(bug("model_id2 %08lx\n", base));
	if (base) {		// ROM27/32
		wp = (uint16 *)(ROMBaseHost + base + 6);
		*wp++ = htons(0x7000);	// moveq	#0,d0
		*wp++ = htons(0xb040);	// cmp.w	d0,d0
		*wp = htons(0x4ed6);	// jmp		(a6)
	}

	// Install slot ROM. Real declaration ROMs are $SM/DeclData/DeclData.r; the
	// Slot Manager that walks them is $SM/OS/SlotMgr/SlotMgr.a.
	if (!InstallSlotROM())
		return false;
	log_patch("InstallSlotROM", "$SM/DeclData/DeclData.r", SlotROMOffset, true);

	// Don't probe NuBus slots
	static const uint8 nubus_dat[] = {0x45, 0xfa, 0x00, 0x0a, 0x42, 0xa7, 0x10, 0x11};
	base = find_rom_data(0x5000, 0x6000, nubus_dat, sizeof(nubus_dat));
	log_patch("NuBus slot probe", "$SM/OS/SlotMgr/SlotMgrInit.a", base, false);
	D(bug("nubus %08lx\n", base));
	if (base) {		// ROM10/11
		wp = (uint16 *)(ROMBaseHost + base + 6);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp = htons(M68K_NOP);
	}

	// Don't EnableOneSecInts
	static const uint8 lea_dat[] = {0x41, 0xf9};
	base = find_rom_data(0x226, 0x22a, lea_dat, sizeof(lea_dat));
	if (log_patch("EnableOneSecInts", "$SM/OS/InterruptHandlers.a:590", base, true) == 0) return false;
	D(bug("enable_one_sec_ints %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp = htons(M68K_NOP);

	// Don't EnableParityPatch/Enable60HzInts
	if ((base = find_rom_data(0x230, 0x234, lea_dat, sizeof(lea_dat))) == 0) {
		wp = (uint16 *)(ROMBaseHost + 0x230);
		if (ntohs(*wp) == 0x6100)	// ROM11
			base = 0x230;
		else
			return false;
	}
	log_patch("Enable60HzInts", "$SM/OS/InterruptHandlers.a:549", base, true);
	D(bug("enable_60hz_ints %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp = htons(M68K_NOP);

	// Compute boot stack pointer and fix logical/physical RAM size (CompBootStack) (must be done after InitMemMgr!)
	wp = (uint16 *)(ROMBaseHost + 0x490);
	*wp++ = htons(0x2038);	// move.l	$10c,d0
	*wp++ = htons(0x010c);
	*wp++ = htons(0xd0b8);	// add.l	$2a6,d0
	*wp++ = htons(0x02a6);
	*wp++ = htons(0xe288);	// lsr.l	#1,d0
	*wp++ = htons(0x0880);	// bclr		#0,d0
	*wp++ = htons(0x0000);
	*wp++ = htons(0x0440);	// subi.w	#$400,d0
	*wp++ = htons(0x0400);
	*wp++ = htons(0x2040);	// move.l	d0,a0
	*wp++ = htons(M68K_EMUL_OP_FIX_MEMSIZE);
	*wp++ = htons(M68K_RTS);
	log_patch("CompBootStack", "$SM/OS/StartMgr/StartInit.a:1642", 0x490, true);

	static const uint8 fix_memsize2_dat[] = {0x22, 0x30, 0x81, 0xe2, 0x0d, 0xdc, 0xff, 0xba, 0xd2, 0xb0, 0x81, 0xe2, 0x0d, 0xdc, 0xff, 0xec, 0x21, 0xc1, 0x1e, 0xf8};
	base = find_rom_data(0x4c000, 0x4c080, fix_memsize2_dat, sizeof(fix_memsize2_dat));
	log_patch("RAM size fixup", "$SM/OS/StartMgr/SizeMem.a", base, false);
	D(bug("fix_memsize2 %08lx\n", base));
	if (base) {		// ROM15/22/23/26/27/32
		wp = (uint16 *)(ROMBaseHost + base + 16);
		*wp++ = htons(M68K_NOP);
		*wp = htons(M68K_NOP);
	}

	// Don't open .Sound driver but install our own drivers. The Sound Manager
	// has no SuperMario source (ships as SoundMgr.rsrc), so this site is
	// anchored only to the ROM image.
	wp = (uint16 *)(ROMBaseHost + 0x1142);
	*wp = htons(M68K_EMUL_OP_INSTALL_DRIVERS);
	log_patch(".Sound open hook", NULL, 0x1142, true);

	// Don't access SonyVars ($SM/Drivers/Sony/Sony.a driver globals)
	log_patch("SonyVars access", "$SM/Drivers/Sony/Sony.a", 0x1144, true);
	wp = (uint16 *)(ROMBaseHost + 0x1144);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	wp += 2;
	*wp = htons(M68K_NOP);

	// Don't write to VIA in InitADB. The spin being removed is the state-3 wait
	// at $SM/OS/ADBMgr/ADBMgrPatch.a:159-174 (movea.l VIA,a1 / move.b vBufB(a1),d0
	// / andi.b #$30,d0 / cmpi.b #$30,d0 / bne.s @wait). InitADB itself is
	// $SM/OS/ADBMgr/ADBMgr.a, called at StartInit.a:1765.
	wp = (uint16 *)(ROMBaseHost + 0xa8a8);
	if (*wp == 0) {		// ROM22/23/26/27/32
		log_patch("InitADB VIA wait (ROM22+)", "$SM/OS/ADBMgr/ADBMgr.a", 0xb2c6a, true);
		wp = (uint16 *)(ROMBaseHost + 0xb2c6a);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp = htons(M68K_NOP);
		wp = (uint16 *)(ROMBaseHost + 0xb2d2e);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		wp += 2;
		*wp++ = htons(M68K_NOP);
		*wp = htons(M68K_NOP);
	} else {
		log_patch("InitADB VIA wait", "$SM/OS/ADBMgr/ADBMgr.a", 0xa8a8, true);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp = htons(M68K_NOP);
		wp = (uint16 *)(ROMBaseHost + 0xa662);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		wp += 2;
		*wp++ = htons(M68K_NOP);
		*wp = htons(M68K_NOP);
	}

	// Don't EnableSlotInts
	base = find_rom_data(0x2ee, 0x2f2, lea_dat, sizeof(lea_dat));
	if (log_patch("EnableSlotInts", "$SM/OS/InterruptHandlers.a:612", base, true) == 0) return false;
	D(bug("enable_slot_ints %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp = htons(M68K_NOP);

	// Don't mangle frame buffer base (GetDevBase). Video sResources and the
	// built-in video drivers are $SM/DeclData/DeclVideo/.
	wp = (uint16 *)(ROMBaseHost + 0x5b78);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(0x2401);		// move.l	d1,d2
	*wp = htons(0x605e);		// bra		0x40805bde
	log_patch("GetDevBase", "$SM/DeclData/DeclVideo/", 0x5b78, true);

	// Really don't mangle frame buffer base
	if (ROMSize > 0x80000) {
		static const uint8 frame_base_dat[] = {0x22, 0x78, 0x0d, 0xd8, 0xd3, 0xe9, 0x00, 0x08};
		base = find_rom_data(0x8c000, 0x8d000, frame_base_dat, sizeof(frame_base_dat));
		log_patch("GetDevBase (ROM22+)", "$SM/DeclData/DeclVideo/", base, false);
		D(bug("frame_base %08lx\n", base));
		if (base) {		// ROM22/23/26/27/32
			wp = (uint16 *)(ROMBaseHost + base);
			*wp++ = htons(0x2401);	// move.l	d1,d2
			*wp = htons(M68K_RTS);
		}
	}

	// Don't write to VIA2
	static const uint8 via2_dat[] = {0x20, 0x78, 0x0c, 0xec, 0x11, 0x7c, 0x00, 0x90};
	base = find_rom_data(0xa000, 0xa400, via2_dat, sizeof(via2_dat));
	if (log_patch("VIA2 write", "$SM/Internal/Asm/HardwarePrivateEqu.a", base, true) == 0) return false;
	D(bug("via2 %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base + 4);
	*wp = htons(M68K_RTS);

	// Don't write to VIA2, even on ROM20
	static const uint8 via2b_dat[] = {0x20, 0x78, 0x0c, 0xec, 0x11, 0x7c, 0x00, 0x90, 0x00, 0x13, 0x4e, 0x75};
	base = find_rom_data(0x40000, 0x44000, via2b_dat, sizeof(via2b_dat));
	log_patch("VIA2 write (ROM19/20)", "$SM/Internal/Asm/HardwarePrivateEqu.a", base, false);
	D(bug("via2b %08lx\n", base));
	if (base) {		// ROM19/20
		wp = (uint16 *)(ROMBaseHost + base + 4);
		*wp = htons(M68K_RTS);
	}

	// Don't use PTEST instruction on 68040/060
	if (ROMSize > 0x80000) {

		// BlockMove() — NOP PTEST/cpusha tail; trap 0xA02E is replaced by EMUL_OP below.
		static const uint8 ptest_dat[] = {0xa0, 0x8d, 0x0c, 0x81, 0x00, 0x00, 0x0c, 0x00, 0x6d, 0x06, 0x4e, 0x71, 0xf4, 0xf8};
		base = find_rom_data(0x87000, 0x87800, ptest_dat, sizeof(ptest_dat));
		log_patch("BlockMove PTEST", "$SM/OS/MemoryMgr/BlockMove.a:435", base, false);
		D(bug("ptest %08lx\n", base));
		if (base) {		// ROM15/22/23/26/27/32
			wp = (uint16 *)(ROMBaseHost + base + 8);
			*wp = htons(M68K_NOP);
		}
#if 1		//Why do we have to mess with SANE?
		//I'm going to guess it has to do when the FPU
		//is enabled.
		// SANE
	if(FPUType==1) {
		static const uint8 ptest2_dat[] = {0x0c, 0x38, 0x00, 0x04, 0x01, 0x2f, 0x6d, 0x54, 0x48, 0xe7, 0xf8, 0x60};
		base = find_rom_data(0, ROMSize, ptest2_dat, sizeof(ptest2_dat));
		log_patch("SANE PTEST", "$SM/Toolbox/SANE/", base, false);
		D(bug("ptest2 %08lx\n", base));
		if (base) {		// ROM15/20/22/23/26/27/32
			wp = (uint16 *)(ROMBaseHost + base + 8);
			*wp++ = htons(M68K_NOP);
			*wp++ = htons(0xf4f8);		// cpusha	dc/ic
			*wp++ = htons(M68K_NOP);
			*wp++ = htons(0x7000);		// moveq	#0,d0
			*wp = htons(M68K_RTS);
		}
	   }//end FPU
#endif
	}

	// Don't set MemoryDispatch() to unimplemented trap
	static const uint8 memdisp_dat[] = {0x30, 0x3c, 0xa8, 0x9f, 0xa7, 0x46, 0x30, 0x3c, 0xa0, 0x5c, 0xa2, 0x47};
	base = find_rom_data(0x4f100, 0x4f180, memdisp_dat, sizeof(memdisp_dat));
	log_patch("MemoryDispatch", "$SM/OS/MemoryMgr/MemoryMgrExtensions.a", base, false);
	D(bug("memdisp %08lx\n", base));
	if (base) {	// ROM15/22/23/26/27/32
		wp = (uint16 *)(ROMBaseHost + base + 10);
		*wp = htons(M68K_NOP);
	}

	// Patch .EDisk driver (don't scan for EDisks in the area ROMBase..0xe00000)
	uint32 edisk_offset = locate_rom_resource('DRVR', 51, ".EDisk driver (DRVR 51)", "$SM/Drivers/EDisk/EDiskDriver.a", false);
	if (edisk_offset) {
		static const uint8 edisk_dat[] = {0xd5, 0xfc, 0x00, 0x01, 0x00, 0x00, 0xb5, 0xfc, 0x00, 0xe0, 0x00, 0x00};
		base = find_rom_data(edisk_offset, edisk_offset + 0x10000, edisk_dat, sizeof(edisk_dat));
		log_patch(".EDisk ROM scan limit", "$SM/Drivers/EDisk/EDiskDriver.a", base, false);
		D(bug("edisk %08lx\n", base));
		if (base) {
			wp = (uint16 *)(ROMBaseHost + base + 8);
			*wp++ = 0;
			*wp = 0;
		}
	}

	// Replace .Sony driver. Real driver: $SM/Drivers/Sony/Sony.a (DiskOpen :253,
	// DiskPrime jump table :199, CtlTbl :495).
	sony_offset = locate_rom_resource('DRVR', 4, ".Sony driver (DRVR 4)", "$SM/Drivers/Sony/Sony.a", true);
	if (sony_offset == 0) return false;
	// Everything below treats the .Sony resource as scratch space out to +0xc10
	// (.Disk driver, icons, vCheckLoad stub, PutScrap trampoline). Refuse rather
	// than run off the end of the image.
	if (sony_offset + 0xc10 > ROMSize) {
		printf("[ROM-PATCH] .Sony driver at %06x leaves too little room (need 0xc10, ROM is %06x)\n",
		       sony_offset, ROMSize);
		return false;
	}
	D(bug("sony %08lx\n", sony_offset));
	memcpy(ROMBaseHost + sony_offset, sony_driver, sizeof(sony_driver));

	// Install .Disk and .AppleCD drivers
	memcpy(ROMBaseHost + sony_offset + 0x100, disk_driver, sizeof(disk_driver));
	//memcpy(ROMBaseHost + sony_offset + 0x200, cdrom_driver, sizeof(cdrom_driver));

	// Copy icons to ROM
	SonyDiskIconAddr = ROMBaseMac + sony_offset + 0x400;
	memcpy(ROMBaseHost + sony_offset + 0x400, SonyDiskIcon, sizeof(SonyDiskIcon));
	SonyDriveIconAddr = ROMBaseMac + sony_offset + 0x600;
	memcpy(ROMBaseHost + sony_offset + 0x600, SonyDriveIcon, sizeof(SonyDriveIcon));
	DiskIconAddr = ROMBaseMac + sony_offset + 0x800;
	memcpy(ROMBaseHost + sony_offset + 0x800, DiskIcon, sizeof(DiskIcon));
	//CDROMIconAddr = ROMBaseMac + sony_offset + 0xa00;
	//memcpy(ROMBaseHost + sony_offset + 0xa00, CDROMIcon, sizeof(CDROMIcon));

	// Install SERD patch and serial drivers
	if (!PrefsFindBool("ltoudp")) {
		// No SuperMario source: the serial driver ships as Serial.rsrc
		// (see docs/rom-patches-vs-supermario.md section 6).
		serd_offset = locate_rom_resource('SERD', 0, "SERD 0 + serial drivers", NULL, true);
		if (serd_offset == 0) return false;
		D(bug("serd %08lx\n", serd_offset));
		wp = (uint16 *)(ROMBaseHost + serd_offset + 12);
		*wp++ = htons(M68K_EMUL_OP_SERD);
		*wp = htons(M68K_RTS);
		memcpy(ROMBaseHost + serd_offset + 0x100, ain_driver, sizeof(ain_driver));
		memcpy(ROMBaseHost + serd_offset + 0x200, aout_driver, sizeof(aout_driver));
		memcpy(ROMBaseHost + serd_offset + 0x300, bin_driver, sizeof(bin_driver));
		memcpy(ROMBaseHost + serd_offset + 0x400, bout_driver, sizeof(bout_driver));
	}

	// Replace ADBOp() ($A07C). Trap wiring $SM/OS/DispTable.a:1428
	// "OS $7C,ADBOpTrap"; body $SM/OS/ADBMgr/ADBMgr.a:337.
	uint32 trap_adbop = require_rom_trap(0xa07c, "ADBOp ($A07C)", "$SM/OS/ADBMgr/ADBMgr.a:337");
	if (trap_adbop == 0) return false;
	memcpy(ROMBaseHost + trap_adbop, adbop_patch, sizeof(adbop_patch));

	// Replace Time Manager (the Microseconds patch is activated in InstallDrivers()).
	// Trap wiring $SM/OS/DispTable.a; bodies $SM/OS/TimeMgr/TimeMgr.a. The
	// sr save / ori #$0700,sr wrapper mirrors what Apple's own Time Manager
	// swap does ($SM/OS/TimeMgr/TimeMgrPatch.a:186-187).
	uint32 trap_instime = require_rom_trap(0xa058, "InsTime ($A058)", "$SM/OS/TimeMgr/TimeMgr.a");
	if (trap_instime == 0) return false;
	wp = (uint16 *)(ROMBaseHost + trap_instime);
	*wp++ = htons(M68K_EMUL_OP_INSTIME);
	*wp = htons(M68K_RTS);
	uint32 trap_rmvtime = require_rom_trap(0xa059, "RmvTime ($A059)", "$SM/OS/TimeMgr/TimeMgr.a");
	if (trap_rmvtime == 0) return false;
	wp = (uint16 *)(ROMBaseHost + trap_rmvtime);
	*wp++ = htons(0x40e7);		// move	sr,-(sp)
	*wp++ = htons(0x007c);		// ori	#$0700,sr
	*wp++ = htons(0x0700);
	*wp++ = htons(M68K_EMUL_OP_RMVTIME);
	*wp++ = htons(0x46df);		// move	(sp)+,sr
	*wp = htons(M68K_RTS);
	uint32 trap_primetime = require_rom_trap(0xa05a, "PrimeTime ($A05A)", "$SM/OS/TimeMgr/TimeMgr.a:482");
	if (trap_primetime == 0) return false;
	wp = (uint16 *)(ROMBaseHost + trap_primetime);
	*wp++ = htons(0x40e7);		// move	sr,-(sp)
	*wp++ = htons(0x007c);		// ori	#$0700,sr
	*wp++ = htons(0x0700);
	*wp++ = htons(M68K_EMUL_OP_PRIMETIME);
	*wp++ = htons(0x46df);		// move	(sp)+,sr
	*wp++ = htons(M68K_RTS);
	// Microseconds stub goes in the spare bytes right after PrimeTime. ABI is
	// A0 = high, D0 = low ($SM/OS/TimeMgr/TimeMgr.a:741), NOT an UnsignedWide*
	// through A0 -- see docs/quadra-32bit-boot-crashes.md.
	microseconds_offset = (uint8 *)wp - ROMBaseHost;
	log_patch("Microseconds ($A093)", "$SM/OS/TimeMgr/TimeMgr.a:741", microseconds_offset, true);
	*wp++ = htons(M68K_EMUL_OP_MICROSECONDS);
	*wp = htons(M68K_RTS);

	// Replace SCSIDispatch() ($A815). $SM/OS/DispTable.a:377
	// "ToolBox $015,SCSIDispatchCommon"; body $SM/OS/SCSIMgr/SCSILinkPatch.a:221.
	uint32 trap_scsi = require_rom_trap(0xa815, "SCSIDispatch ($A815)", "$SM/OS/SCSIMgr/SCSILinkPatch.a:221");
	if (trap_scsi == 0) return false;
	wp = (uint16 *)(ROMBaseHost + trap_scsi);
	*wp++ = htons(M68K_EMUL_OP_SCSI_DISPATCH);
	*wp++ = htons(0x2e49);		// move.l	a1,a7
	*wp = htons(M68K_JMP_A0);

	// Modify vCheckLoad() so we can patch resources.
	//
	// Apple's own way to hook this is the jCheckLoad vector at $07F0
	// ($SM/Interfaces/AIncludes/Private.a:386; install idiom
	// $SM/Patches/BeforePatches.a:690-697) -- which is what the stub below calls
	// through. We reach it by byte-patching the ROM instead; see
	// docs/rom-patches-vs-supermario.md section 3.1.
	log_patch("vCheckLoad hook", "$SM/Patches/BeforePatches.a:690", 0x1b8f4, true);
	wp = (uint16 *)(ROMBaseHost + 0x1b8f4);
	*wp++ = htons(M68K_JMP);
	*wp++ = htons((ROMBaseMac + sony_offset + 0x300) >> 16);
	*wp = htons((ROMBaseMac + sony_offset + 0x300) & 0xffff);
	wp = (uint16 *)(ROMBaseHost + sony_offset + 0x300);
	*wp++ = htons(0x2f03);		// move.l	d3,-(sp) (save type)
	*wp++ = htons(0x2078);		// move.l	$07f0,a0
	*wp++ = htons(0x07f0);
	*wp++ = htons(M68K_JSR_A0);
	*wp++ = htons(0x221f);		// move.l	(sp)+,d1 (restore type)
	*wp++ = htons(M68K_EMUL_OP_CHECKLOAD);
	*wp = htons(M68K_RTS);

	// Patch PowerOff() ($A05B). $SM/Toolbox/ShutDownMgr/ShutDownMgr.a
	uint32 trap_poweroff = require_rom_trap(0xa05b, "PowerOff ($A05B)", "$SM/Toolbox/ShutDownMgr/ShutDownMgr.a");
	if (trap_poweroff == 0) return false;
	wp = (uint16 *)(ROMBaseHost + trap_poweroff);	// PowerOff()
	*wp = htons(M68K_EMUL_OP_SHUTDOWN);

	// Install PutScrap() patch for clipboard data exchange (the patch is activated by EMUL_OP_INSTALL_DRIVERS)
	uint32 trap_putscrap = require_rom_trap(0xa9fe, "PutScrap ($A9FE)", "$SM/Toolbox/ScrapMgr/");
	if (trap_putscrap == 0) return false;
	PutScrapPatch = ROMBaseMac + sony_offset + 0xc00;
	base = ROMBaseMac + trap_putscrap;
	wp = (uint16 *)(ROMBaseHost + sony_offset + 0xc00);
	*wp++ = htons(M68K_EMUL_OP_PUT_SCRAP);
	*wp++ = htons(M68K_JMP);
	*wp++ = htons(base >> 16);
	*wp = htons(base & 0xffff);

#if EMULATED_68K
	// Replace BlockMove() ($A02E). $SM/OS/MemoryMgr/BlockMove.a
	uint32 trap_blockmove = require_rom_trap(0xa02e, "BlockMove ($A02E)", "$SM/OS/MemoryMgr/BlockMove.a");
	if (trap_blockmove == 0) return false;
	wp = (uint16 *)(ROMBaseHost + trap_blockmove);	// BlockMove()
	*wp++ = htons(M68K_EMUL_OP_BLOCK_MOVE);
	*wp++ = htons(0x7000);
	*wp = htons(M68K_RTS);
#endif

	// Look for double PACK 4 resources (SANE; $SM/Resources/RomResources.r
	// 'rrsc' 120/130/140)
	base = locate_rom_resource('PACK', 4, "PACK 4 (SANE)", "$SM/Resources/RomResources.r", true);
	if (base == 0) return false;
	if ((base = find_rom_resource('PACK', 4, true)) == 0 && FPUType == 0)
		printf("WARNING: This ROM seems to require an FPU\n");

	// Patch VIA interrupt handler.
	//
	// The level-1 secondary dispatcher (Level1Via1Int,
	// $SM/OS/InterruptHandlers.a:1558) selects a slot in Via1DT == Lvl1DT ($192):
	// jOneSecInt = +4*ifCA2, jVBLInt = +4*ifCA1, jKbdAdbInt = +4*ifSR
	// ($SM/OS/InterruptHandlers.a:489-496). Forcing "moveq #2,d0" here pins it to
	// one slot, so one-second and ADB shift-register interrupts cannot be
	// delivered on their own vectors. See docs/rom-patches-vs-supermario.md 3.2.
	log_patch("VIA level-1 dispatcher", "$SM/OS/InterruptHandlers.a:1558", 0x9bc4, true);
	wp = (uint16 *)(ROMBaseHost + 0x9bc4);	// Level 1 handler
	*wp++ = htons(0x7002);		// moveq	#2,d0 (always 60Hz interrupt)
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp = htons(M68K_NOP);

	log_patch("VIA 60Hz handler", "$SM/OS/InterruptHandlers.a", 0xa296, true);
	wp = (uint16 *)(ROMBaseHost + 0xa296);	// 60Hz handler (handles everything)
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_EMUL_OP_IRQ);
	*wp++ = htons(0x4a80);		// tst.l	d0
	*wp = htons(0x67f4);		// beq		0x4080a294
	return true;
}

bool PatchROM(void)
{
	// Start a fresh patch log; GetPatchLog() always describes one pass.
	patch_log.clear();

	// Print some information about the ROM
	if (PrintROMInfo)
		print_rom_info();

	// Patch ROM depending on version
	switch (ROMVersion) {
		case ROM_VERSION_CLASSIC:
			if (!patch_rom_classic())
				return false;
			break;
		case ROM_VERSION_32:
			if (!patch_rom_32())
				return false;
			break;
		default:
			printf("You must have a version 630 or greater ROM (The Macintosh Classic ROM)\n");
			return false;
	}

	// Install breakpoint
	if (ROMBreakpoint) {
		uint16 *wp = (uint16 *)(ROMBaseHost + ROMBreakpoint);
		*wp = htons(M68K_EMUL_BREAK);
	}

	// Clear caches as we loaded and patched code
	FlushCodeCache(ROMBaseHost, ROMSize);
	return true;
}
