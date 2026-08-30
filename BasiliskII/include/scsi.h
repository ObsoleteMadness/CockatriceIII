/*
 *  scsi.h - SCSI Manager
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

#ifndef SCSI_H
#define SCSI_H

extern int16 SCSIReset(void);
extern int16 SCSIGet(void);
extern int16 SCSISelect(int id);
extern int16 SCSICmd(int cmd_length, uint8 *cmd);
extern int16 SCSIRead(uint32 tib);
extern int16 SCSIWrite(uint32 tib);
extern int16 SCSIComplete(uint32 timeout, uint32 message, uint32 stat);
extern uint16 SCSIStat(void);
extern int16 SCSIMgrBusy(void);

// System specific and internal functions/data
extern void SCSIInit(void);
extern void SCSIExit(void);

extern bool g_scsi_debug;
extern void SCSI_SetDebug(bool enable);
extern const char *scsi_cmd_name(uint8 opcode);

extern void scsi_set_cmd(int cmd_length, uint8 *cmd);
extern bool scsi_is_target_present(int id);
extern bool scsi_set_target(int id, int lun);
extern bool scsi_send_cmd(size_t data_length, bool reading, int sg_index, uint8 **sg_ptr, uint32 *sg_len, uint16 *stat, uint32 timeout);

#ifdef __cplusplus
extern "C" {
#endif

extern bool SCSI_Attach(int id, const char *path);
extern bool SCSI_Detach(int id);
extern bool SCSI_GetDeviceInfo(int id, bool *present, bool *cdrom, char *path_out, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif
