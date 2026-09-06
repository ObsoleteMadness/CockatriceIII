/*
 *  slot_rom.h - Slot declaration ROM
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

#ifndef SLOT_ROM_H
#define SLOT_ROM_H

extern bool InstallSlotROM(void);

/*
 * Recalculates the declaration-ROM CRC in place after a VModeParms patch.
 */
void ChecksumSlotROM(void);

/*
 * Patches the slot-ROM VModeParms for one Apple depth (rowBytes and bounds)
 * and refreshes the CRC. Display Manager and pre-7.6 InitGDevice re-read
 * these sResources after cscSwitchMode / cscSetMode.
 *
 * Arguments:
 *   mode: VMODE_* depth whose sResource 0x80+mode table is updated.
 *   width, height: New pixel size written into vpBounds.
 *   row_bytes: Packed bytes per row written into vpRowBytes.
 */
void SlotROM_PatchMode(int mode, int width, int height, uint32 row_bytes);

/*
 *  ROM offset the synthesised declaration ROM was copied to, set by
 *  InstallSlotROM(). It is placed at the tail of the ROM image, so this is
 *  ROMSize minus the generated size. 0 before InstallSlotROM() runs.
 */
extern uint32 SlotROMOffset;

#endif
