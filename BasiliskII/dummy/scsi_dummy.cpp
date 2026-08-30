/*
 *  scsi_dummy.cpp - SCSI Manager, dummy/software implementation
 *
 *  Basilisk II (C) 1997-1999 Christian Bauer
 *  Cockatrice III enhancements (C) 2026
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <chrono>
#include <vector>

#include "sysdeps.h"
#include "prefs.h"
#include "scsi.h"
#include "menu_bar.h"

#define DEBUG 0
#include "debug.h"

#define SCSI_LOG(...) do { \
	if (g_scsi_debug) { \
		printf(__VA_ARGS__); \
		fflush(stdout); \
	} \
} while (0)

/******** Macros *************/
#define COMMAND_ReadInt16(a, i) (((unsigned) a[i] << 8) | a[i + 1])
#define COMMAND_ReadInt24(a, i) (((unsigned) a[i] << 16) | ((unsigned) a[i + 1] << 8) | a[i + 2])
#define COMMAND_ReadInt32(a, i) (((unsigned) a[i] << 24) | ((unsigned) a[i + 1] << 16) | ((unsigned) a[i + 2] << 8) | a[i + 3])

#define BLOCKSIZE 512
#define CDBLOCKSIZE 2048

#define LUN_DISK 0 // for now only LUN 0 is valid for our phys drives

/* Status Codes */
#define STAT_GOOD           0x00
#define STAT_CHECK_COND     0x02
#define STAT_COND_MET       0x04
#define STAT_BUSY           0x08
#define STAT_INTERMEDIATE   0x10
#define STAT_INTER_COND_MET 0x14
#define STAT_RESERV_CONFL   0x18

/* Messages */
#define MSG_COMPLETE        0x00
#define MSG_SAVE_PTRS       0x02
#define MSG_RESTORE_PTRS    0x03
#define MSG_DISCONNECT      0x04
#define MSG_INITIATOR_ERR   0x05
#define MSG_ABORT           0x06
#define MSG_MSG_REJECT      0x07
#define MSG_NOP             0x08
#define MSG_PARITY_ERR      0x09
#define MSG_LINK_CMD_CMPLT  0x0A
#define MSG_LNKCMDCMPLTFLAG 0x0B
#define MSG_DEVICE_RESET    0x0C

#define MSG_IDENTIFY_MASK   0x80
#define MSG_ID_DISCONN      0x40
#define MSG_LUNMASK         0x07

/* Sense Keys */
#define SK_NOSENSE          0x00
#define SK_RECOVERED        0x01
#define SK_NOTREADY         0x02
#define SK_MEDIA            0x03
#define SK_HARDWARE         0x04
#define SK_ILLEGAL_REQ      0x05
#define SK_UNIT_ATN         0x06
#define SK_DATAPROTECT      0x07
#define SK_ABORTED_CMD      0x0B
#define SK_VOL_OVERFLOW     0x0D
#define SK_MISCOMPARE       0x0E

/* Additional Sense Codes */
#define SC_NO_ERROR         0x00    // 0
#define SC_NO_SECTOR        0x01    // 4
#define SC_WRITE_FAULT      0x03    // 5
#define SC_INVALID_CMD      0x20    // 5
#define SC_INVALID_LBA      0x21    // 5
#define SC_INVALID_CDB      0x24    // 5
#define SC_INVALID_LUN      0x25    // 5
#define SC_WRITE_PROTECT    0x27    // 7
#define SC_NOT_READY_TO_READY_TRANSITION 0x28 // 6
#define SC_MEDIUM_NOT_PRESENT 0x3A // 2

/* SCSI Commands */
#define CMD_TEST_UNIT_RDY   0x00    /* Test unit ready */
#define CMD_REZERO_UNIT     0x01    /* Rezero unit */
#define CMD_REQ_SENSE       0x03    /* Request sense */
#define CMD_FORMAT_DRIVE    0x04    /* Format the whole drive */
#define CMD_VERIFY_TRACK    0x05    /* Verify track */
#define CMD_FORMAT_TRACK    0x06    /* Format track */
#define CMD_READ_SECTOR     0x08    /* Read sector */
#define CMD_WRITE_SECTOR    0x0A    /* Write sector */
#define CMD_SEEK            0x0B    /* Seek */
#define CMD_CORRECTION      0x0D    /* Correction */
#define CMD_INQUIRY         0x12    /* Inquiry */
#define CMD_MODESELECT      0x15    /* Mode select */
#define CMD_RESERVE         0x16    /* Reserve */
#define CMD_RELEASE         0x17    /* Release */
#define CMD_MODESENSE       0x1A    /* Mode sense */
#define CMD_START_STOP      0x1B    /* Start/stop unit */
#define CMD_SHIP            0x1B    /* Ship drive */
#define CMD_PREVENT_ALLOW   0x1E    /* Prevent/Allow medium removal */
#define CMD_READ_CAPACITY1  0x25    /* Read capacity (class 1) */
#define CMD_READ_SECTOR1    0x28    /* Read sector (class 1) */
#define CMD_WRITE_SECTOR1   0x2A    /* Write sector (class 1) */
#define CMD_SEEK10          0x2B    /* Seek (10) */
#define CMD_SYNCHRONIZE_CACHE 0x35  /* Synchronize cache */
#define SCSI_SUBCHANNEL     0x42    /* Read Subchannel */
#define CMD_READ_TOC        0x43    /* Read table of contents */
#define SCSI_READHEADER     0x44    /* Read Header */
#define SCSI_PLAYAUD_10     0x45    /* Play Audio 10-Byte */
#define CMD_GET_CONFIGURATION 0x46  /* MMC Get Configuration */
#define SCSI_PLAYAUDMSF     0x47    /* Play Audio MSF */
#define SCSI_PLAYA_TKIN     0x48    /* Play Audio Track/Index */
#define SCSI_PLYTKREL10     0x49    /* Play Track Relative 10-Byte */
#define CMD_GET_EVENT_STATUS 0x4A   /* MMC Get Event Status Notification */
#define SCSI_PAUSE_RESUME   0x4B    /* Pause/Resume Audio */
#define SCSI_STOP_PLAY_SCAN 0x4E    /* Stop Play / Scan */
#define CMD_READ_DISC_INFO  0x51    /* MMC Read Disc Information */
#define CMD_READ_TRACK_INFO 0x52    /* MMC Read Track Information */
#define CMD_MODESELECT10    0x55    /* Mode select 10 */
#define CMD_RESERVE10       0x56    /* Reserve 10 */
#define CMD_RELEASE10       0x57    /* Release 10 */
#define CMD_MODESENSE10     0x5A    /* Mode sense 10 */
#define CMD_READ12          0xA8    /* Read 12 */
#define CMD_WRITE12         0xAA    /* Write 12 */
#define SCSI_PLAYAUD_12     0xA5    /* Play Audio 12-Byte */
#define SCSI_PLYTKREL12     0xA9    /* Play Track Relative 12-Byte */
#define CMD_READ_CD_MSF     0xB9    /* MMC Read CD MSF */
#define CMD_SET_CD_SPEED    0xBB    /* MMC Set CD Speed */
#define CMD_MECHANISM_STATUS 0xBD   /* MMC Mechanism Status */
#define CMD_READ_CD         0xBE    /* MMC Read CD */
#define CMD_APPLE_FF_REW    0xCD    /* AppleCD Audio Player Fast Forward / Rewind */
#define CMD_APPLE_CDDA      0xD8    /* Apple 300+ Read CD-DA (LBA) */
#define CMD_APPLE_CDDA_MSF  0xD9    /* Apple 300+ Read CD-DA (MSF) */

/* INQUIRY response data */
#define DEVTYPE_DISK        0x00    /* read/write disks */
#define DEVTYPE_TAPE        0x01    /* tapes and other sequential devices */
#define DEVTYPE_PRINTER     0x02    /* printers */
#define DEVTYPE_PROCESSOR   0x03    /* cpus */
#define DEVTYPE_WORM        0x04    /* write-once optical disks */
#define DEVTYPE_READONLY    0x05    /* cd-roms */
#define DEVTYPE_NOTPRESENT  0x7f    /* logical unit not present */

#define ESP_MAX_DEVS 7

static unsigned char inquiry_bytes[] =
{
	0x00,		/* 0: device type: Direct-access block device */
	0x00,		/* 1: &0x7F - device type qualifier 0x00, &0x80 - rmb: 0x00 = nonremovable */
	0x01,		/* 2: ANSI SCSI standard compliant */
	0x01, 		/* 3: Response format SCSI-1 compliant */
	0x31, 		/* 4: additional length of the following data */
	0x00, 0x00, 	/* 5,6: reserved */
	0x00, 		/* 7: RelAdr=0, Wbus32=0, Wbus16=0, Sync=1, Linked=1, RSVD=1, CmdQue=0, SftRe=0 */
	'Q','U','A','N','T','U','M',' ',		/*  8-15: Vendor ASCII */
	'V','D','I','S','K',' ',' ',' ',        /* 16-23: Model ASCII */
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,		/* 24-31: Blank space ASCII */
	'0','9','0','0', 0x00, 0x00, 0x00, 0xd9,		/* 32-39: Revision ASCII */
	'2','0','1','4','v','0','2','a',
	0x07, 0x00, 0xa0, 0x00, 0x00, 0xff			/* 48-53: Blank space ASCII */
};

/* Apple-compatible CD-ROM Inquiry (Sony CDU-8004, matching snow & Apple CD-ROM extensions) */
static unsigned char CDinquiry_bytes[] =
{
	0x05,             /* 0: device type: CD-ROM */
	0x80,             /* 1: RMB = removable */
	0x02,             /* 2: ANSI SCSI-2 */
	0x02,             /* 3: Response format SCSI-2 */
	0x1F,             /* 4: additional length (36-byte INQUIRY total) */
	0x00, 0x00,       /* 5,6: reserved */
	0x18,             /* 7: Sync=1, Linked=1 */
	'S','O','N','Y',' ',' ',' ',' ',        /*  8-15: Vendor */
	'C','D','-','R','O','M',' ','C',        /* 16-23: Product */
	'D','U','-','8','0','0','4',' ',        /* 24-31: "CD-ROM CDU-8004 " */
	'1','.','9','a'                         /* 32-35: Revision */
};

/******************************/
/* CD-ROM Track & Disc Model  */
/******************************/

enum CDTrackType {
	CD_MODE_AUDIO = 0,
	CD_MODE_MODE1_2048,
	CD_MODE_MODE1_2352,
	CD_MODE_MODE2_2352
};

struct CDTrack {
	int track_num;              // 1..99
	CDTrackType type;
	uint32 sector_size;         // 2048 or 2352
	uint32 start_lba;           // Track start (including pregap)
	uint32 data_start_lba;      // Index 01 start (audio/data start)
	uint32 length_sectors;      // Sector count
	uint64 file_offset;         // File byte offset for data_start_lba
	uint32 unstored_pregap;     // Unstored pregap length in sectors
	char filename[1024];        // Full path to bin/wav/iso file
	FILE *fh;                   // Open file handle
};

const int MAX_TRACKS_PER_DISC = 100;

struct CDROMDriveState {
	bool has_cue;
	int track_count;
	CDTrack tracks[MAX_TRACKS_PER_DISC];
	uint32 leadout_lba;

	// Audio playback state
	uint8 audio_status;         // 0x11 = playing, 0x12 = paused, 0x15 = stopped/no status
	uint32 audio_start_lba;
	uint32 audio_end_lba;
	uint32 audio_current_lba;
	uint64 audio_start_time_ms;
	bool audio_paused;

	// Event status
	uint8 media_events;         // 0x02 = new media, 0x03 = media removal
	bool tray_open;
};

static CDROMDriveState g_cd_state[ESP_MAX_DEVS];

/* SCSI disk structure */
struct SCSIdiskst {
	FILE* dsk;

	uint32 size;
	bool cdrom;
	uint32 sector_size;         // 512 or 2048 (default 2048 for CD-ROM, 512 for HDD)
	uint8 lun;
	uint8 status;
	uint8 message;
	bool unit_attention;

	struct senset {
		uint8 key;
		uint8 code;
		uint8 ascq;
		bool valid;
		uint32 info;
	} sense;

	uint32 lba;
	uint32 blockcounter;
} SCSIdisk[ESP_MAX_DEVS];

struct scsi_bufferst {
	uint32 limit;
	uint32 size;
	bool disk;
} scsi_buffer;

/* Mode Pages */
#define MODEPAGE_MAX_SIZE 30

struct MODEPAGE {
	uint8 current[MODEPAGE_MAX_SIZE];
	uint8 changeable[MODEPAGE_MAX_SIZE];
	uint8 modepage[MODEPAGE_MAX_SIZE]; // default values
	uint8 saved[MODEPAGE_MAX_SIZE];
	uint8 pagesize;
};

typedef struct MODEPAGE MODEPAGE;

/* Global state */
static unsigned long buffer_size;
static uint8 *buffer = NULL;
static uint8 cmd_buffer[12];
static int scsi_CmdLength;
static int SENSE_LENGTH = 256;
static uint8 *sense_buffer = NULL;
static int target = 0;
static char scsi_paths[ESP_MAX_DEVS][1024];

/******* Prototypes ****************/
void SCSI_Emulate_Command(unsigned char *cdb);
void SCSIabort(void);
void SCSI_Inquiry(unsigned char *cdb);
int SCSI_GetTransferLength(uint8 opcode, uint8 *cdb);
void SCSI_TestUnitReady(uint8 *cdb);
void SCSI_StartStop(uint8 *cdb);
void SCSI_ReadCapacity(uint8 *cdb);
void scsi_read_sector(void);
void SCSI_ReadSector(uint8 *cdb);
unsigned long SCSI_GetOffset(uint8 opcode, uint8 *cdb);
int SCSI_GetCount(uint8 opcode, uint8 *cdb);
void SCSI_ModeSense(uint8 *cdb);
void SCSI_ModeSense10(uint8 *cdb);
void SCSI_ModeSelect(uint8 *cdb);
MODEPAGE SCSI_GetModePage(uint8 pagecode);
void SCSI_GuessGeometry(uint32 size, uint32 *cylinders, uint32 *heads, uint32 *sectors);
void SCSI_WriteSector(uint8 *cdb);
void scsi_write_sector(void);
void SCSI_RequestSense(uint8 *cdb);
void SCSI_ReadTOC(uint8 *cdb);
void SCSI_ReadHeader(uint8 *cdb);
void SCSI_ReadSubChannel(uint8 *cdb);
void SCSI_GetConfiguration(uint8 *cdb);
void SCSI_GetEventStatus(uint8 *cdb);
void SCSI_ReadDiscInformation(uint8 *cdb);
void SCSI_ReadTrackInformation(uint8 *cdb);
void SCSI_MechanismStatus(uint8 *cdb);
void SCSI_ReadCD(uint8 *cdb, bool is_msf);
void SCSI_AppleReadCDDA(uint8 *cdb, bool is_msf);
void SCSI_PlayAudio(uint8 *cdb);
void SCSI_PauseResume(uint8 *cdb);
void SCSI_StopAudio(uint8 *cdb);

static void CD_ClearDisc(CDROMDriveState &cd);
static bool CD_LoadCueSheet(int id, const char *cue_path);
static bool CD_SetupSingleFile(int id, const char *path, FILE *fh, uint64 fsize);
static const CDTrack *CD_FindTrackForLBA(const CDROMDriveState &cd, uint32 lba);
static bool CD_Read2048(int id, uint32 lba, uint32 block_count, uint8 *dest);
static bool CD_ReadRawSector(int id, uint32 lba, uint8 *dest_2352);
static void CD_UpdateAudioPlayback(int id);

/**********************************************************************************/
/* Conversion and Timing Helpers                                                  */
/**********************************************************************************/

static inline uint64 get_time_ms(void)
{
	auto now = std::chrono::steady_clock::now();
	return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

static inline void LBA_to_MSF(int32 lba, uint8 *msf, bool relative)
{
	if (!relative) {
		lba += 150;
	}
	uint32 ulba = (lba < 0) ? (uint32)(-lba) : (uint32)lba;
	msf[0] = (ulba / 75) / 60;   // Minutes
	msf[1] = (ulba / 75) % 60;   // Seconds
	msf[2] = ulba % 75;          // Frames
}

static inline void LBA_to_MSF_BCD(int32 lba, uint8 *msf, bool relative)
{
	LBA_to_MSF(lba, msf, relative);
	msf[0] = ((msf[0] / 10) << 4) | (msf[0] % 10);
	msf[1] = ((msf[1] / 10) << 4) | (msf[1] % 10);
	msf[2] = ((msf[2] / 10) << 4) | (msf[2] % 10);
}

static inline int32 MSF_to_LBA(uint8 m, uint8 s, uint8 f, bool relative)
{
	int32 lba = ((int32)m * 60 + (int32)s) * 75 + (int32)f;
	if (!relative) {
		lba -= 150;
	}
	return lba;
}

static void CD_UpdateAudioPlayback(int id)
{
	if (id < 0 || id >= ESP_MAX_DEVS) return;
	CDROMDriveState &cd = g_cd_state[id];
	if (cd.audio_status == 0x11 && !cd.audio_paused) {
		uint64 now = get_time_ms();
		uint64 elapsed_ms = (now > cd.audio_start_time_ms) ? (now - cd.audio_start_time_ms) : 0;
		uint32 elapsed_sectors = (uint32)((elapsed_ms * 75) / 1000);
		cd.audio_current_lba = cd.audio_start_lba + elapsed_sectors;
		if (cd.audio_current_lba >= cd.audio_end_lba) {
			cd.audio_current_lba = cd.audio_end_lba;
			cd.audio_status = 0x15; // Completed / stopped
			SCSI_LOG("[SCSI-CDROM] Target %d: Audio playback completed at LBA %u\n", id, cd.audio_end_lba);
		}
	}
}

/**********************************************************************************/
/* CD-ROM Disc & CUE Parsing Implementation                                       */
/**********************************************************************************/

static void CD_ClearDisc(CDROMDriveState &cd)
{
	for (int i = 0; i < cd.track_count; i++) {
		if (cd.tracks[i].fh) {
			FILE *fh = cd.tracks[i].fh;
			fclose(fh);
			for (int j = i + 1; j < cd.track_count; j++) {
				if (cd.tracks[j].fh == fh) {
					cd.tracks[j].fh = NULL;
				}
			}
			cd.tracks[i].fh = NULL;
		}
	}
	memset(&cd, 0, sizeof(cd));
	cd.audio_status = 0x15;
}

static bool CD_SetupSingleFile(int id, const char *path, FILE *fh, uint64 fsize)
{
	CDROMDriveState &cd = g_cd_state[id];
	CD_ClearDisc(cd);
	cd.has_cue = false;
	cd.track_count = 1;

	CDTrack &t = cd.tracks[0];
	t.track_num = 1;
	t.type = CD_MODE_MODE1_2048;
	t.sector_size = 2048;
	t.start_lba = 0;
	t.data_start_lba = 0;
	t.file_offset = 0;
	t.length_sectors = (uint32)(fsize / 2048);
	t.fh = fh;
	strncpy(t.filename, path, sizeof(t.filename) - 1);

	cd.leadout_lba = t.length_sectors;
	if (cd.leadout_lba == 0) cd.leadout_lba = 1;

	cd.audio_status = 0x15;
	cd.audio_paused = false;
	cd.media_events = 0x02;

	SCSI_LOG("[SCSI-CDROM] Target %d: Setup single CD image '%s' (%u sectors / %u MB)\n",
	         id, path, cd.leadout_lba, (cd.leadout_lba * 2048) / (1024 * 1024));
	return true;
}

static bool CD_LoadCueSheet(int id, const char *cue_path)
{
	FILE *fp = fopen(cue_path, "r");
	if (!fp) return false;

	CDROMDriveState &cd = g_cd_state[id];
	CD_ClearDisc(cd);
	cd.has_cue = true;

	char cue_dir[1024] = {0};
	const char *last_slash = strrchr(cue_path, '/');
	const char *last_bslash = strrchr(cue_path, '\\');
	const char *sep = (last_slash > last_bslash) ? last_slash : last_bslash;
	if (sep) {
		size_t dlen = (size_t)(sep - cue_path + 1);
		if (dlen < sizeof(cue_dir)) {
			memcpy(cue_dir, cue_path, dlen);
			cue_dir[dlen] = '\0';
		}
	}

	char line[1024];
	int file_count = 0;

	struct TempTrack {
		int track_num;
		CDTrackType type;
		uint32 sector_size;
		int file_index;
		int32 index00_lba;
		int32 index01_lba;
		int32 pregap_lba;
	};
	std::vector<TempTrack> temp_tracks;

	struct FileEntry {
		char path[1024];
		uint64 size;
		FILE *fh;
	};
	std::vector<FileEntry> files;

	while (fgets(line, sizeof(line), fp)) {
		char *p = line;
		while (isspace((unsigned char)*p)) p++;
		if (*p == '\0' || *p == '#') continue;
		char *end = p + strlen(p) - 1;
		while (end > p && isspace((unsigned char)*end)) {
			*end = '\0';
			end--;
		}

		if (strncasecmp(p, "REM", 3) == 0 && (isspace((unsigned char)p[3]) || p[3] == '\0')) {
			continue;
		}

		if (strncasecmp(p, "FILE", 4) == 0 && isspace((unsigned char)p[4])) {
			char *fstart = p + 4;
			while (isspace((unsigned char)*fstart)) fstart++;
			char fname[1024] = {0};
			if (*fstart == '"') {
				fstart++;
				char *fend = strchr(fstart, '"');
				if (fend) {
					size_t flen = (size_t)(fend - fstart);
					if (flen >= sizeof(fname)) flen = sizeof(fname) - 1;
					memcpy(fname, fstart, flen);
					fname[flen] = '\0';
				}
			} else {
				char *fend = fstart;
				while (*fend && !isspace((unsigned char)*fend)) fend++;
				size_t flen = (size_t)(fend - fstart);
				if (flen >= sizeof(fname)) flen = sizeof(fname) - 1;
				memcpy(fname, fstart, flen);
				fname[flen] = '\0';
			}

			char full_path[1024] = {0};
			if (cue_dir[0] && fname[0] != '/' && fname[0] != '\\' && (fname[1] != ':')) {
				snprintf(full_path, sizeof(full_path), "%s%s", cue_dir, fname);
			} else {
				strncpy(full_path, fname, sizeof(full_path) - 1);
			}

			FILE *fh = fopen(full_path, "rb");
			if (!fh) {
				const char *base_name = strrchr(fname, '/');
				if (!base_name) base_name = strrchr(fname, '\\');
				base_name = base_name ? (base_name + 1) : fname;
				snprintf(full_path, sizeof(full_path), "%s%s", cue_dir, base_name);
				fh = fopen(full_path, "rb");
			}

			uint64 fsize = 0;
			if (fh) {
				fseek(fh, 0, SEEK_END);
				fsize = (uint64)ftell(fh);
				fseek(fh, 0, SEEK_SET);
			}

			FileEntry fe;
			strncpy(fe.path, full_path, sizeof(fe.path) - 1);
			fe.size = fsize;
			fe.fh = fh;
			files.push_back(fe);
			file_count++;
			continue;
		}

		if (strncasecmp(p, "TRACK", 5) == 0 && isspace((unsigned char)p[5])) {
			char *tstart = p + 5;
			while (isspace((unsigned char)*tstart)) tstart++;
			int tnum = atoi(tstart);
			while (*tstart && !isspace((unsigned char)*tstart)) tstart++;
			while (isspace((unsigned char)*tstart)) tstart++;

			CDTrackType ttype = CD_MODE_MODE1_2048;
			uint32 sec_size = 2048;

			if (strncasecmp(tstart, "AUDIO", 5) == 0) {
				ttype = CD_MODE_AUDIO;
				sec_size = 2352;
			} else if (strncasecmp(tstart, "MODE1/2048", 10) == 0) {
				ttype = CD_MODE_MODE1_2048;
				sec_size = 2048;
			} else if (strncasecmp(tstart, "MODE1/2352", 10) == 0) {
				ttype = CD_MODE_MODE1_2352;
				sec_size = 2352;
			} else if (strncasecmp(tstart, "MODE2/2352", 10) == 0) {
				ttype = CD_MODE_MODE2_2352;
				sec_size = 2352;
			}

			TempTrack tt;
			tt.track_num = tnum;
			tt.type = ttype;
			tt.sector_size = sec_size;
			tt.file_index = (file_count > 0) ? (file_count - 1) : 0;
			tt.index00_lba = -1;
			tt.index01_lba = -1;
			tt.pregap_lba = 0;
			temp_tracks.push_back(tt);
			continue;
		}

		if (strncasecmp(p, "INDEX", 5) == 0 && isspace((unsigned char)p[5])) {
			if (temp_tracks.empty()) continue;
			char *istart = p + 5;
			while (isspace((unsigned char)*istart)) istart++;
			int inum = atoi(istart);
			while (*istart && !isspace((unsigned char)*istart)) istart++;
			while (isspace((unsigned char)*istart)) istart++;

			int m = 0, s = 0, f = 0;
			if (sscanf(istart, "%d:%d:%d", &m, &s, &f) == 3) {
				int32 lba = (int32)MSF_to_LBA((uint8)m, (uint8)s, (uint8)f, true);
				if (inum == 0) {
					temp_tracks.back().index00_lba = lba;
				} else if (inum == 1) {
					temp_tracks.back().index01_lba = lba;
				}
			}
			continue;
		}

		if (strncasecmp(p, "PREGAP", 6) == 0 && isspace((unsigned char)p[6])) {
			if (temp_tracks.empty()) continue;
			char *gstart = p + 6;
			while (isspace((unsigned char)*gstart)) gstart++;
			int m = 0, s = 0, f = 0;
			if (sscanf(gstart, "%d:%d:%d", &m, &s, &f) == 3) {
				temp_tracks.back().pregap_lba = (int32)MSF_to_LBA((uint8)m, (uint8)s, (uint8)f, true);
			}
			continue;
		}
	}
	fclose(fp);

	if (temp_tracks.empty() || files.empty()) {
		for (size_t i = 0; i < files.size(); i++) {
			if (files[i].fh) fclose(files[i].fh);
		}
		return false;
	}

	bool is_multi_file = (files.size() > 1);
	cd.track_count = (int)temp_tracks.size();
	if (cd.track_count > MAX_TRACKS_PER_DISC) cd.track_count = MAX_TRACKS_PER_DISC;

	uint32 running_disc_lba = 0;

	for (int i = 0; i < cd.track_count; i++) {
		TempTrack &tt = temp_tracks[i];
		FileEntry &fe = files[tt.file_index];

		CDTrack &t = cd.tracks[i];
		t.track_num = tt.track_num;
		t.type = tt.type;
		t.sector_size = tt.sector_size;
		strncpy(t.filename, fe.path, sizeof(t.filename) - 1);
		t.fh = fe.fh;

		if (!is_multi_file) {
			int32 idx00 = (tt.index00_lba >= 0) ? tt.index00_lba : tt.index01_lba;
			int32 idx01 = (tt.index01_lba >= 0) ? tt.index01_lba : 0;
			t.start_lba = (uint32)idx00;
			t.data_start_lba = (uint32)idx01;
			t.file_offset = (uint64)idx01 * t.sector_size;

			if (i + 1 < cd.track_count) {
				TempTrack &next_tt = temp_tracks[i + 1];
				int32 next_idx = (next_tt.index00_lba >= 0) ? next_tt.index00_lba : next_tt.index01_lba;
				t.length_sectors = (next_idx > idx00) ? (uint32)(next_idx - idx00) : 0;
			} else {
				uint64 total_file_sectors = (uint64)(fe.size / t.sector_size);
				t.length_sectors = (total_file_sectors > (uint64)idx00) ? (uint32)(total_file_sectors - idx00) : 0;
			}
		} else {
			running_disc_lba += (uint32)tt.pregap_lba;
			t.start_lba = running_disc_lba;
			t.data_start_lba = running_disc_lba;
			t.file_offset = 0;
			t.length_sectors = (uint32)(fe.size / t.sector_size);
			running_disc_lba += t.length_sectors;
		}
	}

	const CDTrack &last_t = cd.tracks[cd.track_count - 1];
	cd.leadout_lba = last_t.start_lba + last_t.length_sectors;
	if (cd.leadout_lba == 0) cd.leadout_lba = 1;

	cd.audio_status = 0x15;
	cd.audio_paused = false;
	cd.media_events = 0x02;

	SCSI_LOG("[SCSI-CDROM] Target %d: Loaded CUE sheet '%s' with %d tracks (%u sectors / %u MB)\n",
	         id, cue_path, cd.track_count, cd.leadout_lba, (cd.leadout_lba * 2048) / (1024 * 1024));
	return true;
}

static const CDTrack *CD_FindTrackForLBA(const CDROMDriveState &cd, uint32 lba)
{
	if (cd.track_count <= 0) return NULL;
	for (int i = 0; i < cd.track_count; i++) {
		uint32 tend = cd.tracks[i].start_lba + cd.tracks[i].length_sectors;
		if (lba >= cd.tracks[i].start_lba && lba < tend) {
			return &cd.tracks[i];
		}
	}
	if (lba < cd.leadout_lba) {
		return &cd.tracks[cd.track_count - 1];
	}
	return NULL;
}

static bool CD_Read2048(int id, uint32 lba, uint32 block_count, uint8 *dest)
{
	CDROMDriveState &cd = g_cd_state[id];
	for (uint32 i = 0; i < block_count; i++) {
		uint32 cur_lba = lba + i;
		const CDTrack *t = CD_FindTrackForLBA(cd, cur_lba);
		if (!t || !t->fh) {
			SCSI_LOG("[SCSI-CDROM] Target %d: Failed to find track/handle for LBA %u\n", id, cur_lba);
			return false;
		}

		uint64 file_pos = 0;
		if (cur_lba >= t->data_start_lba) {
			file_pos = t->file_offset + (uint64)(cur_lba - t->data_start_lba) * t->sector_size;
		} else if (cur_lba >= t->start_lba) {
			file_pos = (uint64)cur_lba * t->sector_size;
		} else {
			file_pos = t->file_offset;
		}

		if (t->type == CD_MODE_MODE1_2048) {
			if (fseek(t->fh, (long)file_pos, SEEK_SET) != 0 || fread(dest + i * 2048, 2048, 1, t->fh) != 1) {
				SCSI_LOG("[SCSI-CDROM] Target %d: Read error MODE1_2048 at file pos %llu (LBA %u)\n", id, file_pos, cur_lba);
				return false;
			}
		} else if (t->type == CD_MODE_MODE1_2352) {
			if (fseek(t->fh, (long)(file_pos + 16), SEEK_SET) != 0 || fread(dest + i * 2048, 2048, 1, t->fh) != 1) {
				SCSI_LOG("[SCSI-CDROM] Target %d: Read error MODE1_2352 at file pos %llu (LBA %u)\n", id, file_pos + 16, cur_lba);
				return false;
			}
		} else if (t->type == CD_MODE_MODE2_2352) {
			if (fseek(t->fh, (long)(file_pos + 24), SEEK_SET) != 0 || fread(dest + i * 2048, 2048, 1, t->fh) != 1) {
				SCSI_LOG("[SCSI-CDROM] Target %d: Read error MODE2_2352 at file pos %llu (LBA %u)\n", id, file_pos + 24, cur_lba);
				return false;
			}
		} else if (t->type == CD_MODE_AUDIO) {
			if (fseek(t->fh, (long)file_pos, SEEK_SET) != 0 || fread(dest + i * 2048, 2048, 1, t->fh) != 1) {
				SCSI_LOG("[SCSI-CDROM] Target %d: Read error AUDIO at file pos %llu (LBA %u)\n", id, file_pos, cur_lba);
				return false;
			}
		}
	}
	return true;
}

static bool CD_ReadRawSector(int id, uint32 lba, uint8 *dest_2352)
{
	CDROMDriveState &cd = g_cd_state[id];
	const CDTrack *t = CD_FindTrackForLBA(cd, lba);
	if (!t || !t->fh) {
		memset(dest_2352, 0, 2352);
		return false;
	}

	uint64 file_pos = 0;
	if (lba >= t->data_start_lba) {
		file_pos = t->file_offset + (uint64)(lba - t->data_start_lba) * t->sector_size;
	} else if (lba >= t->start_lba) {
		file_pos = (uint64)lba * t->sector_size;
	} else {
		file_pos = t->file_offset;
	}

	if (t->type == CD_MODE_AUDIO || t->type == CD_MODE_MODE1_2352 || t->type == CD_MODE_MODE2_2352) {
		if (fseek(t->fh, (long)file_pos, SEEK_SET) != 0 || fread(dest_2352, 2352, 1, t->fh) != 1) {
			memset(dest_2352, 0, 2352);
			return false;
		}
		return true;
	} else if (t->type == CD_MODE_MODE1_2048) {
		dest_2352[0] = 0x00;
		memset(dest_2352 + 1, 0xFF, 10);
		dest_2352[11] = 0x00;
		LBA_to_MSF_BCD((int32)lba, dest_2352 + 12, false);
		dest_2352[15] = 0x01; // Mode 1
		if (fseek(t->fh, (long)file_pos, SEEK_SET) != 0 || fread(dest_2352 + 16, 2048, 1, t->fh) != 1) {
			memset(dest_2352 + 16, 0, 2048);
		}
		memset(dest_2352 + 16 + 2048, 0, 288);
		return true;
	}
	return false;
}

/**********************************************************************************/
/* SCSI Bus & Device Initialization                                               */
/**********************************************************************************/

void SCSIInit(void)
{
	char prefs_name[16];

	g_scsi_debug = PrefsFindBool("scsi_debug");
	SCSI_LOG("[SCSI-EMU] SCSIInit() initializing subsystem (scsi_debug=%d)\n", g_scsi_debug);

	buffer = (uint8 *)malloc(buffer_size = 4 * 1024 * 1024);
	sense_buffer = (uint8*)malloc(SENSE_LENGTH + 1);

	memset(SCSIdisk, 0x0, sizeof(SCSIdisk));
	memset(scsi_paths, 0x0, sizeof(scsi_paths));
	for (int i = 0; i < ESP_MAX_DEVS; i++) {
		CD_ClearDisc(g_cd_state[i]);
		SCSIdisk[i].sector_size = BLOCKSIZE;
	}

	for (int count = 0; count < 7; count++) {
		snprintf(prefs_name, sizeof(prefs_name), "scsi%d", count);
		const char *str = PrefsFindString(prefs_name);
		if (str != NULL && str[0] != '\0') {
			SCSI_Attach(count, str);
		}
	}

	for (int i = 0; i < ESP_MAX_DEVS; i++) {
		SCSIdisk[i].unit_attention = false;
	}

	SCSIReset();
	MenuBar_UpdateAll();
}

void SCSIExit(void)
{
	SCSI_LOG("[SCSI-EMU] SCSIExit()\n");
	for (int count = 0; count < 7; count++) {
		SCSI_Detach(count);
	}
}

bool SCSI_Attach(int id, const char *path)
{
	if (id < 0 || id >= ESP_MAX_DEVS || !path || !path[0])
		return false;

	bool was_cd = SCSIdisk[id].cdrom;

	if (SCSIdisk[id].cdrom) {
		CD_ClearDisc(g_cd_state[id]);
		SCSIdisk[id].dsk = NULL;
	} else if (SCSIdisk[id].dsk != NULL) {
		fclose(SCSIdisk[id].dsk);
		SCSIdisk[id].dsk = NULL;
	}
	SCSIdisk[id].size = 0;

	size_t len = strlen(path);
	bool is_cd = was_cd;
	if (!is_cd) {
		if (id == 3 || id == 6)
			is_cd = true;
		if (len >= 4) {
			const char *ext = path + len - 4;
			if (strcasecmp(ext, ".iso") == 0 || strcasecmp(ext, ".cdr") == 0 || strcasecmp(ext, ".cue") == 0)
				is_cd = true;
			else if (strcasecmp(ext, ".hda") == 0 || strcasecmp(ext, ".dsk") == 0 || strcasecmp(ext, ".img") == 0 || strcasecmp(ext, ".vhd") == 0)
				is_cd = false;
		}
		if (len >= 6 && strcasecmp(path + len - 6, ".toast") == 0)
			is_cd = true;
	}

	if (is_cd) {
		bool loaded = false;
		if (len >= 4 && strcasecmp(path + len - 4, ".cue") == 0) {
			loaded = CD_LoadCueSheet(id, path);
		} else {
			char companion_cue[1024] = {0};
			strncpy(companion_cue, path, sizeof(companion_cue) - 5);
			char *dot = strrchr(companion_cue, '.');
			if (dot) *dot = '\0';
			strcat(companion_cue, ".cue");

			FILE *test_cue = fopen(companion_cue, "r");
			if (test_cue) {
				fclose(test_cue);
				loaded = CD_LoadCueSheet(id, companion_cue);
			}

			if (!loaded) {
				FILE *fh = fopen(path, "rb");
				if (fh) {
					fseek(fh, 0, SEEK_END);
					uint64 fsize = (uint64)ftell(fh);
					fseek(fh, 0, SEEK_SET);
					loaded = CD_SetupSingleFile(id, path, fh, fsize);
				}
			}
		}

		if (!loaded) {
			scsi_paths[id][0] = '\0';
			SCSI_LOG("[SCSI-EMU] Target %d: Error opening CD-ROM image [%s]\n", id, path);
			return false;
		}

		SCSIdisk[id].dsk = g_cd_state[id].tracks[0].fh;
		SCSIdisk[id].size = g_cd_state[id].leadout_lba * CDBLOCKSIZE;
		SCSIdisk[id].cdrom = true;
		SCSIdisk[id].sector_size = CDBLOCKSIZE;
	} else {
		SCSIdisk[id].dsk = fopen(path, "rb+");
		if (SCSIdisk[id].dsk == NULL)
			SCSIdisk[id].dsk = fopen(path, "rb");
		SCSIdisk[id].cdrom = false;
		SCSIdisk[id].sector_size = BLOCKSIZE;

		if (SCSIdisk[id].dsk == NULL) {
			scsi_paths[id][0] = '\0';
			SCSI_LOG("[SCSI-EMU] Target %d: Error opening disk image [%s]\n", id, path);
			return false;
		}

		fseek(SCSIdisk[id].dsk, 0, SEEK_END);
		SCSIdisk[id].size = ftell(SCSIdisk[id].dsk);
		fseek(SCSIdisk[id].dsk, 0, SEEK_SET);
	}

	SCSIdisk[id].lun = 0;
	SCSIdisk[id].status = STAT_GOOD;
	SCSIdisk[id].sense.code = SC_NO_ERROR;
	SCSIdisk[id].sense.valid = false;
	SCSIdisk[id].unit_attention = true;

	strncpy(scsi_paths[id], path, sizeof(scsi_paths[id]) - 1);
	scsi_paths[id][sizeof(scsi_paths[id]) - 1] = '\0';

	char prefs_name[16];
	snprintf(prefs_name, sizeof(prefs_name), "scsi%d", id);
	PrefsReplaceString(prefs_name, path);

	unsigned int cylinders, heads, sectors;
	if (!SCSIdisk[id].cdrom) {
		SCSI_GuessGeometry(SCSIdisk[id].size / BLOCKSIZE, &cylinders, &heads, &sectors);
		SCSI_LOG("[SCSI-EMU] Target %d (Hard Disk) attached: '%s' (%u bytes, %d cyl, %d heads, %d sec)\n",
		         id, path, SCSIdisk[id].size, cylinders, heads, sectors);
	} else {
		SCSI_LOG("[SCSI-EMU] Target %d (CD-ROM) attached: '%s' (%u sectors, %u MB, %d tracks)\n",
		         id, path, g_cd_state[id].leadout_lba, (g_cd_state[id].leadout_lba * 2048) / (1024 * 1024), g_cd_state[id].track_count);
	}

	return true;
}

bool SCSI_Detach(int id)
{
	if (id < 0 || id >= ESP_MAX_DEVS)
		return false;

	if (SCSIdisk[id].cdrom) {
		CD_ClearDisc(g_cd_state[id]);
		SCSIdisk[id].dsk = NULL;
		SCSIdisk[id].cdrom = false;
		SCSIdisk[id].unit_attention = false;
		g_cd_state[id].media_events = 0x03; // Media removal
	} else if (SCSIdisk[id].dsk != NULL) {
		fclose(SCSIdisk[id].dsk);
		SCSIdisk[id].dsk = NULL;
	}
	SCSIdisk[id].size = 0;
	scsi_paths[id][0] = '\0';

	char prefs_name[16];
	snprintf(prefs_name, sizeof(prefs_name), "scsi%d", id);
	PrefsRemoveItem(prefs_name);

	SCSI_LOG("[SCSI-EMU] Target %d: Detached\n", id);
	return true;
}

bool SCSI_GetDeviceInfo(int id, bool *present, bool *cdrom, char *path_out, size_t max_len)
{
	if (id < 0 || id >= ESP_MAX_DEVS) {
		if (present) *present = false;
		if (cdrom) *cdrom = false;
		if (path_out && max_len > 0)
			path_out[0] = '\0';
		return false;
	}

	bool is_present = false;
	if (SCSIdisk[id].cdrom) {
		is_present = (g_cd_state[id].track_count > 0 && g_cd_state[id].tracks[0].fh != NULL);
	} else {
		is_present = (SCSIdisk[id].dsk != NULL);
	}

	if (present) *present = is_present;
	if (cdrom) *cdrom = SCSIdisk[id].cdrom;
	if (path_out && max_len > 0) {
		if (is_present && scsi_paths[id][0] != '\0') {
			strncpy(path_out, scsi_paths[id], max_len - 1);
			path_out[max_len - 1] = '\0';
		} else {
			path_out[0] = '\0';
		}
	}
	return true;
}

void scsi_set_cmd(int cmd_length, uint8 *cmd)
{
	memcpy(cmd_buffer, cmd, cmd_length);
	scsi_CmdLength = cmd_length;
}

bool scsi_is_target_present(int id)
{
	if (id < 0 || id >= ESP_MAX_DEVS)
		return false;
	if (SCSIdisk[id].cdrom)
		return (g_cd_state[id].track_count > 0 && g_cd_state[id].tracks[0].fh != NULL) || (scsi_paths[id][0] != '\0');
	return (SCSIdisk[id].dsk != NULL);
}

bool scsi_set_target(int id, int lun)
{
	if (id >= 0 && id < ESP_MAX_DEVS && scsi_is_target_present(id) && (lun == 0)) {
		target = id;
		return true;
	} else {
		return false;
	}
}

static bool try_buffer(int size)
{
	if (size <= (int)buffer_size)
		return true;
	uint8 *new_buffer = (uint8 *)realloc(buffer, size);
	if (new_buffer == NULL)
		return false;
	buffer = new_buffer;
	buffer_size = size;
	return true;
}

bool scsi_send_cmd(size_t data_length, bool reading, int sg_size, uint8 **sg_ptr, uint32 *sg_len, uint16 *stat, uint32 timeout)
{
	if (!try_buffer(data_length)) {
		printf("SCSI BUFFER ERROR!\n");
		return false;
	}

	scsi_buffer.size = 0;
	scsi_buffer.limit = 0;

	if (!reading) {
		uint8 *buffer_ptr = buffer;
		for (int i = 0; i < sg_size; i++) {
			uint32 len = sg_len[i];
			memcpy(buffer_ptr, sg_ptr[i], len);
			buffer_ptr += len;
		}
	}

	SCSI_Emulate_Command(cmd_buffer);
	*stat = SCSIdisk[target].status;

	if (reading) {
		uint8 *buffer_ptr = buffer;
		size_t remaining_valid = scsi_buffer.size;
		for (int i = 0; i < sg_size; i++) {
			uint32 len = sg_len[i];
			if (remaining_valid >= len) {
				memcpy(sg_ptr[i], buffer_ptr, len);
				buffer_ptr += len;
				remaining_valid -= len;
			} else if (remaining_valid > 0) {
				memcpy(sg_ptr[i], buffer_ptr, remaining_valid);
				memset(sg_ptr[i] + remaining_valid, 0, len - remaining_valid);
				buffer_ptr += remaining_valid;
				remaining_valid = 0;
			} else {
				memset(sg_ptr[i], 0, len);
			}
		}

		if (SCSIdisk[target].cdrom && sg_size > 0 && sg_len[0] > 0 && buffer != NULL) {
			char hex_prev[128] = {0};
			char ascii_prev[36] = {0};
			size_t prev_len = sg_len[0] < 32 ? sg_len[0] : 32;
			for (size_t d = 0; d < prev_len; d++) {
				char byte_hex[4];
				snprintf(byte_hex, sizeof(byte_hex), "%02X ", buffer[d]);
				strncat(hex_prev, byte_hex, sizeof(hex_prev) - strlen(hex_prev) - 1);
				ascii_prev[d] = (buffer[d] >= 32 && buffer[d] <= 126) ? (char)buffer[d] : '.';
			}
			ascii_prev[prev_len] = '\0';
			SCSI_LOG("[SCSI-CDROM] Target %d -> Host Mac Transfer Preview (%u bytes transferred): [%s] | '%s'\n",
			         target, sg_len[0], hex_prev, ascii_prev);
		}
	}

	return true;
}

/**********************************************************************************/
/* SCSI Command Dispatcher                                                        */
/**********************************************************************************/

void SCSI_Emulate_Command(unsigned char *cdb)
{
	unsigned char opcode = cdb[0];

	SCSI_LOG("[SCSI-EMU] Executing Opcode 0x%02X (%s) on Target %d LUN %d\n",
	         opcode, scsi_cmd_name(opcode), target, SCSIdisk[target].lun);

	switch (opcode) {
		case CMD_INQUIRY:
			SCSI_Inquiry(cdb);
			break;
		case CMD_REQ_SENSE:
			SCSI_RequestSense(cdb);
			break;
		default:
			if (SCSIdisk[target].lun != LUN_DISK) {
				SCSI_LOG("[SCSI-EMU] Target %d: Invalid LUN %d -> STAT_CHECK_COND (INVALID_LUN)\n", target, SCSIdisk[target].lun);
				SCSIdisk[target].status = STAT_CHECK_COND;
				SCSIdisk[target].message = MSG_COMPLETE;
				SCSIdisk[target].sense.key = SK_ILLEGAL_REQ;
				SCSIdisk[target].sense.code = SC_INVALID_LUN;
				SCSIdisk[target].sense.valid = false;
				return;
			}

			if (SCSIdisk[target].unit_attention && opcode != CMD_INQUIRY && opcode != CMD_REQ_SENSE) {
				SCSIdisk[target].unit_attention = false;
				SCSIdisk[target].status = STAT_CHECK_COND;
				SCSIdisk[target].sense.key = SK_UNIT_ATN;
				SCSIdisk[target].sense.code = SC_NOT_READY_TO_READY_TRANSITION;
				SCSIdisk[target].sense.ascq = 0x00;
				SCSIdisk[target].sense.valid = false;
				SCSI_LOG("[SCSI-EMU] Target %d: Unit Attention triggered on opcode 0x%02X (%s) -> STAT_CHECK_COND (Key 0x06 / ASC 0x28 Medium Changed)\n",
				         target, opcode, scsi_cmd_name(opcode));
				return;
			}

			switch (opcode) {
				case CMD_TEST_UNIT_RDY:
					SCSI_TestUnitReady(cdb);
					break;
				case CMD_REZERO_UNIT:
					if (SCSIdisk[target].cdrom) {
						SCSI_StopAudio(cdb);
					}
					SCSIdisk[target].status = STAT_GOOD;
					SCSIdisk[target].sense.code = SC_NO_ERROR;
					SCSIdisk[target].sense.valid = false;
					break;
				case CMD_READ_CAPACITY1:
					SCSI_ReadCapacity(cdb);
					break;
				case CMD_READ_SECTOR:
				case CMD_READ_SECTOR1:
				case CMD_READ12:
					SCSI_ReadSector(cdb);
					break;
				case CMD_WRITE_SECTOR:
				case CMD_WRITE_SECTOR1:
				case CMD_WRITE12:
					SCSI_WriteSector(cdb);
					break;
				case CMD_SEEK:
				case CMD_SEEK10:
					if (SCSIdisk[target].cdrom) {
						CD_UpdateAudioPlayback(target);
					}
					SCSIdisk[target].status = STAT_GOOD;
					SCSIdisk[target].sense.code = SC_NO_ERROR;
					SCSIdisk[target].sense.valid = false;
					break;
				case CMD_START_STOP:
					SCSI_StartStop(cdb);
					break;
				case CMD_MODESELECT:
				case CMD_MODESELECT10:
					SCSI_ModeSelect(cdb);
					break;
				case CMD_MODESENSE:
					SCSI_ModeSense(cdb);
					break;
				case CMD_MODESENSE10:
					SCSI_ModeSense10(cdb);
					break;
				case CMD_RESERVE:
				case CMD_RELEASE:
				case CMD_RESERVE10:
				case CMD_RELEASE10:
					SCSIdisk[target].status = STAT_GOOD;
					SCSIdisk[target].sense.code = SC_NO_ERROR;
					SCSIdisk[target].sense.valid = false;
					break;
				case CMD_SYNCHRONIZE_CACHE:
					SCSIdisk[target].status = STAT_GOOD;
					SCSIdisk[target].sense.code = SC_NO_ERROR;
					SCSIdisk[target].sense.valid = false;
					break;
				case CMD_FORMAT_DRIVE:
					if (SCSIdisk[target].cdrom) {
						SCSIdisk[target].status = STAT_CHECK_COND;
						SCSIdisk[target].sense.code = SC_WRITE_PROTECT;
						SCSIdisk[target].sense.valid = false;
					} else {
						SCSIdisk[target].status = STAT_GOOD;
						SCSIdisk[target].sense.code = SC_NO_ERROR;
						SCSIdisk[target].sense.valid = false;
					}
					break;
				case CMD_READ_TOC:
					SCSI_ReadTOC(cdb);
					break;
				case SCSI_READHEADER:
					SCSI_ReadHeader(cdb);
					break;
				case SCSI_SUBCHANNEL:
					SCSI_ReadSubChannel(cdb);
					break;
				case CMD_GET_CONFIGURATION:
					SCSI_GetConfiguration(cdb);
					break;
				case CMD_GET_EVENT_STATUS:
					SCSI_GetEventStatus(cdb);
					break;
				case CMD_READ_DISC_INFO:
					SCSI_ReadDiscInformation(cdb);
					break;
				case CMD_READ_TRACK_INFO:
					SCSI_ReadTrackInformation(cdb);
					break;
				case CMD_MECHANISM_STATUS:
					SCSI_MechanismStatus(cdb);
					break;
				case CMD_SET_CD_SPEED:
					SCSIdisk[target].status = STAT_GOOD;
					SCSIdisk[target].sense.code = SC_NO_ERROR;
					SCSIdisk[target].sense.valid = false;
					break;
				case CMD_READ_CD:
					SCSI_ReadCD(cdb, false);
					break;
				case CMD_READ_CD_MSF:
					SCSI_ReadCD(cdb, true);
					break;
				case CMD_APPLE_CDDA:
					SCSI_AppleReadCDDA(cdb, false);
					break;
				case CMD_APPLE_CDDA_MSF:
					SCSI_AppleReadCDDA(cdb, true);
					break;
				case CMD_APPLE_FF_REW:
					SCSIdisk[target].status = STAT_GOOD;
					SCSIdisk[target].sense.code = SC_NO_ERROR;
					SCSIdisk[target].sense.valid = false;
					break;
				case CMD_PREVENT_ALLOW:
					SCSI_LOG("[SCSI-EMU] Target %d PREVENT_ALLOW_MEDIUM_REMOVAL: Prevent=%d\n", target, cdb[4] & 1);
					SCSIdisk[target].status = STAT_GOOD;
					SCSIdisk[target].sense.code = SC_NO_ERROR;
					SCSIdisk[target].sense.valid = false;
					break;
				case SCSI_PLAYAUD_10:
				case SCSI_PLAYAUD_12:
				case SCSI_PLAYAUDMSF:
				case SCSI_PLAYA_TKIN:
				case SCSI_PLYTKREL10:
				case SCSI_PLYTKREL12:
					SCSI_PlayAudio(cdb);
					break;
				case SCSI_PAUSE_RESUME:
					SCSI_PauseResume(cdb);
					break;
				case SCSI_STOP_PLAY_SCAN:
					SCSI_StopAudio(cdb);
					break;
				default:
					SCSI_LOG("[SCSI-EMU] Target %d: UNKNOWN Opcode 0x%02X -> STAT_CHECK_COND (INVALID_CMD)\n", target, opcode);
					SCSIdisk[target].status = STAT_CHECK_COND;
					SCSIdisk[target].sense.code = SC_INVALID_CMD;
					SCSIdisk[target].sense.valid = false;
					break;
			}
			break;
	}

	SCSIdisk[target].message = MSG_COMPLETE;
}

void SCSIabort(void)
{
}

void SCSI_Inquiry(unsigned char *cdb)
{
	scsi_buffer.disk = false;
	int max_inq = SCSIdisk[target].cdrom ? (int)sizeof(CDinquiry_bytes) : (int)sizeof(inquiry_bytes);
	scsi_buffer.limit = scsi_buffer.size = SCSI_GetTransferLength(cdb[0], cdb);
	if ((int)scsi_buffer.limit > max_inq) {
		scsi_buffer.limit = scsi_buffer.size = max_inq;
	}

	if (SCSIdisk[target].cdrom)
		memcpy(buffer, CDinquiry_bytes, scsi_buffer.limit);
	else
		memcpy(buffer, inquiry_bytes, scsi_buffer.limit);

	if (SCSIdisk[target].lun != LUN_DISK) {
		buffer[0] = DEVTYPE_NOTPRESENT;
	}

	SCSI_LOG("[SCSI-EMU] Target %d INQUIRY: returning %d bytes -> DeviceType=0x%02X (%s), RMB=0x%02X, Vendor '%.8s', Product '%.16s', Rev '%.4s'\n",
	         target, scsi_buffer.limit, buffer[0], SCSIdisk[target].cdrom ? "CD-ROM" : "DISK", buffer[1],
	         (char*)buffer + 8, (char*)buffer + 16, (char*)buffer + 32);

	SCSIdisk[target].status = STAT_GOOD;
	SCSIdisk[target].sense.code = SC_NO_ERROR;
	SCSIdisk[target].sense.valid = false;
}

int SCSI_GetTransferLength(uint8 opcode, uint8 *cdb)
{
	return opcode < 0x20 ? cdb[4] : COMMAND_ReadInt16(cdb, 7);
}

void SCSI_TestUnitReady(uint8 *cdb)
{
	bool ready = false;
	if (SCSIdisk[target].cdrom) {
		ready = (!g_cd_state[target].tray_open && g_cd_state[target].track_count > 0 && g_cd_state[target].tracks[0].fh != NULL);
	} else {
		ready = (SCSIdisk[target].dsk != NULL);
	}

	if (!ready) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.key = SK_NOTREADY;
		SCSIdisk[target].sense.code = SC_MEDIUM_NOT_PRESENT;
		SCSIdisk[target].sense.ascq = 0x00;
		SCSIdisk[target].sense.valid = false;
		SCSI_LOG("[SCSI-EMU] Target %d TEST_UNIT_READY -> STAT_CHECK_COND (Key 0x02 / ASC 0x3A Medium Not Present)\n", target);
	} else {
		SCSIdisk[target].status = STAT_GOOD;
		SCSIdisk[target].sense.key = SK_NOSENSE;
		SCSIdisk[target].sense.code = SC_NO_ERROR;
		SCSIdisk[target].sense.ascq = 0x00;
		SCSIdisk[target].sense.valid = false;
		SCSI_LOG("[SCSI-EMU] Target %d TEST_UNIT_READY -> STAT_GOOD (Ready)\n", target);
	}
}

void SCSI_StartStop(uint8 *cdb)
{
	uint8 loej = (cdb[4] & 0x02);
	uint8 start = (cdb[4] & 0x01);

	if (SCSIdisk[target].cdrom) {
		CD_UpdateAudioPlayback(target);
		g_cd_state[target].audio_status = 0x15; // Stopped
		g_cd_state[target].audio_paused = false;

		if (loej) {
			if (start) {
				g_cd_state[target].tray_open = false;
				g_cd_state[target].media_events = 0x02; // New media
			} else {
				SCSI_LOG("[SCSI-EMU] Target %d START_STOP_UNIT: Guest OS requested disc ejection\n", target);
				CD_ClearDisc(g_cd_state[target]);
				SCSIdisk[target].dsk = NULL;
				SCSIdisk[target].size = 0;
				scsi_paths[target][0] = '\0';
				g_cd_state[target].tray_open = true;
				g_cd_state[target].media_events = 0x03; // Media removal

				char prefs_name[16];
				snprintf(prefs_name, sizeof(prefs_name), "scsi%d", target);
				PrefsRemoveItem(prefs_name);
				MenuBar_UpdateAll();
			}
		}
		SCSI_LOG("[SCSI-EMU] Target %d START_STOP_UNIT: loej=%d, start=%d (Tray is %s)\n",
		         target, loej ? 1 : 0, start ? 1 : 0, g_cd_state[target].tray_open ? "OPEN" : "CLOSED");
	} else {
		SCSI_LOG("[SCSI-EMU] Target %d START_STOP_UNIT: start=%d\n", target, start ? 1 : 0);
	}

	SCSIdisk[target].status = STAT_GOOD;
	SCSIdisk[target].sense.code = SC_NO_ERROR;
	SCSIdisk[target].sense.valid = false;
}

void SCSI_ReadCapacity(uint8 *cdb)
{
	if (SCSIdisk[target].cdrom && (g_cd_state[target].tray_open || g_cd_state[target].track_count == 0 || g_cd_state[target].tracks[0].fh == NULL)) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.key = SK_NOTREADY;
		SCSIdisk[target].sense.code = SC_MEDIUM_NOT_PRESENT;
		SCSIdisk[target].sense.ascq = 0x00;
		SCSIdisk[target].sense.valid = false;
		scsi_buffer.limit = scsi_buffer.size = 0;
		SCSI_LOG("[SCSI-EMU] Target %d READ_CAPACITY: empty drive -> STAT_CHECK_COND (Key 0x02 / ASC 0x3A Medium Not Present)\n", target);
		return;
	}

	int blocksize = SCSIdisk[target].cdrom ? (SCSIdisk[target].sector_size ? SCSIdisk[target].sector_size : CDBLOCKSIZE) : BLOCKSIZE;
	uint32 total_sectors = 0;
	if (SCSIdisk[target].cdrom) {
		if (blocksize == 512) {
			total_sectors = g_cd_state[target].leadout_lba * 4;
		} else {
			total_sectors = g_cd_state[target].leadout_lba;
		}
	} else {
		total_sectors = SCSIdisk[target].size / blocksize;
	}

	uint32 last_lba = (total_sectors > 0) ? (total_sectors - 1) : 0;

	static uint8 scsi_disksize[8];
	scsi_disksize[0] = (last_lba >> 24) & 0xFF;
	scsi_disksize[1] = (last_lba >> 16) & 0xFF;
	scsi_disksize[2] = (last_lba >> 8) & 0xFF;
	scsi_disksize[3] = last_lba & 0xFF;
	scsi_disksize[4] = (blocksize >> 24) & 0xFF;
	scsi_disksize[5] = (blocksize >> 16) & 0xFF;
	scsi_disksize[6] = (blocksize >> 8) & 0xFF;
	scsi_disksize[7] = blocksize & 0xFF;

	memcpy(buffer, scsi_disksize, 8);
	scsi_buffer.limit = scsi_buffer.size = 8;
	scsi_buffer.disk = false;

	SCSI_LOG("[SCSI-EMU] Target %d READ_CAPACITY: last_lba=%u, block_size=%u (Total %u MB, %u sectors)\n",
	         target, last_lba, blocksize, ((last_lba + 1) * blocksize) / (1024 * 1024), total_sectors);

	SCSIdisk[target].status = STAT_GOOD;
	SCSIdisk[target].sense.code = SC_NO_ERROR;
	SCSIdisk[target].sense.valid = false;
}

void SCSI_ReadSector(uint8 *cdb)
{
	int blocksize = SCSIdisk[target].cdrom ? (SCSIdisk[target].sector_size ? SCSIdisk[target].sector_size : CDBLOCKSIZE) : BLOCKSIZE;
	SCSIdisk[target].lba = SCSI_GetOffset(cdb[0], cdb);
	SCSIdisk[target].blockcounter = SCSI_GetCount(cdb[0], cdb);
	scsi_buffer.disk = true;
	scsi_buffer.size = 0;

	SCSI_LOG("[SCSI-EMU] Target %d READ: %u block(s) at LBA %u (blocksize %u bytes, %s mode)\n",
	         target, SCSIdisk[target].blockcounter, SCSIdisk[target].lba, blocksize,
	         SCSIdisk[target].cdrom ? (blocksize == 512 ? "512-HFS" : "2048-CD") : "HDD");

	scsi_read_sector();
}

void scsi_read_sector(void)
{
	int loop = 0;
	int blocksize = SCSIdisk[target].cdrom ? (SCSIdisk[target].sector_size ? SCSIdisk[target].sector_size : CDBLOCKSIZE) : BLOCKSIZE;

	if (SCSIdisk[target].blockcounter == 0) {
		return;
	}

	uint32 total_blocks = SCSIdisk[target].blockcounter;
	if (!try_buffer(total_blocks * blocksize)) {
		SCSI_LOG("[SCSI-EMU] Target %d: Buffer allocation error (%u bytes) in read sector!\n", target, total_blocks * blocksize);
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.key = SK_HARDWARE;
		SCSIdisk[target].sense.code = SC_NO_ERROR;
		SCSIdisk[target].sense.valid = false;
		return;
	}

	if (SCSIdisk[target].cdrom) {
		if (g_cd_state[target].tray_open || g_cd_state[target].track_count == 0 || g_cd_state[target].tracks[0].fh == NULL) {
			SCSIdisk[target].status = STAT_CHECK_COND;
			SCSIdisk[target].sense.key = SK_NOTREADY;
			SCSIdisk[target].sense.code = SC_MEDIUM_NOT_PRESENT;
			SCSIdisk[target].sense.ascq = 0x00;
			SCSIdisk[target].sense.valid = false;
			scsi_buffer.limit = scsi_buffer.size = 0;
			SCSI_LOG("[SCSI-EMU] Target %d: Read sector failed - no disc present\n", target);
			return;
		}

		uint32 req_lba = SCSIdisk[target].lba;
		uint32 req_count = SCSIdisk[target].blockcounter;

		if (blocksize == 2048) {
			if (CD_Read2048(target, req_lba, req_count, buffer)) {
				SCSIdisk[target].status = STAT_GOOD;
				SCSIdisk[target].sense.code = SC_NO_ERROR;
				SCSIdisk[target].sense.valid = false;
				SCSIdisk[target].lba += req_count;
				SCSIdisk[target].blockcounter = 0;
				scsi_buffer.limit = scsi_buffer.size = req_count * 2048;
			} else {
				SCSI_LOG("[SCSI-EMU] Target %d: READ ERROR at 2048-LBA %u (count %u)!\n", target, req_lba, req_count);
				SCSIdisk[target].status = STAT_CHECK_COND;
				SCSIdisk[target].sense.code = SC_INVALID_LBA;
				SCSIdisk[target].sense.valid = true;
				SCSIdisk[target].sense.info = req_lba;
				scsi_buffer.limit = scsi_buffer.size = 0;
			}
		} else if (blocksize == 512) {
			uint8 temp2048[2048];
			bool ok = true;
			for (uint32 b = 0; b < req_count; b++) {
				uint32 cur_512_lba = req_lba + b;
				uint32 cd_lba = cur_512_lba / 4;
				uint32 cd_offset = (cur_512_lba % 4) * 512;
				if (!CD_Read2048(target, cd_lba, 1, temp2048)) {
					ok = false;
					SCSI_LOG("[SCSI-EMU] Target %d: READ ERROR at 512-LBA %u (CD LBA %u)!\n", target, cur_512_lba, cd_lba);
					SCSIdisk[target].status = STAT_CHECK_COND;
					SCSIdisk[target].sense.code = SC_INVALID_LBA;
					SCSIdisk[target].sense.valid = true;
					SCSIdisk[target].sense.info = cur_512_lba;
					break;
				}
				memcpy(buffer + b * 512, temp2048 + cd_offset, 512);
			}
			if (ok) {
				SCSIdisk[target].status = STAT_GOOD;
				SCSIdisk[target].sense.code = SC_NO_ERROR;
				SCSIdisk[target].sense.valid = false;
				SCSIdisk[target].lba += req_count;
				SCSIdisk[target].blockcounter = 0;
				scsi_buffer.limit = scsi_buffer.size = req_count * 512;
			}
		}

		if (scsi_buffer.size > 0) {
			char hex_dump[128] = {0};
			char ascii_dump[36] = {0};
			size_t dump_len = scsi_buffer.size < 32 ? scsi_buffer.size : 32;
			for (size_t d = 0; d < dump_len; d++) {
				char byte_hex[4];
				snprintf(byte_hex, sizeof(byte_hex), "%02X ", buffer[d]);
				strncat(hex_dump, byte_hex, sizeof(hex_dump) - strlen(hex_dump) - 1);
				ascii_dump[d] = (buffer[d] >= 32 && buffer[d] <= 126) ? (char)buffer[d] : '.';
			}
			ascii_dump[dump_len] = '\0';
			SCSI_LOG("[SCSI-CDROM] Target %d Sector Data Preview (LBA %u, %u bytes, blocksize %u): [%s] | '%s'\n",
			         target, req_lba, scsi_buffer.size, blocksize, hex_dump, ascii_dump);
		}
		return;
	}

	int n;
	while (SCSIdisk[target].blockcounter > 0) {
		if ((SCSIdisk[target].dsk == NULL) || (fseek(SCSIdisk[target].dsk, (long)SCSIdisk[target].lba * blocksize, SEEK_SET) != 0)) {
			n = 0;
		} else {
			n = fread(buffer + (loop * blocksize), blocksize, 1, SCSIdisk[target].dsk);
		}

		if (n == 1) {
			SCSIdisk[target].status = STAT_GOOD;
			SCSIdisk[target].sense.code = SC_NO_ERROR;
			SCSIdisk[target].sense.valid = false;
			SCSIdisk[target].lba++;
			SCSIdisk[target].blockcounter--;
		} else {
			SCSI_LOG("[SCSI-EMU] Target %d: HDD READ ERROR at LBA %u!\n", target, SCSIdisk[target].lba);
			SCSIdisk[target].status = STAT_CHECK_COND;
			SCSIdisk[target].sense.code = SC_INVALID_LBA;
			SCSIdisk[target].sense.valid = true;
			SCSIdisk[target].sense.info = SCSIdisk[target].lba;
			break;
		}
		loop++;
	}
	scsi_buffer.limit = scsi_buffer.size = loop * blocksize;
}

void SCSI_ReadTOC(uint8 *cdb)
{
	if (!SCSIdisk[target].cdrom) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.code = SC_INVALID_CMD;
		SCSIdisk[target].sense.valid = false;
		return;
	}

	if (g_cd_state[target].tray_open || g_cd_state[target].track_count == 0 || g_cd_state[target].tracks[0].fh == NULL) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.key = SK_NOTREADY;
		SCSIdisk[target].sense.code = SC_MEDIUM_NOT_PRESENT;
		SCSIdisk[target].sense.ascq = 0x00;
		SCSIdisk[target].sense.valid = false;
		scsi_buffer.limit = scsi_buffer.size = 0;
		SCSI_LOG("[SCSI-EMU] Target %d READ_TOC: empty drive -> STAT_CHECK_COND (Key 0x02 / ASC 0x3A Medium Not Present)\n", target);
		return;
	}

	CD_UpdateAudioPlayback(target);
	const CDROMDriveState &cd = g_cd_state[target];

	bool msf = (cdb[1] & 0x02) != 0;
	uint8 format = cdb[2] & 0x0F;
	bool use_bcd = false;

	if (format == 0) {
		if (cdb[9] == 0x80) {
			format = 2;
			use_bcd = true;
		} else if (cdb[9] == 0x40) {
			format = 1;
		}
	}

	uint8 start_track = cdb[6];
	int alloc_len = COMMAND_ReadInt16(cdb, 7);
	if (alloc_len == 0) alloc_len = 65535;

	uint8 resp[4096];
	memset(resp, 0, sizeof(resp));
	int total_len = 0;

	if (format == 1) {
		resp[0] = 0x00;
		resp[1] = 0x0A;
		resp[2] = 1;
		resp[3] = 1;
		resp[4] = 0x00;
		resp[5] = (cd.tracks[0].type == CD_MODE_AUDIO) ? 0x10 : 0x14;
		resp[6] = 1;
		resp[7] = 0x00;
		if (msf) {
			resp[8] = 0x00;
			resp[9] = 0x00;
			resp[10] = 0x02;
			resp[11] = 0x00;
		} else {
			resp[8] = 0x00;
			resp[9] = 0x00;
			resp[10] = 0x00;
			resp[11] = 0x00;
		}
		total_len = 12;
	} else if (format == 2) {
		int desc_offset = 4;

		// Point A0
		resp[desc_offset + 0] = 1;
		resp[desc_offset + 1] = (cd.tracks[0].type == CD_MODE_AUDIO) ? 0x10 : 0x14;
		resp[desc_offset + 2] = 0;
		resp[desc_offset + 3] = 0xA0;
		resp[desc_offset + 4] = 0;
		resp[desc_offset + 5] = 0;
		resp[desc_offset + 6] = 0;
		resp[desc_offset + 7] = 0;
		resp[desc_offset + 8] = 1;
		resp[desc_offset + 9] = 0x00;
		resp[desc_offset + 10] = 0;
		desc_offset += 11;

		// Point A1
		resp[desc_offset + 0] = 1;
		resp[desc_offset + 1] = (cd.tracks[cd.track_count - 1].type == CD_MODE_AUDIO) ? 0x10 : 0x14;
		resp[desc_offset + 2] = 0;
		resp[desc_offset + 3] = 0xA1;
		resp[desc_offset + 4] = 0;
		resp[desc_offset + 5] = 0;
		resp[desc_offset + 6] = 0;
		resp[desc_offset + 7] = 0;
		resp[desc_offset + 8] = (uint8)cd.track_count;
		resp[desc_offset + 9] = 0;
		resp[desc_offset + 10] = 0;
		desc_offset += 11;

		// Point A2
		resp[desc_offset + 0] = 1;
		resp[desc_offset + 1] = 0x14;
		resp[desc_offset + 2] = 0;
		resp[desc_offset + 3] = 0xA2;
		resp[desc_offset + 4] = 0;
		resp[desc_offset + 5] = 0;
		resp[desc_offset + 6] = 0;
		resp[desc_offset + 7] = 0;
		if (use_bcd) {
			LBA_to_MSF_BCD((int32)cd.leadout_lba, &resp[desc_offset + 8], false);
		} else {
			LBA_to_MSF((int32)cd.leadout_lba, &resp[desc_offset + 8], false);
		}
		desc_offset += 11;

		// Track points 1..N
		for (int i = 0; i < cd.track_count; i++) {
			resp[desc_offset + 0] = 1;
			resp[desc_offset + 1] = (cd.tracks[i].type == CD_MODE_AUDIO) ? 0x10 : 0x14;
			resp[desc_offset + 2] = 0;
			resp[desc_offset + 3] = cd.tracks[i].track_num;
			resp[desc_offset + 4] = 0;
			resp[desc_offset + 5] = 0;
			resp[desc_offset + 6] = 0;
			resp[desc_offset + 7] = 0;
			if (use_bcd) {
				LBA_to_MSF_BCD((int32)cd.tracks[i].data_start_lba, &resp[desc_offset + 8], false);
			} else {
				LBA_to_MSF((int32)cd.tracks[i].data_start_lba, &resp[desc_offset + 8], false);
			}
			desc_offset += 11;
		}

		uint16 toclen = desc_offset - 2;
		resp[0] = (toclen >> 8) & 0xFF;
		resp[1] = toclen & 0xFF;
		resp[2] = 1;
		resp[3] = 1;
		total_len = desc_offset;
	} else {
		if (start_track == 0xAA) {
			resp[0] = 0x00;
			resp[1] = 0x0A;
			resp[2] = 1;
			resp[3] = (uint8)cd.track_count;
			resp[4] = 0x00;
			resp[5] = 0x14;
			resp[6] = 0xAA;
			resp[7] = 0x00;
			if (msf) {
				resp[8] = 0x00;
				LBA_to_MSF((int32)cd.leadout_lba, &resp[9], false);
			} else {
				resp[8] = (cd.leadout_lba >> 24) & 0xFF;
				resp[9] = (cd.leadout_lba >> 16) & 0xFF;
				resp[10] = (cd.leadout_lba >> 8) & 0xFF;
				resp[11] = cd.leadout_lba & 0xFF;
			}
			total_len = 12;
		} else {
			int track_idx = (start_track > 0) ? (start_track - 1) : 0;
			if (track_idx >= cd.track_count) track_idx = cd.track_count - 1;

			int desc_count = 0;
			int desc_offset = 4;

			for (int i = track_idx; i < cd.track_count; i++) {
				resp[desc_offset + 0] = 0x00;
				resp[desc_offset + 1] = (cd.tracks[i].type == CD_MODE_AUDIO) ? 0x10 : 0x14;
				resp[desc_offset + 2] = cd.tracks[i].track_num;
				resp[desc_offset + 3] = 0x00;
				if (msf) {
					resp[desc_offset + 4] = 0x00;
					LBA_to_MSF((int32)cd.tracks[i].data_start_lba, &resp[desc_offset + 5], false);
				} else {
					resp[desc_offset + 4] = (cd.tracks[i].data_start_lba >> 24) & 0xFF;
					resp[desc_offset + 5] = (cd.tracks[i].data_start_lba >> 16) & 0xFF;
					resp[desc_offset + 6] = (cd.tracks[i].data_start_lba >> 8) & 0xFF;
					resp[desc_offset + 7] = cd.tracks[i].data_start_lba & 0xFF;
				}
				desc_offset += 8;
				desc_count++;
			}

			resp[desc_offset + 0] = 0x00;
			resp[desc_offset + 1] = 0x14;
			resp[desc_offset + 2] = 0xAA;
			resp[desc_offset + 3] = 0x00;
			if (msf) {
				resp[desc_offset + 4] = 0x00;
				LBA_to_MSF((int32)cd.leadout_lba, &resp[desc_offset + 5], false);
			} else {
				resp[desc_offset + 4] = (cd.leadout_lba >> 24) & 0xFF;
				resp[desc_offset + 5] = (cd.leadout_lba >> 16) & 0xFF;
				resp[desc_offset + 6] = (cd.leadout_lba >> 8) & 0xFF;
				resp[desc_offset + 7] = cd.leadout_lba & 0xFF;
			}
			desc_offset += 8;
			desc_count++;

			uint16 toclen = 2 + desc_count * 8;
			resp[0] = (toclen >> 8) & 0xFF;
			resp[1] = toclen & 0xFF;
			resp[2] = 1;
			resp[3] = (uint8)cd.track_count;
			total_len = desc_offset;
		}
	}

	int copy_len = (total_len < alloc_len) ? total_len : alloc_len;
	if (!try_buffer(copy_len)) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.key = SK_HARDWARE;
		SCSIdisk[target].sense.code = SC_NO_ERROR;
		return;
	}
	memcpy(buffer, resp, copy_len);
	scsi_buffer.limit = scsi_buffer.size = copy_len;
	scsi_buffer.disk = false;

	SCSI_LOG("[SCSI-EMU] Target %d READ_TOC: Format %d (MSF=%d, BCD=%d), start_track=%u -> returning %u bytes, %d tracks, LeadOut LBA %u\n",
	         target, format, msf, use_bcd, start_track, copy_len, cd.track_count, cd.leadout_lba);

	SCSIdisk[target].status = STAT_GOOD;
	SCSIdisk[target].sense.code = SC_NO_ERROR;
	SCSIdisk[target].sense.valid = false;
}

void SCSI_ReadHeader(uint8 *cdb)
{
	if (!SCSIdisk[target].cdrom) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.code = SC_INVALID_CMD;
		SCSIdisk[target].sense.valid = false;
		return;
	}

	if (g_cd_state[target].tray_open || g_cd_state[target].track_count == 0 || g_cd_state[target].tracks[0].fh == NULL) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.key = SK_NOTREADY;
		SCSIdisk[target].sense.code = SC_MEDIUM_NOT_PRESENT;
		SCSIdisk[target].sense.ascq = 0x00;
		SCSIdisk[target].sense.valid = false;
		scsi_buffer.limit = scsi_buffer.size = 0;
		SCSI_LOG("[SCSI-EMU] Target %d READ_HEADER: empty drive -> STAT_CHECK_COND\n", target);
		return;
	}

	const CDROMDriveState &cd = g_cd_state[target];
	bool msf = (cdb[1] & 0x02) != 0;
	uint32 lba = COMMAND_ReadInt32(cdb, 2);
	int alloc_len = COMMAND_ReadInt16(cdb, 7);
	if (alloc_len == 0) alloc_len = 8;

	const CDTrack *t = CD_FindTrackForLBA(cd, lba);
	uint8 mode = 1;
	if (t) {
		if (t->type == CD_MODE_AUDIO) mode = 0;
		else if (t->type == CD_MODE_MODE2_2352) mode = 2;
		else mode = 1;
	}

	uint8 hdr[8];
	memset(hdr, 0, sizeof(hdr));
	hdr[0] = mode;
	if (msf) {
		hdr[4] = 0x00;
		LBA_to_MSF((int32)lba, hdr + 5, false);
	} else {
		hdr[4] = (lba >> 24) & 0xFF;
		hdr[5] = (lba >> 16) & 0xFF;
		hdr[6] = (lba >> 8) & 0xFF;
		hdr[7] = lba & 0xFF;
	}

	int copy_len = (int)sizeof(hdr);
	if (copy_len > alloc_len) copy_len = alloc_len;
	if (!try_buffer(copy_len)) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.key = SK_HARDWARE;
		SCSIdisk[target].sense.code = SC_NO_ERROR;
		return;
	}
	memcpy(buffer, hdr, copy_len);
	scsi_buffer.limit = scsi_buffer.size = copy_len;
	scsi_buffer.disk = false;

	SCSI_LOG("[SCSI-EMU] Target %d READ_HEADER: LBA %u (MSF=%d) -> mode %u\n", target, lba, msf, mode);

	SCSIdisk[target].status = STAT_GOOD;
	SCSIdisk[target].sense.code = SC_NO_ERROR;
	SCSIdisk[target].sense.valid = false;
}

void SCSI_ReadDiscInformation(uint8 *cdb)
{
	if (!SCSIdisk[target].cdrom) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.code = SC_INVALID_CMD;
		SCSIdisk[target].sense.valid = false;
		return;
	}

	if (g_cd_state[target].tray_open || g_cd_state[target].track_count == 0 || g_cd_state[target].tracks[0].fh == NULL) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.key = SK_NOTREADY;
		SCSIdisk[target].sense.code = SC_MEDIUM_NOT_PRESENT;
		SCSIdisk[target].sense.ascq = 0x00;
		SCSIdisk[target].sense.valid = false;
		scsi_buffer.limit = scsi_buffer.size = 0;
		SCSI_LOG("[SCSI-EMU] Target %d READ_DISC_INFO: empty drive -> STAT_CHECK_COND\n", target);
		return;
	}

	const CDROMDriveState &cd = g_cd_state[target];
	int alloc_len = COMMAND_ReadInt16(cdb, 7);
	if (alloc_len == 0) alloc_len = 34;

	uint8 disc_info[34];
	memset(disc_info, 0, sizeof(disc_info));

	disc_info[0] = 0x00;
	disc_info[1] = 0x20; // 32 bytes follow
	disc_info[2] = 0x0E; // Finalized, single-session, complete CD-ROM
	disc_info[3] = 1;    // First track number
	disc_info[4] = 1;    // Number of sessions
	disc_info[5] = 1;    // First track in last session
	disc_info[6] = (uint8)cd.track_count; // Last track in last session
	disc_info[7] = 0x00;
	disc_info[8] = 0x00; // CD-ROM / CD-DA

	disc_info[20] = (cd.leadout_lba >> 24) & 0xFF;
	disc_info[21] = (cd.leadout_lba >> 16) & 0xFF;
	disc_info[22] = (cd.leadout_lba >> 8) & 0xFF;
	disc_info[23] = cd.leadout_lba & 0xFF;

	int copy_len = (int)sizeof(disc_info);
	if (copy_len > alloc_len) copy_len = alloc_len;
	if (!try_buffer(copy_len)) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.key = SK_HARDWARE;
		SCSIdisk[target].sense.code = SC_NO_ERROR;
		return;
	}
	memcpy(buffer, disc_info, copy_len);
	scsi_buffer.limit = scsi_buffer.size = copy_len;
	scsi_buffer.disk = false;

	SCSI_LOG("[SCSI-EMU] Target %d READ_DISC_INFO: %d tracks, LeadOut LBA %u\n", target, cd.track_count, cd.leadout_lba);

	SCSIdisk[target].status = STAT_GOOD;
	SCSIdisk[target].sense.code = SC_NO_ERROR;
	SCSIdisk[target].sense.valid = false;
}

void SCSI_ReadTrackInformation(uint8 *cdb)
{
	if (!SCSIdisk[target].cdrom) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.code = SC_INVALID_CMD;
		SCSIdisk[target].sense.valid = false;
		return;
	}

	if (g_cd_state[target].tray_open || g_cd_state[target].track_count == 0 || g_cd_state[target].tracks[0].fh == NULL) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.key = SK_NOTREADY;
		SCSIdisk[target].sense.code = SC_MEDIUM_NOT_PRESENT;
		SCSIdisk[target].sense.ascq = 0x00;
		SCSIdisk[target].sense.valid = false;
		scsi_buffer.limit = scsi_buffer.size = 0;
		SCSI_LOG("[SCSI-EMU] Target %d READ_TRACK_INFO: empty drive -> STAT_CHECK_COND\n", target);
		return;
	}

	const CDROMDriveState &cd = g_cd_state[target];
	bool track_mode = (cdb[1] & 0x01) != 0;
	uint32 lba_or_track = COMMAND_ReadInt32(cdb, 2);
	int alloc_len = COMMAND_ReadInt16(cdb, 7);
	if (alloc_len == 0) alloc_len = 28;

	const CDTrack *t = NULL;
	if (track_mode) {
		int track_num = (int)lba_or_track;
		if (track_num >= 1 && track_num <= cd.track_count) {
			t = &cd.tracks[track_num - 1];
		}
	} else {
		t = CD_FindTrackForLBA(cd, lba_or_track);
	}

	if (!t) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.code = SC_INVALID_CDB;
		SCSIdisk[target].sense.valid = false;
		return;
	}

	uint8 track_info[28];
	memset(track_info, 0, sizeof(track_info));

	track_info[0] = 0x00;
	track_info[1] = 0x1A; // 26 bytes follow
	track_info[2] = t->track_num;
	track_info[3] = 1;    // Session 1
	track_info[4] = 0x00;
	track_info[5] = (t->type == CD_MODE_AUDIO) ? 0x00 : 0x04;
	track_info[6] = 0x8F; // Data mode & flags
	track_info[7] = 0x00;

	track_info[8] = (t->start_lba >> 24) & 0xFF;
	track_info[9] = (t->start_lba >> 16) & 0xFF;
	track_info[10] = (t->start_lba >> 8) & 0xFF;
	track_info[11] = t->start_lba & 0xFF;

	track_info[12] = 0xFF; track_info[13] = 0xFF; track_info[14] = 0xFF; track_info[15] = 0xFF;

	track_info[24] = (t->length_sectors >> 24) & 0xFF;
	track_info[25] = (t->length_sectors >> 16) & 0xFF;
	track_info[26] = (t->length_sectors >> 8) & 0xFF;
	track_info[27] = t->length_sectors & 0xFF;

	int copy_len = (int)sizeof(track_info);
	if (copy_len > alloc_len) copy_len = alloc_len;
	if (!try_buffer(copy_len)) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.key = SK_HARDWARE;
		SCSIdisk[target].sense.code = SC_NO_ERROR;
		return;
	}
	memcpy(buffer, track_info, copy_len);
	scsi_buffer.limit = scsi_buffer.size = copy_len;
	scsi_buffer.disk = false;

	SCSI_LOG("[SCSI-EMU] Target %d READ_TRACK_INFO: Track %d (start LBA %u, len %u sectors)\n",
	         target, t->track_num, t->start_lba, t->length_sectors);

	SCSIdisk[target].status = STAT_GOOD;
	SCSIdisk[target].sense.code = SC_NO_ERROR;
	SCSIdisk[target].sense.valid = false;
}

void SCSI_GetConfiguration(uint8 *cdb)
{
	if (!SCSIdisk[target].cdrom) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.code = SC_INVALID_CMD;
		SCSIdisk[target].sense.valid = false;
		return;
	}

	uint8 rt = cdb[1] & 0x03;
	uint16 start_feature = COMMAND_ReadInt16(cdb, 2);
	int alloc_len = COMMAND_ReadInt16(cdb, 7);
	if (alloc_len == 0) alloc_len = 256;

	if (rt > 2) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.code = SC_INVALID_CDB;
		SCSIdisk[target].sense.valid = false;
		return;
	}

	uint8 resp[512];
	memset(resp, 0, sizeof(resp));

	// Feature Header: Current profile CD-ROM 0x0008
	resp[6] = 0x00;
	resp[7] = 0x08;

	int len = 8;

	// Feature 0x0000: Profile List
	if ((rt == 2 && start_feature == 0) || (rt <= 1 && start_feature <= 0)) {
		resp[len++] = 0x00; resp[len++] = 0x00;
		resp[len++] = 0x03; // Version 0, Persist=1, Current=1
		resp[len++] = 8;
		resp[len++] = 0x00; resp[len++] = 0x08; resp[len++] = 0x01; resp[len++] = 0x00; // CD-ROM
		resp[len++] = 0x00; resp[len++] = 0x02; resp[len++] = 0x00; resp[len++] = 0x00; // Removable disk
	}

	// Feature 0x0001: Core Feature
	if ((rt == 2 && start_feature == 1) || (rt <= 1 && start_feature <= 1)) {
		resp[len++] = 0x00; resp[len++] = 0x01;
		resp[len++] = 0x0B;
		resp[len++] = 8;
		resp[len++] = 0x00; resp[len++] = 0x00; resp[len++] = 0x00; resp[len++] = 0x01;
		resp[len++] = 0x03; // INQ2 and DBE support
		resp[len++] = 0x00; resp[len++] = 0x00; resp[len++] = 0x00;
	}

	// Feature 0x0002: Morphing Feature
	if ((rt == 2 && start_feature == 2) || (rt <= 1 && start_feature <= 2)) {
		resp[len++] = 0x00; resp[len++] = 0x02;
		resp[len++] = 0x07;
		resp[len++] = 4;
		resp[len++] = 0x02; // OCEvent=1
		resp[len++] = 0x00; resp[len++] = 0x00; resp[len++] = 0x00;
	}

	// Feature 0x0003: Removable Medium
	if ((rt == 2 && start_feature == 3) || (rt <= 1 && start_feature <= 3)) {
		resp[len++] = 0x00; resp[len++] = 0x03;
		resp[len++] = 0x03;
		resp[len++] = 4;
		resp[len++] = 0x28; // Lockable, ejectable
		resp[len++] = 0x00; resp[len++] = 0x00; resp[len++] = 0x00;
	}

	// Feature 0x0010: Random Readable
	if ((rt == 2 && start_feature == 16) || (rt <= 1 && start_feature <= 16)) {
		resp[len++] = 0x00; resp[len++] = 0x10;
		resp[len++] = 0x01;
		resp[len++] = 8;
		resp[len++] = 0x00; resp[len++] = 0x00; resp[len++] = 0x08; resp[len++] = 0x00; // 2048 bytes
		resp[len++] = 0x00; resp[len++] = 0x01;
		resp[len++] = 0x00; resp[len++] = 0x00;
	}

	// Feature 0x001D: Multi-Read
	if ((rt == 2 && start_feature == 29) || (rt <= 1 && start_feature <= 29)) {
		resp[len++] = 0x00; resp[len++] = 0x1D;
		resp[len++] = 0x01;
		resp[len++] = 0;
	}

	// Feature 0x001E: CD Read
	if ((rt == 2 && start_feature == 30) || (rt <= 1 && start_feature <= 30)) {
		resp[len++] = 0x00; resp[len++] = 0x1E;
		resp[len++] = 0x09;
		resp[len++] = 4;
		resp[len++] = 0x00;
		resp[len++] = 0x00; resp[len++] = 0x00; resp[len++] = 0x00;
	}

	// Feature 0x0103: CD Audio Playback
	if ((rt == 2 && start_feature == 0x0103) || (rt <= 1 && start_feature <= 0x0103)) {
		resp[len++] = 0x01; resp[len++] = 0x03;
		resp[len++] = 0x05;
		resp[len++] = 4;
		resp[len++] = 0x03;
		resp[len++] = 0x00;
		resp[len++] = 0x01; // 256 volume levels
		resp[len++] = 0x00;
	}

	uint32 data_len = len - 4;
	resp[0] = (data_len >> 24) & 0xFF;
	resp[1] = (data_len >> 16) & 0xFF;
	resp[2] = (data_len >> 8) & 0xFF;
	resp[3] = data_len & 0xFF;

	int copy_len = (len < alloc_len) ? len : alloc_len;
	if (!try_buffer(copy_len)) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.key = SK_HARDWARE;
		SCSIdisk[target].sense.code = SC_NO_ERROR;
		return;
	}
	memcpy(buffer, resp, copy_len);
	scsi_buffer.limit = scsi_buffer.size = copy_len;
	scsi_buffer.disk = false;

	SCSI_LOG("[SCSI-EMU] Target %d GET_CONFIGURATION: start_feature=0x%04X -> returning %d bytes\n", target, start_feature, copy_len);

	SCSIdisk[target].status = STAT_GOOD;
	SCSIdisk[target].sense.code = SC_NO_ERROR;
	SCSIdisk[target].sense.valid = false;
}

void SCSI_GetEventStatus(uint8 *cdb)
{
	if (!SCSIdisk[target].cdrom) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.code = SC_INVALID_CMD;
		SCSIdisk[target].sense.valid = false;
		return;
	}

	CDROMDriveState &cd = g_cd_state[target];
	int alloc_len = COMMAND_ReadInt16(cdb, 7);
	if (alloc_len == 0) alloc_len = 8;

	uint8 resp[8];
	memset(resp, 0, sizeof(resp));

	if (cd.media_events != 0) {
		resp[0] = 0;
		resp[1] = 6;
		resp[2] = 0x04; // Media status events
		resp[3] = 0x04; // Supported event class
		resp[4] = cd.media_events;
		resp[5] = 0x01; // Active power
		resp[6] = 0; resp[7] = 0;
		SCSI_LOG("[SCSI-EMU] Target %d GET_EVENT_STATUS: reporting media event 0x%02X\n", target, cd.media_events);
		cd.media_events = 0;
	} else {
		resp[0] = 0;
		resp[1] = 2;
		resp[2] = 0x00;
		resp[3] = 0x04;
		resp[4] = 0; resp[5] = 0; resp[6] = 0; resp[7] = 0;
	}

	int copy_len = (int)sizeof(resp);
	if (copy_len > alloc_len) copy_len = alloc_len;
	if (!try_buffer(copy_len)) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.key = SK_HARDWARE;
		SCSIdisk[target].sense.code = SC_NO_ERROR;
		return;
	}
	memcpy(buffer, resp, copy_len);
	scsi_buffer.limit = scsi_buffer.size = copy_len;
	scsi_buffer.disk = false;

	SCSIdisk[target].status = STAT_GOOD;
	SCSIdisk[target].sense.code = SC_NO_ERROR;
	SCSIdisk[target].sense.valid = false;
}

void SCSI_MechanismStatus(uint8 *cdb)
{
	if (!SCSIdisk[target].cdrom) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.code = SC_INVALID_CMD;
		SCSIdisk[target].sense.valid = false;
		return;
	}

	CD_UpdateAudioPlayback(target);
	const CDROMDriveState &cd = g_cd_state[target];
	int alloc_len = COMMAND_ReadInt16(cdb, 8);
	if (alloc_len == 0) alloc_len = 8;

	uint8 resp[8];
	memset(resp, 0, sizeof(resp));
	resp[0] = 0x00;
	resp[1] = (cd.audio_status == 0x11) ? 0x20 : 0x00;
	resp[2] = (cd.audio_current_lba >> 16) & 0xFF;
	resp[3] = (cd.audio_current_lba >> 8) & 0xFF;
	resp[4] = cd.audio_current_lba & 0xFF;
	resp[5] = 0;
	resp[6] = 0;
	resp[7] = 0;

	int copy_len = (int)sizeof(resp);
	if (copy_len > alloc_len) copy_len = alloc_len;
	if (!try_buffer(copy_len)) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.key = SK_HARDWARE;
		SCSIdisk[target].sense.code = SC_NO_ERROR;
		return;
	}
	memcpy(buffer, resp, copy_len);
	scsi_buffer.limit = scsi_buffer.size = copy_len;
	scsi_buffer.disk = false;

	SCSI_LOG("[SCSI-EMU] Target %d MECHANISM_STATUS: audio_status=0x%02X, cur_lba=%u\n", target, cd.audio_status, cd.audio_current_lba);

	SCSIdisk[target].status = STAT_GOOD;
	SCSIdisk[target].sense.code = SC_NO_ERROR;
	SCSIdisk[target].sense.valid = false;
}

void SCSI_ReadSubChannel(uint8 *cdb)
{
	if (!SCSIdisk[target].cdrom) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.code = SC_INVALID_CMD;
		SCSIdisk[target].sense.valid = false;
		return;
	}

	CD_UpdateAudioPlayback(target);
	const CDROMDriveState &cd = g_cd_state[target];

	bool msf = (cdb[1] & 0x02) != 0;
	bool subq = (cdb[2] & 0x40) != 0;
	uint8 format = cdb[3];
	int alloc_len = COMMAND_ReadInt16(cdb, 7);
	if (alloc_len == 0) alloc_len = 16;

	uint8 subch[48];
	memset(subch, 0, sizeof(subch));

	subch[0] = 0x00;
	subch[1] = cd.audio_status;
	subch[2] = 0x00;
	subch[3] = 0x00;

	int data_len = 4;
	if (subq && (format == 0x00 || format == 0x01)) {
		const CDTrack *t = CD_FindTrackForLBA(cd, cd.audio_current_lba);
		uint8 trk_num = t ? t->track_num : 1;
		uint8 ctrl_adr = (t && t->type == CD_MODE_AUDIO) ? 0x10 : 0x14;
		uint32 data_start = t ? t->data_start_lba : 0;
		uint8 idx_num = (cd.audio_current_lba >= data_start) ? 1 : 0;

		subch[3] = 12;
		data_len = 16;
		subch[4] = 0x01;
		subch[5] = ctrl_adr;
		subch[6] = trk_num;
		subch[7] = idx_num;

		if (msf) {
			subch[8] = 0x00;
			LBA_to_MSF((int32)cd.audio_current_lba, subch + 9, false);
		} else {
			subch[8] = (cd.audio_current_lba >> 24) & 0xFF;
			subch[9] = (cd.audio_current_lba >> 16) & 0xFF;
			subch[10] = (cd.audio_current_lba >> 8) & 0xFF;
			subch[11] = cd.audio_current_lba & 0xFF;
		}

		int32 rel_lba = (int32)cd.audio_current_lba - (int32)data_start;
		if (msf) {
			subch[12] = 0x00;
			LBA_to_MSF(rel_lba, subch + 13, true);
		} else {
			uint32 urel = (uint32)rel_lba;
			subch[12] = (urel >> 24) & 0xFF;
			subch[13] = (urel >> 16) & 0xFF;
			subch[14] = (urel >> 8) & 0xFF;
			subch[15] = urel & 0xFF;
		}
	} else if (subq && format == 0x02) {
		subch[3] = 16;
		data_len = 20;
		subch[4] = 0x02;
		subch[5] = 0x14;
	} else if (subq && format == 0x03) {
		subch[3] = 16;
		data_len = 20;
		subch[4] = 0x03;
		subch[5] = 0x14;
	}

	int copy_len = (data_len < alloc_len) ? data_len : alloc_len;
	if (!try_buffer(copy_len)) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.key = SK_HARDWARE;
		SCSIdisk[target].sense.code = SC_NO_ERROR;
		return;
	}
	memcpy(buffer, subch, copy_len);
	scsi_buffer.limit = scsi_buffer.size = copy_len;
	scsi_buffer.disk = false;

	SCSI_LOG("[SCSI-EMU] Target %d READ_SUBCHANNEL: subq=%d, format=%u, MSF=%d -> audio_status=0x%02X, cur_lba=%u (returned %d bytes)\n",
	         target, subq, format, msf, cd.audio_status, cd.audio_current_lba, copy_len);

	SCSIdisk[target].status = STAT_GOOD;
	SCSIdisk[target].sense.code = SC_NO_ERROR;
	SCSIdisk[target].sense.valid = false;
}

void SCSI_ReadCD(uint8 *cdb, bool is_msf)
{
	if (!SCSIdisk[target].cdrom) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.code = SC_INVALID_CMD;
		SCSIdisk[target].sense.valid = false;
		return;
	}

	if (g_cd_state[target].tray_open || g_cd_state[target].track_count == 0 || g_cd_state[target].tracks[0].fh == NULL) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.key = SK_NOTREADY;
		SCSIdisk[target].sense.code = SC_MEDIUM_NOT_PRESENT;
		SCSIdisk[target].sense.ascq = 0x00;
		SCSIdisk[target].sense.valid = false;
		scsi_buffer.limit = scsi_buffer.size = 0;
		SCSI_LOG("[SCSI-EMU] Target %d READ_CD: empty drive -> STAT_CHECK_COND\n", target);
		return;
	}

	uint32 lba = 0;
	uint32 blocks = 0;
	if (is_msf) {
		uint32 start = (uint32)MSF_to_LBA(cdb[3], cdb[4], cdb[5], false);
		uint32 end = (uint32)MSF_to_LBA(cdb[6], cdb[7], cdb[8], false);
		lba = start;
		blocks = (end > start) ? (end - start) : 0;
	} else {
		lba = COMMAND_ReadInt32(cdb, 2);
		blocks = ((uint32)cdb[6] << 16) | ((uint32)cdb[7] << 8) | (uint32)cdb[8];
	}

	if (blocks == 0) {
		SCSIdisk[target].status = STAT_GOOD;
		SCSIdisk[target].sense.code = SC_NO_ERROR;
		scsi_buffer.limit = scsi_buffer.size = 0;
		return;
	}

	uint8 main_channel = cdb[9];
	uint8 sub_channel = cdb[10];

	int block_out_size = 2048;
	if ((main_channel & 0xF8) == 0xF8 || (main_channel & 0xB8) == 0xB8 || main_channel == 0) {
		block_out_size = 2352;
	}
	bool include_q = (sub_channel == 0x02);
	int final_block_size = block_out_size + (include_q ? 16 : 0);

	uint32 total_bytes = blocks * final_block_size;
	if (!try_buffer(total_bytes)) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.key = SK_HARDWARE;
		SCSIdisk[target].sense.code = SC_NO_ERROR;
		return;
	}

	uint8 *dest = buffer;
	uint8 raw_buf[2352];

	for (uint32 b = 0; b < blocks; b++) {
		uint32 cur_lba = lba + b;
		if (!CD_ReadRawSector(target, cur_lba, raw_buf)) {
			SCSI_LOG("[SCSI-EMU] Target %d: READ_CD failed at raw LBA %u\n", target, cur_lba);
			SCSIdisk[target].status = STAT_CHECK_COND;
			SCSIdisk[target].sense.code = SC_INVALID_LBA;
			SCSIdisk[target].sense.valid = true;
			SCSIdisk[target].sense.info = cur_lba;
			return;
		}

		if (block_out_size == 2048) {
			const CDTrack *t = CD_FindTrackForLBA(g_cd_state[target], cur_lba);
			int data_offset = 16;
			if (t && t->type == CD_MODE_MODE2_2352) data_offset = 24;
			else if (t && t->type == CD_MODE_MODE1_2048) data_offset = 16;
			memcpy(dest, raw_buf + data_offset, 2048);
			dest += 2048;
		} else {
			memcpy(dest, raw_buf, 2352);
			dest += 2352;
		}

		if (include_q) {
			const CDTrack *t = CD_FindTrackForLBA(g_cd_state[target], cur_lba);
			uint8 trk_num = t ? t->track_num : 1;
			uint8 ctrl_adr = (t && t->type == CD_MODE_AUDIO) ? 0x10 : 0x14;
			uint32 data_start = t ? t->data_start_lba : 0;
			uint8 idx_num = (cur_lba >= data_start) ? 1 : 0;

			*dest++ = ctrl_adr;
			*dest++ = trk_num;
			*dest++ = idx_num;
			int32 rel_lba = (int32)cur_lba - (int32)data_start;
			LBA_to_MSF(rel_lba, dest, true); dest += 3;
			*dest++ = 0x00;
			LBA_to_MSF((int32)cur_lba, dest, false); dest += 3;
			*dest++ = 0x00; *dest++ = 0x00;
			*dest++ = 0x00; *dest++ = 0x00; *dest++ = 0x00; *dest++ = 0x00;
		}
	}

	scsi_buffer.limit = scsi_buffer.size = total_bytes;
	scsi_buffer.disk = false;

	SCSI_LOG("[SCSI-EMU] Target %d READ_CD: LBA %u, %u blocks -> returning %u bytes\n", target, lba, blocks, total_bytes);

	if (scsi_buffer.size > 0) {
		char hex_dump[128] = {0};
		char ascii_dump[36] = {0};
		size_t dump_len = scsi_buffer.size < 32 ? scsi_buffer.size : 32;
		for (size_t d = 0; d < dump_len; d++) {
			char byte_hex[4];
			snprintf(byte_hex, sizeof(byte_hex), "%02X ", buffer[d]);
			strncat(hex_dump, byte_hex, sizeof(hex_dump) - strlen(hex_dump) - 1);
			ascii_dump[d] = (buffer[d] >= 32 && buffer[d] <= 126) ? (char)buffer[d] : '.';
		}
		ascii_dump[dump_len] = '\0';
		SCSI_LOG("[SCSI-CDROM] Target %d Raw CD Read Preview (LBA %u, %u bytes): [%s] | '%s'\n",
		         target, lba, scsi_buffer.size, hex_dump, ascii_dump);
	}

	SCSIdisk[target].status = STAT_GOOD;
	SCSIdisk[target].sense.code = SC_NO_ERROR;
	SCSIdisk[target].sense.valid = false;
}

void SCSI_AppleReadCDDA(uint8 *cdb, bool is_msf)
{
	if (!SCSIdisk[target].cdrom) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.code = SC_INVALID_CMD;
		SCSIdisk[target].sense.valid = false;
		return;
	}

	if (g_cd_state[target].tray_open || g_cd_state[target].track_count == 0 || g_cd_state[target].tracks[0].fh == NULL) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.key = SK_NOTREADY;
		SCSIdisk[target].sense.code = SC_MEDIUM_NOT_PRESENT;
		SCSIdisk[target].sense.ascq = 0x00;
		SCSIdisk[target].sense.valid = false;
		scsi_buffer.limit = scsi_buffer.size = 0;
		SCSI_LOG("[SCSI-EMU] Target %d APPLE_CDDA: empty drive -> STAT_CHECK_COND\n", target);
		return;
	}

	uint32 lba = 0;
	uint32 blocks = 0;
	if (is_msf) {
		uint32 start = (uint32)MSF_to_LBA(cdb[3], cdb[4], cdb[5], false);
		uint32 end = (uint32)MSF_to_LBA(cdb[7], cdb[8], cdb[9], false);
		lba = start;
		blocks = (end > start) ? (end - start) : 0;
	} else {
		lba = COMMAND_ReadInt32(cdb, 2);
		blocks = COMMAND_ReadInt32(cdb, 6);
	}

	if (blocks == 0) {
		SCSIdisk[target].status = STAT_GOOD;
		SCSIdisk[target].sense.code = SC_NO_ERROR;
		scsi_buffer.limit = scsi_buffer.size = 0;
		return;
	}

	uint32 total_bytes = blocks * 2352;
	if (!try_buffer(total_bytes)) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.key = SK_HARDWARE;
		SCSIdisk[target].sense.code = SC_NO_ERROR;
		return;
	}

	uint8 *dest = buffer;
	for (uint32 b = 0; b < blocks; b++) {
		uint32 cur_lba = lba + b;
		if (!CD_ReadRawSector(target, cur_lba, dest)) {
			SCSI_LOG("[SCSI-EMU] Target %d: APPLE_CDDA failed at raw LBA %u\n", target, cur_lba);
			SCSIdisk[target].status = STAT_CHECK_COND;
			SCSIdisk[target].sense.code = SC_INVALID_LBA;
			SCSIdisk[target].sense.valid = true;
			SCSIdisk[target].sense.info = cur_lba;
			return;
		}
		dest += 2352;
	}

	scsi_buffer.limit = scsi_buffer.size = total_bytes;
	scsi_buffer.disk = false;

	SCSI_LOG("[SCSI-EMU] Target %d APPLE_CDDA: LBA %u, %u blocks -> returning %u bytes\n", target, lba, blocks, total_bytes);

	if (scsi_buffer.size > 0) {
		char hex_dump[128] = {0};
		char ascii_dump[36] = {0};
		size_t dump_len = scsi_buffer.size < 32 ? scsi_buffer.size : 32;
		for (size_t d = 0; d < dump_len; d++) {
			char byte_hex[4];
			snprintf(byte_hex, sizeof(byte_hex), "%02X ", buffer[d]);
			strncat(hex_dump, byte_hex, sizeof(hex_dump) - strlen(hex_dump) - 1);
			ascii_dump[d] = (buffer[d] >= 32 && buffer[d] <= 126) ? (char)buffer[d] : '.';
		}
		ascii_dump[dump_len] = '\0';
		SCSI_LOG("[SCSI-CDROM] Target %d Raw CDDA Read Preview (LBA %u, %u bytes): [%s] | '%s'\n",
		         target, lba, scsi_buffer.size, hex_dump, ascii_dump);
	}

	SCSIdisk[target].status = STAT_GOOD;
	SCSIdisk[target].sense.code = SC_NO_ERROR;
	SCSIdisk[target].sense.valid = false;
}

void SCSI_PlayAudio(uint8 *cdb)
{
	if (!SCSIdisk[target].cdrom) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.code = SC_INVALID_CMD;
		SCSIdisk[target].sense.valid = false;
		return;
	}

	CDROMDriveState &cd = g_cd_state[target];
	uint8 opcode = cdb[0];
	uint32 start_lba = 0;
	uint32 blocks = 0;

	if (opcode == SCSI_PLAYAUD_10) {
		start_lba = COMMAND_ReadInt32(cdb, 2);
		blocks = COMMAND_ReadInt16(cdb, 7);
	} else if (opcode == SCSI_PLAYAUD_12) {
		start_lba = COMMAND_ReadInt32(cdb, 2);
		blocks = COMMAND_ReadInt32(cdb, 6);
	} else if (opcode == SCSI_PLAYAUDMSF) {
		uint32 start = (uint32)MSF_to_LBA(cdb[3], cdb[4], cdb[5], false);
		uint32 end = (uint32)MSF_to_LBA(cdb[6], cdb[7], cdb[8], false);
		start_lba = start;
		blocks = (end > start) ? (end - start) : 0;
	} else if (opcode == SCSI_PLAYA_TKIN) {
		uint8 start_track = cdb[4];
		uint8 end_track = cdb[7];
		const CDTrack *st = (start_track >= 1 && start_track <= cd.track_count) ? &cd.tracks[start_track - 1] : NULL;
		const CDTrack *et = (end_track >= 1 && end_track <= cd.track_count) ? &cd.tracks[end_track - 1] : NULL;
		start_lba = st ? st->start_lba : 0;
		uint32 end_lba = et ? (et->start_lba + et->length_sectors) : cd.leadout_lba;
		blocks = (end_lba > start_lba) ? (end_lba - start_lba) : 0;
	}

	if (blocks > 0) {
		cd.audio_status = 0x11; // Playing
		cd.audio_start_lba = start_lba;
		cd.audio_current_lba = start_lba;
		cd.audio_end_lba = start_lba + blocks;
		cd.audio_start_time_ms = get_time_ms();
		cd.audio_paused = false;
		SCSI_LOG("[SCSI-EMU] Target %d PLAY_AUDIO: Start LBA %u, %u blocks (End LBA %u)\n", target, start_lba, blocks, cd.audio_end_lba);
	} else {
		cd.audio_status = 0x15; // Stopped
		cd.audio_current_lba = start_lba;
		cd.audio_paused = false;
		SCSI_LOG("[SCSI-EMU] Target %d PLAY_AUDIO: Stopped (0 blocks)\n", target);
	}

	SCSIdisk[target].status = STAT_GOOD;
	SCSIdisk[target].sense.code = SC_NO_ERROR;
	SCSIdisk[target].sense.valid = false;
}

void SCSI_PauseResume(uint8 *cdb)
{
	if (!SCSIdisk[target].cdrom) {
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.code = SC_INVALID_CMD;
		SCSIdisk[target].sense.valid = false;
		return;
	}

	CDROMDriveState &cd = g_cd_state[target];
	bool resume = (cdb[8] & 0x01) != 0;

	if (resume) {
		if (cd.audio_paused) {
			cd.audio_status = 0x11;
			cd.audio_paused = false;
			cd.audio_start_lba = cd.audio_current_lba;
			cd.audio_start_time_ms = get_time_ms();
			SCSI_LOG("[SCSI-EMU] Target %d PAUSE_RESUME: Resumed at LBA %u\n", target, cd.audio_current_lba);
		}
	} else {
		CD_UpdateAudioPlayback(target);
		cd.audio_status = 0x12; // Paused
		cd.audio_paused = true;
		SCSI_LOG("[SCSI-EMU] Target %d PAUSE_RESUME: Paused at LBA %u\n", target, cd.audio_current_lba);
	}

	SCSIdisk[target].status = STAT_GOOD;
	SCSIdisk[target].sense.code = SC_NO_ERROR;
	SCSIdisk[target].sense.valid = false;
}

void SCSI_StopAudio(uint8 *cdb)
{
	if (SCSIdisk[target].cdrom) {
		g_cd_state[target].audio_status = 0x15; // Stopped
		g_cd_state[target].audio_paused = false;
		SCSI_LOG("[SCSI-EMU] Target %d STOP_AUDIO\n", target);
	}
	SCSIdisk[target].status = STAT_GOOD;
	SCSIdisk[target].sense.code = SC_NO_ERROR;
	SCSIdisk[target].sense.valid = false;
}

void SCSI_WriteSector(uint8 *cdb)
{
	SCSIdisk[target].lba = SCSI_GetOffset(cdb[0], cdb);
	SCSIdisk[target].blockcounter = SCSI_GetCount(cdb[0], cdb);

	if (SCSIdisk[target].cdrom) {
		SCSI_LOG("[SCSI-EMU] Target %d WRITE: Attempt to write to read-only CD-ROM -> STAT_CHECK_COND (WRITE_PROTECT)\n", target);
		SCSIdisk[target].status = STAT_CHECK_COND;
		SCSIdisk[target].sense.code = SC_WRITE_PROTECT;
		SCSIdisk[target].sense.valid = false;
		return;
	}
	scsi_buffer.disk = true;
	scsi_buffer.size = 0;
	scsi_buffer.limit = BLOCKSIZE;
	scsi_write_sector();
}

void scsi_write_sector(void)
{
	int loop = 0;
	int n = 0;

	while (SCSIdisk[target].blockcounter > 0) {
		if ((SCSIdisk[target].dsk == NULL) || (fseek(SCSIdisk[target].dsk, (long)SCSIdisk[target].lba * BLOCKSIZE, SEEK_SET) != 0)) {
			n = 0;
		} else {
			n = fwrite(buffer + (loop * 512), BLOCKSIZE, 1, SCSIdisk[target].dsk);
			scsi_buffer.limit = BLOCKSIZE;
			scsi_buffer.size = 0;
		}

		if (n == 1) {
			SCSIdisk[target].status = STAT_GOOD;
			SCSIdisk[target].sense.code = SC_NO_ERROR;
			SCSIdisk[target].sense.valid = false;
			SCSIdisk[target].lba++;
			SCSIdisk[target].blockcounter--;
		} else {
			SCSI_LOG("[SCSI-EMU] Target %d: HDD write error at LBA %u!\n", target, SCSIdisk[target].lba);
			SCSIdisk[target].status = STAT_CHECK_COND;
			SCSIdisk[target].sense.code = SC_INVALID_LBA;
			SCSIdisk[target].sense.valid = true;
			SCSIdisk[target].sense.info = SCSIdisk[target].lba;
		}
		loop++;
	}
}

unsigned long SCSI_GetOffset(uint8 opcode, uint8 *cdb)
{
	if (opcode < 0x20)
		return (COMMAND_ReadInt24(cdb, 1) & 0x1FFFFF);
	else
		return COMMAND_ReadInt32(cdb, 2);
}

int SCSI_GetCount(uint8 opcode, uint8 *cdb)
{
	if (opcode < 0x20)
		return ((cdb[4] == 0) ? 0x100 : cdb[4]);
	else if (opcode == 0xA8 || opcode == 0xAA)
		return COMMAND_ReadInt32(cdb, 6);
	else
		return COMMAND_ReadInt16(cdb, 7);
}

void SCSI_ModeSelect(uint8 *cdb)
{
	if (!SCSIdisk[target].cdrom) {
		SCSIdisk[target].status = STAT_GOOD;
		SCSIdisk[target].sense.code = SC_NO_ERROR;
		SCSIdisk[target].sense.valid = false;
		return;
	}

	uint8 opcode = cdb[0];
	int param_len = (opcode == CMD_MODESELECT) ? cdb[4] : COMMAND_ReadInt16(cdb, 7);
	int bd_len = 0;
	if (param_len > 0 && buffer != NULL) {
		int bd_offset = 0;
		if (opcode == CMD_MODESELECT) {
			if (param_len >= 4) {
				bd_len = buffer[3];
				bd_offset = 4;
			}
		} else {
			if (param_len >= 8) {
				bd_len = COMMAND_ReadInt16(buffer, 6);
				bd_offset = 8;
			}
		}

		if (bd_len >= 8 && (bd_offset + 8) <= param_len) {
			uint32 block_len = ((uint32)buffer[bd_offset + 5] << 16) |
			                   ((uint32)buffer[bd_offset + 6] << 8) |
			                   (uint32)buffer[bd_offset + 7];
			if (block_len == 512 || block_len == 2048 || block_len == 2352) {
				SCSIdisk[target].sector_size = block_len;
				SCSI_LOG("[SCSI-EMU] Target %d MODE_SELECT: Sector size successfully set to %u bytes (param_len=%d, bd_len=%d)\n",
				         target, block_len, param_len, bd_len);
			}
		}
	}

	SCSIdisk[target].status = STAT_GOOD;
	SCSIdisk[target].sense.code = SC_NO_ERROR;
	SCSIdisk[target].sense.valid = false;
}

void SCSI_ModeSense(uint8 *cdb)
{
	int blocksize = SCSIdisk[target].cdrom ? (SCSIdisk[target].sector_size ? SCSIdisk[target].sector_size : CDBLOCKSIZE) : BLOCKSIZE;
	uint8 retbuf[256];
	memset(retbuf, 0, sizeof(retbuf));

	uint32 sectors = 0;
	if (SCSIdisk[target].cdrom) {
		sectors = (blocksize == 512) ? (g_cd_state[target].leadout_lba * 4) : g_cd_state[target].leadout_lba;
	} else {
		sectors = SCSIdisk[target].size / blocksize;
	}

	uint8 pagecontrol = (cdb[2] >> 6) & 0x03;
	uint8 pagecode = cdb[2] & 0x3F;
	uint8 dbd = cdb[1] & 0x08;

	/* Header */
	retbuf[0] = 0x00;
	retbuf[1] = SCSIdisk[target].cdrom ? 0x02 : 0x00; // Medium type (0x02 = 120mm CD-ROM data disc)
	retbuf[2] = 0x00;                                 // Device-specific parameter
	retbuf[3] = dbd ? 0x00 : 0x08;

	/* Block descriptor */
	uint8 header_size = 4;
	if (!dbd) {
		retbuf[4] = SCSIdisk[target].cdrom ? 0x01 : 0x00; // Density (0x01 = User data only)
		if (SCSIdisk[target].cdrom) {
			retbuf[5] = 0x00;
			retbuf[6] = 0x00;
			retbuf[7] = 0x00;
		} else {
			retbuf[5] = (sectors >> 16) & 0xFF;
			retbuf[6] = (sectors >> 8) & 0xFF;
			retbuf[7] = sectors & 0xFF;
		}
		retbuf[8] = 0x00;
		retbuf[9] = (blocksize >> 16) & 0xFF;
		retbuf[10] = (blocksize >> 8) & 0xFF;
		retbuf[11] = blocksize & 0xFF;
		header_size = 12;
	}
	retbuf[0] = header_size - 1;

	/* Mode Pages */
	if (pagecode == 0x3F) {
		uint8 offset = header_size;
		static const uint8 supported_pages[] = {0x01, 0x03, 0x04, 0x0D, 0x0E, 0x2A, 0x30};
		for (size_t i = 0; i < sizeof(supported_pages); i++) {
			uint8 code = supported_pages[i];
			if (code == 0x04 && SCSIdisk[target].cdrom)
				continue;
			if ((code == 0x03 || code == 0x0D || code == 0x0E || code == 0x2A) && !SCSIdisk[target].cdrom)
				continue;

			MODEPAGE page = SCSI_GetModePage(code);
			if (page.pagesize == 0)
				continue;

			if (offset + page.pagesize > sizeof(retbuf))
				break;

			switch (pagecontrol) {
				case 0:
				case 2:
				case 3:
					memcpy(retbuf + offset, page.modepage, page.pagesize);
					break;
				case 1:
					memset(retbuf + offset, 0, page.pagesize);
					retbuf[offset] = page.modepage[0];
					retbuf[offset + 1] = page.modepage[1];
					break;
			}
			offset += page.pagesize;
			retbuf[0] += page.pagesize;
		}
	} else if (pagecode != 0x00) {
		MODEPAGE page = SCSI_GetModePage(pagecode);
		if (page.pagesize == 0) {
			SCSI_LOG("[SCSI-EMU] Target %d MODE_SENSE: Unsupported page code 0x%02X -> STAT_CHECK_COND\n", target, pagecode);
			SCSIdisk[target].status = STAT_CHECK_COND;
			SCSIdisk[target].sense.code = SC_INVALID_CDB;
			SCSIdisk[target].sense.valid = false;
			return;
		}

		switch (pagecontrol) {
			case 0:
			case 2:
			case 3:
				memcpy(retbuf + header_size, page.modepage, page.pagesize);
				break;
			case 1:
				memset(retbuf + header_size, 0, page.pagesize);
				retbuf[header_size] = page.modepage[0];
				retbuf[header_size + 1] = page.modepage[1];
				break;
		}
		retbuf[0] += page.pagesize;
	}

	scsi_buffer.disk = false;
	int alloc_len = SCSI_GetTransferLength(cdb[0], cdb);
	scsi_buffer.limit = scsi_buffer.size = retbuf[0] + 1;
	if ((int)scsi_buffer.limit > alloc_len) {
		scsi_buffer.limit = scsi_buffer.size = alloc_len;
	}
	memcpy(buffer, retbuf, scsi_buffer.limit);

	SCSI_LOG("[SCSI-EMU] Target %d MODE_SENSE(6): Page 0x%02X (ctrl=%d, dbd=%d) -> returning %u bytes (blocksize %u)\n",
	         target, pagecode, pagecontrol, dbd, scsi_buffer.limit, blocksize);

	SCSIdisk[target].status = STAT_GOOD;
	SCSIdisk[target].sense.code = SC_NO_ERROR;
	SCSIdisk[target].sense.valid = false;
}

void SCSI_ModeSense10(uint8 *cdb)
{
	int blocksize = SCSIdisk[target].cdrom ? (SCSIdisk[target].sector_size ? SCSIdisk[target].sector_size : CDBLOCKSIZE) : BLOCKSIZE;
	uint8 retbuf[256];
	memset(retbuf, 0, sizeof(retbuf));

	uint32 sectors = 0;
	if (SCSIdisk[target].cdrom) {
		sectors = (blocksize == 512) ? (g_cd_state[target].leadout_lba * 4) : g_cd_state[target].leadout_lba;
	} else {
		sectors = SCSIdisk[target].size / blocksize;
	}

	uint8 pagecontrol = (cdb[2] >> 6) & 0x03;
	uint8 pagecode = cdb[2] & 0x3F;
	uint8 dbd = cdb[1] & 0x08;

	retbuf[0] = 0x00;
	retbuf[1] = 0x00;
	retbuf[2] = SCSIdisk[target].cdrom ? 0x02 : 0x00;
	retbuf[3] = 0x00;
	retbuf[4] = 0x00;
	retbuf[5] = 0x00;
	retbuf[6] = 0x00;
	retbuf[7] = dbd ? 0x00 : 0x08;

	uint8 header_size = 8;
	if (!dbd) {
		retbuf[8] = SCSIdisk[target].cdrom ? 0x01 : 0x00;
		if (SCSIdisk[target].cdrom) {
			retbuf[9] = 0x00;
			retbuf[10] = 0x00;
			retbuf[11] = 0x00;
		} else {
			retbuf[9] = (sectors >> 16) & 0xFF;
			retbuf[10] = (sectors >> 8) & 0xFF;
			retbuf[11] = sectors & 0xFF;
		}
		retbuf[12] = 0x00;
		retbuf[13] = (blocksize >> 16) & 0xFF;
		retbuf[14] = (blocksize >> 8) & 0xFF;
		retbuf[15] = blocksize & 0xFF;
		header_size = 16;
	}

	uint8 offset = header_size;
	if (pagecode == 0x3F) {
		static const uint8 supported_pages[] = {0x01, 0x03, 0x04, 0x0D, 0x0E, 0x2A, 0x30};
		for (size_t i = 0; i < sizeof(supported_pages); i++) {
			uint8 code = supported_pages[i];
			if (code == 0x04 && SCSIdisk[target].cdrom)
				continue;
			if ((code == 0x03 || code == 0x0D || code == 0x0E || code == 0x2A) && !SCSIdisk[target].cdrom)
				continue;

			MODEPAGE page = SCSI_GetModePage(code);
			if (page.pagesize == 0 || offset + page.pagesize > sizeof(retbuf))
				continue;

			if (pagecontrol == 1) {
				memset(retbuf + offset, 0, page.pagesize);
				retbuf[offset] = page.modepage[0];
				retbuf[offset + 1] = page.modepage[1];
			} else {
				memcpy(retbuf + offset, page.modepage, page.pagesize);
			}
			offset += page.pagesize;
		}
	} else if (pagecode != 0x00) {
		MODEPAGE page = SCSI_GetModePage(pagecode);
		if (page.pagesize == 0) {
			SCSI_LOG("[SCSI-EMU] Target %d MODE_SENSE(10): Unsupported page code 0x%02X -> STAT_CHECK_COND\n", target, pagecode);
			SCSIdisk[target].status = STAT_CHECK_COND;
			SCSIdisk[target].sense.code = SC_INVALID_CDB;
			SCSIdisk[target].sense.valid = false;
			return;
		}
		if (pagecontrol == 1) {
			memset(retbuf + offset, 0, page.pagesize);
			retbuf[offset] = page.modepage[0];
			retbuf[offset + 1] = page.modepage[1];
		} else {
			memcpy(retbuf + offset, page.modepage, page.pagesize);
		}
		offset += page.pagesize;
	}

	uint16 total_data_len = offset - 2;
	retbuf[0] = (total_data_len >> 8) & 0xFF;
	retbuf[1] = total_data_len & 0xFF;

	scsi_buffer.disk = false;
	int alloc_len = COMMAND_ReadInt16(cdb, 7);
	if (alloc_len == 0) alloc_len = offset;
	scsi_buffer.limit = scsi_buffer.size = offset;
	if ((int)scsi_buffer.limit > alloc_len) {
		scsi_buffer.limit = scsi_buffer.size = alloc_len;
	}
	memcpy(buffer, retbuf, scsi_buffer.limit);

	SCSI_LOG("[SCSI-EMU] Target %d MODE_SENSE(10): Page 0x%02X (ctrl=%d, dbd=%d) -> returning %u bytes (blocksize %u)\n",
	         target, pagecode, pagecontrol, dbd, scsi_buffer.limit, blocksize);

	SCSIdisk[target].status = STAT_GOOD;
	SCSIdisk[target].sense.code = SC_NO_ERROR;
	SCSIdisk[target].sense.valid = false;
}

MODEPAGE SCSI_GetModePage(uint8 pagecode)
{
	int blocksize = SCSIdisk[target].cdrom ? (SCSIdisk[target].sector_size ? SCSIdisk[target].sector_size : CDBLOCKSIZE) : BLOCKSIZE;
	MODEPAGE page;
	memset(&page, 0, sizeof(page));

	switch (pagecode) {
		case 0x00:
			page.pagesize = 4;
			page.modepage[0] = 0x00;
			page.modepage[1] = 0x02;
			page.modepage[2] = 0x80;
			page.modepage[3] = 0x00;
			break;

		case 0x01:
			page.pagesize = 8;
			page.modepage[0] = 0x01;
			page.modepage[1] = 0x06;
			page.modepage[2] = 0x00;
			page.modepage[3] = 0x1B;
			page.modepage[4] = 0x0B;
			page.modepage[5] = 0x00;
			page.modepage[6] = 0x00;
			page.modepage[7] = 0xFF;
			break;

		case 0x03:
			if (SCSIdisk[target].cdrom) {
				page.pagesize = 24;
				page.modepage[0] = 0x03;
				page.modepage[1] = 0x16;
			}
			break;

		case 0x04:
			if (!SCSIdisk[target].cdrom) {
				uint32 num_sectors = SCSIdisk[target].size / blocksize;
				uint32 cylinders, heads, sectors;
				SCSI_GuessGeometry(num_sectors, &cylinders, &heads, &sectors);

				page.pagesize = 20;
				page.modepage[0] = 0x04;
				page.modepage[1] = 0x12;
				page.modepage[2] = (cylinders >> 16) & 0xFF;
				page.modepage[3] = (cylinders >> 8) & 0xFF;
				page.modepage[4] = cylinders & 0xFF;
				page.modepage[5] = heads & 0xFF;
			}
			break;

		case 0x0D:
			if (SCSIdisk[target].cdrom) {
				page.pagesize = 8;
				page.modepage[0] = 0x0D;
				page.modepage[1] = 0x06;
				page.modepage[2] = 0x00;
				page.modepage[3] = 0x05;
				page.modepage[4] = 0x00;
				page.modepage[5] = 0x3C;
				page.modepage[6] = 0x00;
				page.modepage[7] = 0x4B;
			}
			break;

		case 0x0E:
			if (SCSIdisk[target].cdrom) {
				page.pagesize = 16;
				page.modepage[0] = 0x0E;
				page.modepage[1] = 0x0E;
				page.modepage[2] = 0x04;
				page.modepage[3] = 0x00;
				page.modepage[4] = 0x00;
				page.modepage[5] = 0x4B;
				page.modepage[6] = 0x01; // Output port 0 channel
				page.modepage[7] = 0xFF; // Volume
				page.modepage[8] = 0x02; // Output port 1 channel
				page.modepage[9] = 0xFF; // Volume
			}
			break;

		case 0x2A:
			if (SCSIdisk[target].cdrom) {
				const int cd_speed = 2 * 176;
				page.pagesize = 22;
				page.modepage[0] = 0x2A;
				page.modepage[1] = 0x14;
				page.modepage[2] = 0x03; // CD-R read, CD-ROM read
				page.modepage[3] = 0x00;
				page.modepage[4] = 0x7F;
				page.modepage[5] = 0xFF;
				page.modepage[6] = 0x2D; // Lock supported
				page.modepage[7] = 0x00;
				page.modepage[8] = (cd_speed >> 8) & 0xFF;
				page.modepage[9] = cd_speed & 0xFF;
				page.modepage[10] = 0;
				page.modepage[11] = 0;
				page.modepage[12] = 2048 >> 8;
				page.modepage[13] = 2048 & 0xFF;
				page.modepage[14] = (cd_speed >> 8) & 0xFF;
				page.modepage[15] = cd_speed & 0xFF;
				page.modepage[16] = (cd_speed >> 8) & 0xFF;
				page.modepage[17] = cd_speed & 0xFF;
				page.modepage[18] = (cd_speed >> 8) & 0xFF;
				page.modepage[19] = cd_speed & 0xFF;
				page.modepage[20] = (cd_speed >> 8) & 0xFF;
				page.modepage[21] = cd_speed & 0xFF;
			}
			break;

		case 0x30:
			page.pagesize = 24;
			page.modepage[0] = 0x30;
			page.modepage[1] = 0x16; // 22 bytes payload
			memcpy(&page.modepage[2], "APPLE COMPUTER, INC   ", 22);
			break;

		default:
			page.pagesize = 0;
			break;
	}
	return page;
}

void SCSI_GuessGeometry(uint32 size, uint32 *cylinders, uint32 *heads, uint32 *sectors)
{
	uint32 c, h, s;

	for (h = 16; h > 0; h--) {
		for (s = 63; s > 15; s--) {
			if ((size % (s * h)) == 0) {
				c = size / (s * h);
				*cylinders = c;
				*heads = h;
				*sectors = s;
				return;
			}
		}
	}

	h = 16;
	s = 63;
	c = size / (s * h);
	if ((size % (s * h)) != 0) {
		c += 1;
	}
	*cylinders = c;
	*heads = h;
	*sectors = s;
}

void SCSI_RequestSense(uint8 *cdb)
{
	int nRetLen = SCSI_GetCount(cdb[0], cdb);
	if (nRetLen <= 0) nRetLen = 18;
	if (nRetLen > 22) nRetLen = 22;

	uint8 retbuf[22];
	memset(retbuf, 0, sizeof(retbuf));

	retbuf[0] = 0x70;
	if (SCSIdisk[target].sense.valid) {
		retbuf[0] |= 0x80;
		retbuf[3] = (SCSIdisk[target].sense.info >> 24) & 0xFF;
		retbuf[4] = (SCSIdisk[target].sense.info >> 16) & 0xFF;
		retbuf[5] = (SCSIdisk[target].sense.info >> 8) & 0xFF;
		retbuf[6] = SCSIdisk[target].sense.info & 0xFF;
	}

	if (SCSIdisk[target].sense.key == 0 && SCSIdisk[target].sense.code != 0) {
		switch (SCSIdisk[target].sense.code) {
			case SC_NO_ERROR:
				SCSIdisk[target].sense.key = SK_NOSENSE;
				break;
			case SC_WRITE_FAULT:
			case SC_INVALID_CMD:
			case SC_INVALID_LBA:
			case SC_INVALID_CDB:
			case SC_INVALID_LUN:
				SCSIdisk[target].sense.key = SK_ILLEGAL_REQ;
				break;
			case SC_WRITE_PROTECT:
				SCSIdisk[target].sense.key = SK_DATAPROTECT;
				break;
			case SC_NOT_READY_TO_READY_TRANSITION:
				SCSIdisk[target].sense.key = SK_UNIT_ATN;
				break;
			case SC_MEDIUM_NOT_PRESENT:
				SCSIdisk[target].sense.key = SK_NOTREADY;
				break;
			case SC_NO_SECTOR:
			default:
				SCSIdisk[target].sense.key = SK_HARDWARE;
				break;
		}
	}

	retbuf[2] = SCSIdisk[target].sense.key;
	retbuf[7] = 14;
	retbuf[12] = SCSIdisk[target].sense.code;
	retbuf[13] = SCSIdisk[target].sense.ascq;

	scsi_buffer.size = scsi_buffer.limit = nRetLen;
	memcpy(buffer, retbuf, scsi_buffer.limit);
	scsi_buffer.disk = false;

	SCSI_LOG("[SCSI-EMU] Target %d REQUEST_SENSE: Key 0x%02X, ASC 0x%02X, ASCQ 0x%02X (Valid=%d, Info=0x%08X, RetLen=%d)\n",
	         target, retbuf[2], retbuf[12], retbuf[13], SCSIdisk[target].sense.valid, SCSIdisk[target].sense.info, nRetLen);

	SCSIdisk[target].status = STAT_GOOD;
	SCSIdisk[target].sense.key = SK_NOSENSE;
	SCSIdisk[target].sense.code = SC_NO_ERROR;
	SCSIdisk[target].sense.ascq = 0x00;
	SCSIdisk[target].sense.valid = false;
}
