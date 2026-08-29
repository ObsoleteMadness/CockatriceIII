/*
 *  serial.cpp - Serial device driver
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

/*
 *  SEE ALSO
 *    Inside Macintosh: Devices, chapter 7 "Serial Driver"
 *    Technote HW 04: "Break/CTS Device Driver Event Structure"
 *    Technote 1018: "Understanding the SerialDMA Driver"
 */

#include <stdio.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "macos_util.h"
#include "serial.h"
#include "serial_defs.h"
#include "prefs.h"

#include "emul_op.h"

#define DEBUG 0
#include "debug.h"


// Global variables
SERDPort *the_serd_port[2];


/*
 *  Driver Open() routine
 */

int16 SerialOpen(uint32 pb, uint32 dce, int port)
{
	printf("SerialOpen port=%d, pb=%08x, dce=%08x\n", port, pb, dce);
	if (port == 2 || port == 3 || PrefsFindBool("ltoudp")) {
		return noErr;
	}
	return openErr;
}


/*
 *  Driver Prime() routine
 */

int16 SerialPrime(uint32 pb, uint32 dce, int port)
{
	printf("SerialPrime port=%d, pb=%08x, dce=%08x\n", port, pb, dce);
	if (port == 2 || port == 3 || PrefsFindBool("ltoudp")) {
		return noErr;
	}
	return readErr;
}


/*
 *  Driver Control() routine
 */

int16 SerialControl(uint32 pb, uint32 dce, int port)
{
	uint16 code = ReadMacInt16(pb + csCode);
	printf("SerialControl code=%d, port=%d, pb=%08x, dce=%08x\n", code, port, pb, dce);
	if (port == 2 || port == 3 || PrefsFindBool("ltoudp")) {
		return noErr;
	}
	return notOpenErr;
}


/*
 *  Driver Status() routine
 */

int16 SerialStatus(uint32 pb, uint32 dce, int port)
{
	uint16 code = ReadMacInt16(pb + csCode);
	printf("SerialStatus code=%d, port=%d, pb=%08x, dce=%08x\n", code, port, pb, dce);
	if (port == 2 || port == 3 || PrefsFindBool("ltoudp")) {
		switch (code) {
			case kSERDVersion:
				WriteMacInt8(pb + csParam, 9);		// Second-generation SerialDMA driver
				return noErr;

			case 0x8000:
				WriteMacInt8(pb + csParam, 9);		// Second-generation SerialDMA driver
				WriteMacInt16(pb + csParam + 4, 0x1997);	// Date of serial driver
				WriteMacInt16(pb + csParam + 6, 0x0616);
				return noErr;

			default:
				return noErr;
		}
	}
	return notOpenErr;
}


/*
 *  Driver Close() routine
 */

int16 SerialClose(uint32 pb, uint32 dce, int port)
{
	D(bug("SerialClose port %d, pb %08lx, dce %08lx\n", port, pb, dce));
	return noErr;
}


/*
 *  Serial interrupt - Prime command completed, activate deferred tasks to call IODone
 */

static void serial_irq(SERDPort *p)
{
//KEEP THIS CODE close... I was getting some weird virtual function failure @ runtime.
/*	if (p->is_open) {
		if (p->read_pending && p->read_done) {
			EnqueueMac(p->input_dt, 0xd92);
			p->read_pending = p->read_done = false;
		}
		if (p->write_pending && p->write_done) {
			EnqueueMac(p->output_dt, 0xd92);
			p->write_pending = p->write_done = false;
		}
	}
*/
}

void SerialInterrupt(void)
{
	D(bug("SerialIRQ\n"));

//	serial_irq(the_serd_port[0]);
//	serial_irq(the_serd_port[1]);
}
