/*
 *  main.cpp - Startup/shutdown code
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

#include "sysdeps.h"

#include "cpu_emulation.h"
#include "xpram.h"
#include "timer.h"
#include "sony.h"
#include "disk.h"
#include "cdrom.h"
#include "scsi.h"
#include "extfs.h"
#include "audio.h"
#include "video.h"
#include "serial.h"
#include "scc.h"
#include "ether.h"
#include "clip.h"
#include "rom_patches.h"
#include "user_strings.h"
#include "prefs.h"
#include "main.h"

#define DEBUG 0
#include "debug.h"

#if ENABLE_MON
#include "mon.h"

static uint32 mon_read_byte_b2(uint32 adr)
{
	return ReadMacInt8(adr);
}

static void mon_write_byte_b2(uint32 adr, uint32 b)
{
	WriteMacInt8(adr, b);
}
#endif


/*
 *  Initialize everything, returns false on error
 */
int yearoffset;	//offset the clock by how many billion ticks

bool InitAll(void)
{
	// Check ROM version
	if (!CheckROM()) {
		ErrorAlert(GetString(STR_UNSUPPORTED_ROM_TYPE_ERR));
		return false;
	}

	yearoffset = PrefsFindInt16("yearoffset");
if(yearoffset>0)
printf("Offsetting the year by %d billion ticks\n",yearoffset);

#if EMULATED_68K
	// The emulated machine is always a fixed 68040 with 32-bit addressing.
	// Classic-Mac targets (68000/68010/68020, 24-bit-addressing ROMs) are
	// no longer supported; the "cpu" pref is ignored.
	CPUType = 4;
	FPUType = 1;
	TwentyFourBitAddressing = false;
	const char *engine_name = "Musashi";
	const char *req_engine = PrefsFindString("cpu_emulator");
	if (req_engine && strcmp(req_engine, "uae") == 0) {
		engine_name = "Amiberry";
	} else if (req_engine && strcmp(req_engine, "m68k_rs") == 0) {
		engine_name = "m68k_rs";
	} else {
		engine_name = "Musashi";
	}

	printf("Setting up for a 680%d0, %s and %sbit addressing via %s\n",
	       CPUType, FPUType ? "With FPU" : "Without FPU",
	       TwentyFourBitAddressing ? "24" : "32", engine_name);

	bool jit_enabled = false;
	if (strcmp(engine_name, "Amiberry") == 0 && PrefsFindBool("jit")) {
		jit_enabled = true;
	}

	if (jit_enabled) {
		if (strcmp(engine_name, "Amiberry") == 0 && PrefsFindBool("jitfpu")) {
			printf("JIT enabled (with JIT FPU)\n");
		} else {
			printf("JIT enabled\n");
		}
	}
	fflush(stdout);
	CPUIs68060 = false;
#endif

	// Load XPRAM
	XPRAMInit();

	// Set boot volume
	int16 i16 = PrefsFindInt16("bootdrive");
	XPRAM[0x78] = i16 >> 8;
	XPRAM[0x79] = i16 & 0xff;
	i16 = PrefsFindInt16("bootdriver");
	XPRAM[0x7a] = i16 >> 8;
	XPRAM[0x7b] = i16 & 0xff;

	// Init drivers
	SonyInit();
	DiskInit();
	//CDROMInit();
	SCSIInit();

#if SUPPORTS_EXTFS
	// Init external file system
	ExtFSInit();
#endif

	// Init serial ports
	SerialInit();

	// Init SCC / LocalTalk
	SCCInit();

	// Init network
	EtherInit();

	// Init Time Manager
	TimerInit();

	// Init clipboard
	ClipInit();

	// Init audio
	AudioInit();

	// Init video
	if (!VideoInit(ROMVersion == ROM_VERSION_64K || ROMVersion == ROM_VERSION_PLUS || ROMVersion == ROM_VERSION_CLASSIC))
		{
		printf("failed to initalize video\n");
		return false;
		}

#if EMULATED_68K
	// Init 680x0 emulation (this also activates the memory system which is needed for PatchROM())
	if (!Init680x0())
		{
		printf("Init680x0 failed to initalize!\n");
		return false;
		}
#endif

	// Install ROM patches
	if (!PatchROM()) {
		ErrorAlert(GetString(STR_UNSUPPORTED_ROM_TYPE_ERR));
		printf("\nError in PatchROM()\n");
		return false;
	}

#if ENABLE_MON
	// Initialize mon
	mon_init();
	mon_read_byte = mon_read_byte_b2;
	mon_write_byte = mon_write_byte_b2;
#endif

	return true;
}


/*
 *  Deinitialize everything
 */

void ExitAll(void)
{
#if ENABLE_MON
	// Deinitialize mon
	mon_exit();
#endif

	// Save XPRAM
	XPRAMExit();

	// Exit video
	VideoExit();

	// Exit audio
	AudioExit();

	// Exit clipboard
	ClipExit();

	// Exit Time Manager
	TimerExit();

	// Exit SCC / LocalTalk
	SCCExit();

	// Exit serial ports
	SerialExit();

	// Exit network
	EtherExit();

#if SUPPORTS_EXTFS
	// Exit external file system
	ExtFSExit();
#endif

	// Exit drivers
	SCSIExit();
	//CDROMExit();
	DiskExit();
	SonyExit();
}
