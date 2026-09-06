/*
 *  rsrc_patches.h - Resource patches
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

#ifndef RSRC_PATCHES_H
#define RSRC_PATCHES_H

#include <vector>

#include "rom_patches.h"	// PatchRecord

extern void CheckLoad(uint32 type, int16 id, uint8 *p, uint32 size);

/*
 *  Resource patches are located by byte signature inside a freshly loaded
 *  resource. A System version whose bytes differ silently gets no patch, so
 *  CheckLoad() records every attempt the same way PatchROM() does.
 *
 *  Unlike the ROM patch log this one is not bounded by a single pass --
 *  CheckLoad() runs for every resource the Mac loads -- so only patch
 *  *attempts* are recorded (a resource we have no patch for adds nothing), and
 *  tests call ClearRsrcPatchLog() to scope it.
 *
 *  See docs/rom-patches-vs-supermario.md section 4 for what each patch
 *  corresponds to in the Apple Mac OS ROM sources.
 */
const std::vector<PatchRecord> &GetRsrcPatchLog(void);
void ClearRsrcPatchLog(void);

#endif
