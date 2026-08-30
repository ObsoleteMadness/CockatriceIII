/*
 *  scsi.cpp - SCSI Manager
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
 *    Inside Macintosh: Devices, chapter 3 "SCSI Manager"
 *    Technote DV 24: "Fear No SCSI"
 */

#include <stdio.h>
#include <string.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "prefs.h"
#include "user_strings.h"
#include "scsi.h"

#define DEBUG 0
#include "debug.h"

bool g_scsi_debug = true;

void SCSI_SetDebug(bool enable)
{
	g_scsi_debug = enable;
}

#define SCSI_LOG(...) do { \
	if (g_scsi_debug) { \
		printf(__VA_ARGS__); \
		fflush(stdout); \
	} \
} while (0)

const char *scsi_cmd_name(uint8 opcode)
{
	switch (opcode) {
		case 0x00: return "TEST_UNIT_READY";
		case 0x01: return "REZERO_UNIT";
		case 0x03: return "REQUEST_SENSE";
		case 0x04: return "FORMAT_DRIVE";
		case 0x05: return "VERIFY_TRACK";
		case 0x06: return "FORMAT_TRACK";
		case 0x08: return "READ(6)";
		case 0x0A: return "WRITE(6)";
		case 0x0B: return "SEEK(6)";
		case 0x0D: return "CORRECTION";
		case 0x12: return "INQUIRY";
		case 0x15: return "MODE_SELECT(6)";
		case 0x16: return "RESERVE(6)";
		case 0x17: return "RELEASE(6)";
		case 0x1A: return "MODE_SENSE(6)";
		case 0x1B: return "START_STOP_UNIT";
		case 0x1E: return "PREVENT_ALLOW_MEDIUM_REMOVAL";
		case 0x25: return "READ_CAPACITY";
		case 0x28: return "READ(10)";
		case 0x2A: return "WRITE(10)";
		case 0x2B: return "SEEK(10)";
		case 0x35: return "SYNCHRONIZE_CACHE";
		case 0x42: return "READ_SUBCHANNEL";
		case 0x43: return "READ_TOC";
		case 0x44: return "READ_HEADER";
		case 0x45: return "PLAY_AUDIO(10)";
		case 0x46: return "GET_CONFIGURATION";
		case 0x47: return "PLAY_AUDIO_MSF";
		case 0x48: return "PLAY_AUDIO_TRACK_INDEX";
		case 0x49: return "PLAY_TRACK_RELATIVE(10)";
		case 0x4A: return "GET_EVENT_STATUS_NOTIFICATION";
		case 0x4B: return "PAUSE_RESUME_AUDIO";
		case 0x4E: return "STOP_PLAY_SCAN";
		case 0x51: return "READ_DISC_INFORMATION";
		case 0x52: return "READ_TRACK_INFORMATION";
		case 0x55: return "MODE_SELECT(10)";
		case 0x56: return "RESERVE(10)";
		case 0x57: return "RELEASE(10)";
		case 0x5A: return "MODE_SENSE(10)";
		case 0xA5: return "PLAY_AUDIO(12)";
		case 0xA8: return "READ(12)";
		case 0xA9: return "PLAY_TRACK_RELATIVE(12)";
		case 0xAA: return "WRITE(12)";
		case 0xB9: return "READ_CD_MSF";
		case 0xBB: return "SET_CD_SPEED";
		case 0xBD: return "MECHANISM_STATUS";
		case 0xBE: return "READ_CD";
		case 0xCD: return "APPLE_FF_REW";
		case 0xD8: return "APPLE_CDDA";
		case 0xD9: return "APPLE_CDDA_MSF";
		default: return "UNKNOWN_COMMAND";
	}
}

// Error codes
enum {
	scCommErr = 2,
	scArbNBErr,
	scBadParmsErr,
	scPhaseErr,
	scCompareErr,
	scMgrBusyErr,
	scSequenceErr,
	scBusTOErr,
	scComplPhaseErr
};

// TIB opcodes
enum {
	scInc = 1,
	scNoInc,
	scAdd,
	scMove,
	scLoop,
	scNop,
	scStop,
	scComp
};

// Logical SCSI phases
enum {
	PH_FREE,		// Bus free
	PH_ARBITRATED,	// Bus arbitrated (after SCSIGet())
	PH_SELECTED,	// Target selected (after SCSISelect())
	PH_TRANSFER		// Command sent (after SCSICmd())
};

// Global variables
static int target_id;					// ID of active target
static int phase;						// Logical SCSI phase
static uint16 fake_status;				// Faked 5830 status word
static bool reading;					// Flag: reading from device

const int SG_TABLE_SIZE = 32768;
static int sg_index;					// Index of first unused entry in S/G table
static uint8 *sg_ptr[SG_TABLE_SIZE];	// Scatter/gather table data pointer (host address space)
static uint32 sg_len[SG_TABLE_SIZE];	// Scatter/gather table data length
static uint32 sg_total_length;			// Total data length


/*
 *  Execute TIB, constructing S/G table
 */

static int16 exec_tib(uint32 tib)
{
	for (;;) {

		// Read next opcode and parameters
		uint16 cmd = ReadMacInt16(tib); tib += 2;
		uint32 ptr = ReadMacInt32(tib); tib += 4;
		uint32 len = ReadMacInt32(tib); tib += 4;

		D(bug(" %d %08x %d\n", cmd, ptr, len));

		switch (cmd) {
			case scInc:
				WriteMacInt32(tib - 8, ptr + len);
			case scNoInc:
				if ((sg_index > 0) && (Mac2HostAddr(ptr) == sg_ptr[sg_index-1] + sg_len[sg_index-1]))
					sg_len[sg_index-1] += len;				// Merge to previous entry
				else {
					if (sg_index == SG_TABLE_SIZE) {
						ErrorAlert(GetString(STR_SCSI_SG_FULL_ERR));
						return -108;
					}
					sg_ptr[sg_index] = Mac2HostAddr(ptr);	// Create new entry
					sg_len[sg_index] = len;
					sg_index++;
				}
				sg_total_length += len;
				break;

			case scAdd:
				WriteMacInt32(ptr, ReadMacInt32(ptr) + len);
				break;

			case scMove:
				WriteMacInt32(len, ReadMacInt32(ptr));
				break;

			case scLoop:
				WriteMacInt32(tib - 4, len - 1);
				if (len - 1 > 0)
					tib += (int32)ptr - 10;
				break;

			case scNop:
				break;

			case scStop:
				return 0;

			case scComp:
				printf("WARNING: Unimplemented scComp opcode\n");
				return scCompareErr;

			default:
				printf("FATAL: Unrecognized TIB opcode %d\n", cmd);
				return scBadParmsErr;
		}
	}
}


/*
 *  Reset SCSI bus
 */

int16 SCSIReset(void)
{
	SCSI_LOG("[SCSI-MGR] SCSIReset()\n");

	phase = PH_FREE;
	fake_status = 0x0000;	// Bus free
	sg_index = 0;
	target_id = 8;
	return 0;
}


/*
 *  Arbitrate bus
 */

int16 SCSIGet(void)
{
	SCSI_LOG("[SCSI-MGR] SCSIGet() -> Arbitrating (current phase=%d)\n", phase);
	if (phase != PH_FREE)
		return scMgrBusyErr;

	phase = PH_ARBITRATED;
	fake_status = 0x0040;	// Bus busy
	reading = false;
	sg_index = 0;			// Flush S/G table
	sg_total_length = 0;
	return 0;
}


/*
 *  Select SCSI device
 */

int16 SCSISelect(int id)
{
	if (phase != PH_ARBITRATED) {
		SCSI_LOG("[SCSI-MGR] SCSISelect(%d): FAILED - phase is not PH_ARBITRATED (phase=%d)\n", id, phase);
		return scSequenceErr;
	}

	// ID valid?
	if (id >= 0 && id <= 7) {
		target_id = id;

		// Target present?
		if (scsi_is_target_present(target_id)) {
			phase = PH_SELECTED;
			fake_status = 0x006a;			// Target selected, command phase
			SCSI_LOG("[SCSI-MGR] SCSISelect(ID %d) -> Target Selected (Command Phase)\n", id);
			return 0;
		}
	}

	SCSI_LOG("[SCSI-MGR] SCSISelect(ID %d) -> Selection Timeout (Target Not Present)\n", id);
	// Error
	phase = PH_FREE;
	fake_status = 0x0000;		// Bus free
	return scCommErr;
}


/*
 *  Send SCSI command
 */

int16 SCSICmd(int cmd_length, uint8 *cmd)
{
	char cdb_hex[64] = {0};
	for (int i = 0; i < cmd_length && i < 16; i++) {
		char bstr[4];
		snprintf(bstr, sizeof(bstr), "%02X ", cmd[i]);
		strncat(cdb_hex, bstr, sizeof(cdb_hex) - strlen(cdb_hex) - 1);
	}

	SCSI_LOG("[SCSI-MGR] SCSICmd(len=%d): Target %d LUN %d | %s | CDB: [%s]\n",
	         cmd_length, target_id, (cmd[1] >> 5) & 7, scsi_cmd_name(cmd[0]), cdb_hex);

	if (phase != PH_SELECTED) {
		SCSI_LOG("[SCSI-MGR] SCSICmd: Phase error (phase=%d != PH_SELECTED)\n", phase);
		return scPhaseErr;
	}

	// Command length valid?
	if (cmd_length != 6 && cmd_length != 10 && cmd_length != 12) {
		SCSI_LOG("[SCSI-MGR] SCSICmd: Bad parameter length (%d)\n", cmd_length);
		return scBadParmsErr;
	}

	// Set command, extract LUN
	scsi_set_cmd(cmd_length, cmd);

	// Extract LUN, set target
	if (!scsi_set_target(target_id, (cmd[1] >> 5) & 7)) {
		SCSI_LOG("[SCSI-MGR] SCSICmd: scsi_set_target(%d, %d) failed\n", target_id, (cmd[1] >> 5) & 7);
		phase = PH_FREE;
		fake_status = 0x0000;	// Bus free
		return scCommErr;
	}

	phase = PH_TRANSFER;
	fake_status = 0x006e;		// Target selected, data phase
	return 0;
}


/*
 *  Read data
 */

int16 SCSIRead(uint32 tib)
{
	if (phase != PH_TRANSFER) {
		SCSI_LOG("[SCSI-MGR] SCSIRead: Phase error (phase=%d != PH_TRANSFER)\n", phase);
		return scPhaseErr;
	}

	// Execute TIB, fill S/G table
	reading = true;
	int16 res = exec_tib(tib);
	SCSI_LOG("[SCSI-MGR] SCSIRead: TIB 0x%08X -> total %u bytes in %d S/G segment(s), result %d\n",
	         tib, sg_total_length, sg_index, res);
	return res;
}


/*
 *  Write data
 */

int16 SCSIWrite(uint32 tib)
{
	if (phase != PH_TRANSFER) {
		SCSI_LOG("[SCSI-MGR] SCSIWrite: Phase error (phase=%d != PH_TRANSFER)\n", phase);
		return scPhaseErr;
	}

	// Execute TIB, fill S/G table
	int16 res = exec_tib(tib);
	SCSI_LOG("[SCSI-MGR] SCSIWrite: TIB 0x%08X -> total %u bytes in %d S/G segment(s), result %d\n",
	         tib, sg_total_length, sg_index, res);
	return res;
}


/*
 *  Wait for command completion (we're actually doing everything in here...)
 */

int16 SCSIComplete(uint32 timeout, uint32 message, uint32 stat)
{
	WriteMacInt16(message, 0);
	if (phase != PH_TRANSFER) {
		SCSI_LOG("[SCSI-MGR] SCSIComplete: Phase error (phase=%d != PH_TRANSFER)\n", phase);
		return scPhaseErr;
	}

	// Send command, process S/G table
	uint16 scsi_stat = 0;
	bool success = scsi_send_cmd(sg_total_length, reading, sg_index, sg_ptr, sg_len, &scsi_stat, timeout);
	WriteMacInt16(stat, scsi_stat);

	SCSI_LOG("[SCSI-MGR] SCSIComplete: Target %d -> Status 0x%02X (%s), Msg 0x00, TotalData %u bytes, Success %d\n",
	         target_id, scsi_stat, (scsi_stat == 0) ? "STAT_GOOD" : ((scsi_stat == 2) ? "STAT_CHECK_CONDITION" : "STAT_OTHER"),
	         sg_total_length, success);

	// Complete command
	phase = PH_FREE;
	fake_status = 0x0000;	// Bus free
	return success ? 0 : scCommErr;
}


/*
 *  Get bus status
 */

uint16 SCSIStat(void)
{
	return fake_status;
}


/*
 *  SCSI Manager busy?
 */

int16 SCSIMgrBusy(void)
{
	return phase != PH_FREE;
}

