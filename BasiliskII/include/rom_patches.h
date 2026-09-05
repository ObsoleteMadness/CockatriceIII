/*
 *  rom_patches.h - ROM patches
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

#ifndef ROM_PATCHES_H
#define ROM_PATCHES_H

#include <vector>

/*
 *  ROM version number, set by CheckROM().
 *
 *  These are the version words Apple assigned to each ROM, from the master
 *  table in the Mac OS ROM sources ($SM/Internal/Asm/LinkedPatchMacros.a):
 *
 *      DefineConditions$ (Plus,$0075),(SE,$0276),(II,$0178),
 *                        (Portable,$037A),(IIci,$067C),(SuperMario,$077D)
 *
 *  Note that the version word does NOT uniquely identify a ROM image: Apple
 *  had to add hasTERROR/notTERROR conditionals to tell apart several distinct
 *  ROMs that all report $067C. Anything that must distinguish those needs the
 *  checksum at ROMBase+0 or ProductKind from the UniversalInfo record.
 *
 *  See docs/rom-patches-vs-supermario.md section 2.
 */
enum {
	ROM_VERSION_64K = 0x0000,		// Original Macintosh (64KB)
	ROM_VERSION_PLUS = 0x0075,		// Mac Plus ROMs (128KB)
	ROM_VERSION_II = 0x0178,		// Not 32-bit clean Mac II ROMs (256KB)
	ROM_VERSION_CLASSIC = 0x0276,	// SE/Classic ROMs (256/512KB)
	ROM_VERSION_PORTABLE = 0x037a,	// Mac Portable ROMs (not supported)
	ROM_VERSION_32 = 0x067c,		// 32-bit clean Mac II ROMs (512KB/1MB)
	ROM_VERSION_SUPERMARIO = 0x077d	// SuperMario ROMs (not supported)
};

/*
 *  One entry in the patch log, recorded by every patch PatchROM() attempts.
 *
 *  The log is the machine-readable form of the "[ROM-PATCH]" console output:
 *  offline tests assert against it instead of scraping stdout, and a boot bomb
 *  can be diagnosed by diffing it against the known-good manifest in
 *  BasiliskII/tests/basilisk/fixtures/.
 *
 *  name:       stable identifier, e.g. "InitADB VIA wait". Never reword one of
 *              these without regenerating the golden manifest.
 *  source_ref: the Mac OS ROM source that justifies the patch, e.g.
 *              "$SM/OS/ADBMgr/ADBMgrPatch.a:159-174". May be NULL where no
 *              source exists (Sound Mgr, serial, AppleTalk -- see
 *              docs/rom-patches-vs-supermario.md section 6).
 *  offset:     ROM offset the patch was applied at; 0 when not applied.
 *  required:   true if a miss must fail the whole patch pass.
 *  applied:    false means the site was not found.
 */
struct PatchRecord {
	const char *name;
	const char *source_ref;
	uint32 offset;
	bool required;
	bool applied;
};

/*
 *  Returns the patch log for the most recent PatchROM() call.
 *
 *  Cleared at PatchROM() entry, so it always reflects one pass. Entries are in
 *  application order.
 */
const std::vector<PatchRecord> &GetPatchLog(void);

/*
 *  A trap replaced through _SetTrapAddress at runtime rather than by patching
 *  the ROM image, the way Apple installs its own replacements
 *  ($SM/OS/TimeMgr/TimeMgrPatch.a:159-161).
 *
 *  name:       stable identifier, also used in the [TRAP-INSTALL] log.
 *  source_ref: Mac OS ROM source for the trap being replaced.
 *  trap:       A-line trap number.
 *  code/len:   the stub, valid before installation so tests can execute it.
 *  addr:       where the stub was installed; 0 until InstallDrivers() runs.
 */
struct RuntimeTrapStub {
	const char *name;
	const char *source_ref;
	uint16 trap;
	const uint8 *code;
	uint32 len;
	uint32 addr;
};

/*
 *  Returns the runtime trap stub table.
 *
 *  Arguments:
 *    count: receives the number of entries; may be NULL.
 */
const RuntimeTrapStub *GetRuntimeTrapStubs(int *count);

/*
 *  Returns the 60 Hz VBL handler installed into the ROM's jVBLInt vector.
 *
 *  Arguments:
 *    original: receives the vector value replaced; may be NULL.
 *
 *  Returns:
 *    Mac address of the handler, or 0 before InstallDrivers() has run.
 */
uint32 GetVBLHandlerStub(uint32 *original);

/*
 *  Validate a jVBLInt vector and work out where the ROM handler continues.
 *
 *  Exposed so the offline tests can cover it: this is the check that replaced
 *  the fixed-offset verification of the 60 Hz handler inside PatchROM().
 *
 *  Arguments:
 *    vector:   value read from jVBLInt ($196).
 *    cont_out: receives the continuation address, 0 on failure; may be NULL.
 *
 *  Returns:
 *    true if the vector points at a ROM VBL handler we recognise.
 */
bool ResolveVBLContinuation(uint32 vector, uint32 *cont_out);

extern uint16 ROMVersion;

// ROM offset of breakpoint, used by PatchROM()
extern uint32 ROMBreakpoint;

// ROM offset of UniversalInfo, set by PatchROM()
extern uint32 UniversalInfo;

// Mac address of PutScrap() patch
extern uint32 PutScrapPatch;

// Flag: print ROM information in PatchROM()
extern bool PrintROMInfo;

extern bool CheckROM(void);
extern bool PatchROM(void);
extern void InstallDrivers(uint32 pb);
extern void InstallSERD(void);
extern void PatchAfterStartup(void);

#endif
