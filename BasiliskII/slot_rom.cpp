/*
 *  slot_rom.cpp - Slot declaration ROM
 *
 *  Basilisk II (C) 1996-1997 Christian Bauer
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

/*
 *  SEE ALSO
 *    Inside Macintosh: Devices, chapter 2 "Slot Manager"
 *    Designing Cards and Drivers for the Macintosh Family, Second Edition
 */

#include <stdio.h>
#include <string.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "video.h"
#include "emul_op.h"
#include "version.h"
#include "slot_rom.h"


// Temporary buffer for slot ROM
static uint8 srom[4096];

// ROM offset the declaration ROM was copied to (see slot_rom.h)
uint32 SlotROMOffset = 0;

// Index in srom
static uint32 p;

// Generated declaration-ROM size, used by ChecksumSlotROM()
static int slot_rom_size = 0;

/*
 * srom-relative offsets of each depth's VModeParms (50-byte VPBlock).
 * SlotROM_PatchMode writes rowBytes and bounds here after a live switch so
 * Display Manager / InitGDevice re-read the new size from the sResource.
 */
static uint32 s_vmode_parm_off[6];


/*
 *  Construct slot declaration ROM and copy it into the Mac ROM (must be called after VideoInit())
 */

static void Offs(uint8 type, uint32 ptr)
{
	uint32 offs = ptr - p;
	srom[p++] = type;
	srom[p++] = offs >> 16;
	srom[p++] = offs >> 8;
	srom[p++] = offs;
}

static void Rsrc(uint8 type, uint32 data)
{
	srom[p++] = type;
	srom[p++] = data >> 16;
	srom[p++] = data >> 8;
	srom[p++] = data;
}

static void EndOfList(void)
{
	srom[p++] = 0xff;
	srom[p++] = 0;
	srom[p++] = 0;
	srom[p++] = 0;
}

static void Long(uint32 data)
{
	srom[p++] = data >> 24;
	srom[p++] = data >> 16;
	srom[p++] = data >> 8;
	srom[p++] = data;
}

static void Word(uint16 data)
{
	srom[p++] = data >> 8;
	srom[p++] = data;
}

static void String(char *str)
{
	while ((srom[p++] = *str++) != 0) ;
	if (p & 1)
		srom[p++] = 0;
}

static void PString(char *str)
{
	srom[p++] = strlen(str);
	while ((srom[p++] = *str++) != 0) ;
	p--;
	if (p & 1)
		srom[p++] = 0;
}

/*
 * Builds one VPBlock (video mode parameters) for a depth at the boot size.
 * Display Manager later asks cscGetVideoParameters for other resolutions;
 * this table is what pre-DM InitGDevice and the patched sResource use.
 *
 * Arguments:
 *   depth: VMODE_* pixel depth.
 *
 * Returns:
 *   srom offset of the 50-byte parameter block.
 */
static uint32 VModeParms(int depth)
{
	uint32 ret = p;
	uint32 row_bytes = Video_BytesPerRowForMode((int)VideoMonitor.x, depth);
	Long(50);					// Length
	Long(0);					// Base offset
	Word((uint16)row_bytes);
	Word(0);					// Bounds
	Word(0);
	Word((uint16)VideoMonitor.y);
	Word((uint16)VideoMonitor.x);
	Word(0);					// Version
	Word(0);					// Pack type
	Long(0);					// Pack size
	Long(0x00480000);			// HRes (72 dpi)
	Long(0x00480000);			// VRes
	switch (depth) {
		case VMODE_1BIT:
			Word(0); Word(1); Word(1); Word(1);
			break;
		case VMODE_2BIT:
			Word(0); Word(2); Word(1); Word(2);
			break;
		case VMODE_4BIT:
			Word(0); Word(4); Word(1); Word(4);
			break;
		case VMODE_8BIT:
			Word(0); Word(8); Word(1); Word(8);
			break;
		case VMODE_16BIT:
			Word(16); Word(16); Word(3); Word(5);
			break;
		case VMODE_32BIT:
			Word(16); Word(32); Word(3); Word(8);
			break;
		default:
			Word(0); Word(8); Word(1); Word(8);
			break;
	}
	Long(0);					// Plane size
	Long(0);					// Reserved
	return ret;
}

/*
 * Builds the sResource list that names one depth's parameter block.
 *
 * Arguments:
 *   params: Offset of the VPBlock from VModeParms().
 *   direct: true for 16/32-bit (device type 2), false for CLUT depths.
 *
 * Returns:
 *   srom offset of the mode description list.
 */
static uint32 VModeDesc(uint32 params, bool direct)
{
	uint32 ret = p;
	Offs(0x01, params);			// Video parameters
	Rsrc(0x03, 1);				// Page count
	Rsrc(0x04, direct ? 2 : 0);	// Device type (0 = CLUT, 2 = direct)
	EndOfList();
	return ret;
}

/*
 * Recalculates the declaration-ROM CRC in place after a VModeParms patch.
 */
void ChecksumSlotROM(void)
{
	if (slot_rom_size <= 12 || !ROMBaseHost)
		return;

	uint8 *rom = ROMBaseHost + SlotROMOffset;
	rom[slot_rom_size - 12] = 0;
	rom[slot_rom_size - 11] = 0;
	rom[slot_rom_size - 10] = 0;
	rom[slot_rom_size - 9] = 0;
	uint32 crc = 0;
	for (int i = 0; i < slot_rom_size; i++) {
		crc = (crc << 1) | (crc >> 31);
		crc += rom[i];
	}
	rom[slot_rom_size - 12] = (uint8)(crc >> 24);
	rom[slot_rom_size - 11] = (uint8)(crc >> 16);
	rom[slot_rom_size - 10] = (uint8)(crc >> 8);
	rom[slot_rom_size - 9] = (uint8)crc;
}

/*
 * Patches the slot-ROM VModeParms for one Apple depth and refreshes the CRC.
 *
 * Arguments:
 *   mode: VMODE_* depth whose sResource table is updated.
 *   width, height: New pixel size written into vpBounds.
 *   row_bytes: Packed bytes per row written into vpRowBytes.
 */
void SlotROM_PatchMode(int mode, int width, int height, uint32 row_bytes)
{
	if (mode < VMODE_1BIT || mode > VMODE_32BIT)
		return;
	if (!ROMBaseHost || SlotROMOffset == 0)
		return;
	uint32 off = s_vmode_parm_off[mode];
	if (off == 0)
		return;

	uint8 *block = ROMBaseHost + SlotROMOffset + off;
	block[8] = (uint8)(row_bytes >> 8);
	block[9] = (uint8)row_bytes;
	block[14] = (uint8)((uint16)height >> 8);
	block[15] = (uint8)height;
	block[16] = (uint8)((uint16)width >> 8);
	block[17] = (uint8)width;
	ChecksumSlotROM();
}

bool InstallSlotROM(void)
{
	uint32 boardType, boardName, vendorID, revLevel, partNum, date;
	uint32 vendorInfo, sRsrcBoard;

	uint32 videoType, videoName, minorBase, minorLength, videoDrvr, vidDrvrDir;
	uint32 defaultGamma, gammaDir, sRsrcVideo;
	uint32 vidModeParms[6], vidMode[6];

	uint32 cpuType, cpuName, cpuMajor, cpuMinor, sRsrcCPU;

	uint32 etherType, etherName, etherDrvr, etherDrvrDir, sRsrcEther;

	uint32 sRsrcDir;

	char str[256];
	p = 0;

	// Board sResource
	boardType = p;						// Literals
	Word(1); Word(0); Word(0); Word(0);	// Board sResource
	boardName = p;
	String("Basilisk II Slot ROM");
	vendorID = p;
	String("Christian Bauer");
	revLevel = p;
	sprintf(str, "V%d.%d", VERSION_MAJOR, VERSION_MINOR);
	String(str);
	partNum = p;
	String("BasiliskII");
	date = p;
	String(__DATE__);

	vendorInfo = p;						// Vendor Info
	Offs(0x01, vendorID);				// Vendor ID
	Offs(0x03, revLevel);				// Revision level
	Offs(0x04, partNum);				// Part number
	Offs(0x05, date);					// ROM build date
	EndOfList();

	sRsrcBoard = p;
	Offs(0x01, boardType);				// Board descriptor
	Offs(0x02, boardName);				// Board name
	Rsrc(0x20, 0x4232);					// Board ID ('B2')
	Offs(0x24, vendorInfo);				// Vendor Info
	EndOfList();

	// Video sResource
	videoType = p;						// Literals
	Word(3); Word(1); Word(1); Word(0x4232);			// Display Video Apple 'B2'
	videoName = p;
	String("Display_Video_Apple_Basilisk");
	minorBase = p;
	Long(VideoMonitor.mac_frame_base);					// Frame buffer base
	minorLength = p;
	Long(MacFrameSize ? MacFrameSize : VideoMonitor.bytes_per_row * VideoMonitor.y);	// Frame buffer size

	videoDrvr = p;						// Video driver
	Long(0x72);							// Length
	Word(0x4c00); Word(0); Word(0); Word(0);
	Word(0x32);							// Open offset
	Word(0x36);							// Prime offset
	Word(0x3a);							// Control offset
	Word(0x46);							// Status offset
	Word(0x6c);							// Close offset
	PString(".Display_Video_Apple_Basilisk");
	Word(1);							// Driver version
	Word(M68K_EMUL_OP_VIDEO_OPEN);		// Open()
	Word(0x4e75);
	Word(0x70ff);						// Prime()
	Word(0x600e);
	Word(M68K_EMUL_OP_VIDEO_CONTROL);	// Control()
	Word(0x0c68); Word(0x0001); Word(0x001a);
	Word(0x6604);
	Word(0x4e75);
	Word(M68K_EMUL_OP_VIDEO_STATUS);	// Status()
	Word(0x3228); Word(0x0006);			// IOReturn
	Word(0x0801); Word(0x0009);
	Word(0x670c);
	Word(0x4a40);
	Word(0x6f02);
	Word(0x4240);
	Word(0x3140); Word(0x0010);
	Word(0x4e75);
	Word(0x4a40);
	Word(0x6f04);
	Word(0x4240);
	Word(0x4e75);
	Word(0x2f38); Word(0x08fc);
	Word(0x4e75);
	Word(0x70e8);						// Close()
	Word(0x4e75);

	vidDrvrDir = p;						// Driver directory
	Offs(0x02, videoDrvr);				// sMacOS68020
	EndOfList();

	defaultGamma = p;					// Gamma table
	Long(38 + 0x100);					// Length
	Word(0x2000);						// Resource ID
	String("Mac HiRes Std Gamma");
	Word(0);							// Version
	Word(0);							// Type
	Word(0);							// FormulaSize
	Word(1);							// ChanCnt
	Word(0x0100);						// DataCnt
	Word(8);							// ChanWidth
	Long(0x0005090B); Long(0x0E101315); Long(0x17191B1D); Long(0x1E202224);
	Long(0x2527282A); Long(0x2C2D2F30); Long(0x31333436); Long(0x37383A3B);
	Long(0x3C3E3F40); Long(0x42434445); Long(0x4748494A); Long(0x4B4D4E4F);
	Long(0x50515254); Long(0x55565758); Long(0x595A5B5C); Long(0x5E5F6061);
	Long(0x62636465); Long(0x66676869); Long(0x6A6B6C6D); Long(0x6E6F7071);
	Long(0x72737475); Long(0x76777879); Long(0x7A7B7C7D); Long(0x7E7F8081);
	Long(0x81828384); Long(0x85868788); Long(0x898A8B8C); Long(0x8C8D8E8F);
	Long(0x90919293); Long(0x94959596); Long(0x9798999A); Long(0x9B9B9C9D);
	Long(0x9E9FA0A1); Long(0xA1A2A3A4); Long(0xA5A6A6A7); Long(0xA8A9AAAB);
	Long(0xABACADAE); Long(0xAFB0B0B1); Long(0xB2B3B4B4); Long(0xB5B6B7B8);
	Long(0xB8B9BABB); Long(0xBCBCBDBE); Long(0xBFC0C0C1); Long(0xC2C3C3C4);
	Long(0xC5C6C7C7); Long(0xC8C9CACA); Long(0xCBCCCDCD); Long(0xCECFD0D0);
	Long(0xD1D2D3D3); Long(0xD4D5D6D6); Long(0xD7D8D9D9); Long(0xDADBDCDC);
	Long(0xDDDEDFDF); Long(0xE0E1E1E2); Long(0xE3E4E4E5); Long(0xE6E7E7E8);
	Long(0xE9E9EAEB); Long(0xECECEDEE); Long(0xEEEFF0F1); Long(0xF1F2F3F3);
	Long(0xF4F5F5F6); Long(0xF7F8F8F9); Long(0xFAFAFBFC); Long(0xFCFDFEFF);

	gammaDir = p;						// Gamma directory
	Offs(0x80, defaultGamma);
	EndOfList();

	/*
	 * One sResource per Apple depth (0x80..0x85). They are contiguous so
	 * pre-Display Manager Mac OS can walk them as "modes". Each VPBlock is
	 * baked at the boot size; SlotROM_PatchMode rewrites rowBytes/bounds
	 * when cscSetMode / cscSwitchMode (or the host Video menu) changes it.
	 */
	for (int d = VMODE_1BIT; d <= VMODE_32BIT; d++) {
		vidModeParms[d] = VModeParms(d);
		s_vmode_parm_off[d] = vidModeParms[d];
		vidMode[d] = VModeDesc(vidModeParms[d], IsDirectMode(d));
	}

	sRsrcVideo = p;
	Offs(0x01, videoType);				// Video type descriptor
	Offs(0x02, videoName);				// Driver name
	Offs(0x04, vidDrvrDir);				// Driver directory
	Rsrc(0x08, 0x4232);					// Hardware device ID ('B2')
	Offs(0x0a, minorBase);				// Frame buffer base
	Offs(0x0b, minorLength);			// Frame buffer length
	Offs(0x40, gammaDir);				// Gamma directory
	Rsrc(0x7d, 6);						// Video attributes: Default to color, built-in
	Offs(0x80, vidMode[VMODE_1BIT]);
	Offs(0x81, vidMode[VMODE_2BIT]);
	Offs(0x82, vidMode[VMODE_4BIT]);
	Offs(0x83, vidMode[VMODE_8BIT]);
	Offs(0x84, vidMode[VMODE_16BIT]);
	Offs(0x85, vidMode[VMODE_32BIT]);
	EndOfList();

	// CPU sResource
	cpuType = p;						// Literals
	Word(10); Word(3); Word(0); Word(24);	// CPU 68020
	cpuName = p;
	String("CPU_68020");
	cpuMajor = p;
	Long(0); Long(0x7fffffff);
	cpuMinor = p;
	Long(0xf0800000); Long(0xf0ffffff);

	sRsrcCPU = p;
	Offs(0x01, cpuType);				// Type descriptor
	Offs(0x02, cpuName);				// CPU name
	Offs(0x81, cpuMajor);				// Major RAM space
	Offs(0x82, cpuMinor);				// Minor RAM space
	EndOfList();

	// Ethernet sResource
	etherType = p;						// Literals
	Word(4); Word(1); Word(1); Word(0x4232);	// Network Ethernet Apple 'B2'
	etherName = p;
	String("Network_Ethernet_Apple_BasiliskII");

	etherDrvr = p;						// Video driver
	Long(0x88);							// Length
	Word(0x4400); Word(0); Word(0); Word(0);
	Word(0x4a);							// Open offset
	Word(0x4e);							// Prime offset
	Word(0x52);							// Control offset
	Word(0x4e);							// Status offset
	Word(0x82);							// Close offset
	PString(".ENET");
	Word(0x0111); Word(0x8000);			// Driver version
	Word(0);
	PString("1.1.1  ");
	PString("Basilisk II Ethernet Network Driver");
	Word(M68K_EMUL_OP_ETHER_OPEN);		// Open()
	Word(0x4e75);
	Word(0x70ef);						// Prime()/Status()
	Word(0x600c);
	Word(M68K_EMUL_OP_ETHER_CONTROL);	// Control()
	Word(0x0c68); Word(0x0001); Word(0x001a);
	Word(0x6602);
	Word(0x4e75);
	Word(0x3228); Word(0x0006);			// IOReturn
	Word(0x0801); Word(0x0009);
	Word(0x670c);
	Word(0x4a40);
	Word(0x6f02);
	Word(0x4240);
	Word(0x3140); Word(0x0010);
	Word(0x4e75);
	Word(0x4a40);
	Word(0x6f04);
	Word(0x4240);
	Word(0x4e75);
	Word(0x2f38); Word(0x08fc);
	Word(0x4e75);
	Word(0x70e8);						// Close()
	Word(0x4e75);

	etherDrvrDir = p;					// Driver directory
	Offs(0x02, etherDrvr);				// sMacOS68020
	EndOfList();

	sRsrcEther = p;
	Offs(0x01, etherType);				// Type descriptor
	Offs(0x02, etherName);				// Driver name
	Offs(0x04, etherDrvrDir);			// Driver directory
	Rsrc(0x07, 0);						// Flags: 0 (not OpenAtStart, so user port selection persists)
	Rsrc(0x08, 0x4232);					// Hardware device ID ('B2')
	EndOfList();

	// sResource directory
	sRsrcDir = p;
	Offs(0x01, sRsrcBoard);
	Offs(0x80, sRsrcVideo);
	Offs(0xf0, sRsrcCPU);
	Offs(0xf1, sRsrcEther);
	EndOfList();

	// Format/header block
	Offs(0, sRsrcDir);					// sResource directory
	Long(p + 16);						// Length of declaration data
	Long(0);							// CRC (calculated below)
	Word(0x0101);						// Rev. level, format
	Long(0x5a932bc7);					// Test pattern
	Word(0x000f);						// Byte lanes

	// Copy slot ROM to Mac ROM, then stamp the CRC in place
	slot_rom_size = (int)p;
	SlotROMOffset = ROMSize - p;
	memcpy(ROMBaseHost + SlotROMOffset, srom, p);
	ChecksumSlotROM();
	return true;
}
