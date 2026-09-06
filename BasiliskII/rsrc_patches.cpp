/*
 *  rsrc_patches.cpp - Resource patches
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
#include "audio.h"
#include "audio_defs.h"
#include "rsrc_patches.h"
#include "prefs.h"

#if ENABLE_MON
#include "mon.h"
#endif

#define DEBUG 0
#include "debug.h"


#if !EMULATED_68K
// Assembly functions
extern "C" void Scod060Patch1(void);
extern "C" void Scod060Patch2(void);
extern "C" void ThInitFPUPatch(void);
#endif


/*
 *  Resource patch log
 *
 *  See rsrc_patches.h. Mirrors the [ROM-PATCH] log in rom_patches.cpp so a
 *  resource patch that silently stops matching a System version is visible at
 *  patch time instead of surfacing as a bomb much later.
 */

static std::vector<PatchRecord> rsrc_patch_log;

const std::vector<PatchRecord> &GetRsrcPatchLog(void)
{
	return rsrc_patch_log;
}

void ClearRsrcPatchLog(void)
{
	rsrc_patch_log.clear();
}

/*
 *  Record one resource patch attempt and report it on the console.
 *
 *  Arguments:
 *    name:       stable identifier, e.g. "lpch 31 SCSIDispatch".
 *    source_ref: Mac OS ROM source justifying the patch, or NULL where none
 *                exists (Sound Mgr, serial, AppleTalk).
 *    offset:     offset within the resource, or 0 if the signature was not
 *                found. Note that offset 0 is also a legitimate location for
 *                the whole-resource rewrites, so `applied` is passed
 *                explicitly rather than inferred.
 *    applied:    whether the patch was actually written.
 *
 *  Returns:
 *    applied, so call sites can wrap a condition.
 */

static bool log_rsrc_patch(const char *name, const char *source_ref, uint32 offset, bool applied)
{
	PatchRecord rec;
	rec.name = name;
	rec.source_ref = source_ref;
	rec.offset = offset;
	rec.required = false;		// a resource patch never fails the boot
	rec.applied = applied;
	rsrc_patch_log.push_back(rec);

	if (applied)
		printf("[RSRC-PATCH] %-30s @ %04x\n", name, offset);
	else
		printf("[RSRC-PATCH] %-30s MISSED\n", name);
	// Flush like the [ROM-PATCH] log does. Resource patching runs long after
	// stdout has been redirected to a file and gone block-buffered, so without
	// this the last few KB -- which is the whole of this log -- is still in the
	// buffer when a bomb or a kill ends the process, and it looks as though
	// resource patching never ran at all.
	fflush(stdout);
	return applied;
}


/*
 *  Search resource for byte string, return offset (or 0)
 *
 *  Arguments:
 *    rsrc:       resource data.
 *    max:        size of the resource in bytes.
 *    search:     needle.
 *    search_len: needle length.
 *    ofs:        offset to start scanning from.
 *
 *  Returns:
 *    Offset of the first match, or 0 if absent (offset 0 is treated as "not
 *    found" by every caller, which is why patches at the very start of a
 *    resource are written unconditionally instead).
 */

static uint32 find_rsrc_data(const uint8 *rsrc, uint32 max, const uint8 *search, uint32 search_len, uint32 ofs = 0)
{
	// max and search_len are unsigned: a resource shorter than the needle would
	// underflow "max - search_len" into a huge bound and run off the buffer.
	if (max < search_len)
		return 0;
	while (ofs <= max - search_len) {
		if (!memcmp(rsrc + ofs, search, search_len))
			return ofs;
		ofs++;
	}
	return 0;
}



/*
 *  Install SynchIdleTime() patch
 */

static void patch_idle_time(uint8 *p, uint32 size, int n = 1)
{
	if (!PrefsFindBool("idlewait"))
		return;

	// The trap call itself is easy to find; the ExpandMem reference we actually
	// want to overwrite sits just before it, so anchor on the call and scan back.
	// SynchIdleTime is $SM/Patches/MiscPatches.a:192 (trap $ABF7).
	static const uint8 dat[] = {0x70, 0x03, 0xa0, 0x9f};
	uint32 base = find_rsrc_data(p, size, dat, sizeof(dat));
	if (base >= 0x80) {		// need 0x80 bytes of run-up, or the scan underflows p
		uint8 *pbase = p + base - 0x80;
		static const uint8 dat2[] = {0x20, 0x78, 0x02, 0xb6, 0x41, 0xe8, 0x00, 0x80};
		base = find_rsrc_data(pbase, 0x80, dat2, sizeof(dat2));
		if (base) {
			uint16 *p16 = (uint16 *)(pbase + base);
			*p16++ = htons(M68K_EMUL_OP_IDLE_TIME);
			*p16 = htons(M68K_NOP);
			FlushCodeCache(pbase + base, 4);
			D(bug("  patch %d applied\n", n));
		}
	} else
		base = 0;
	log_rsrc_patch("SynchIdleTime", "$SM/Patches/MiscPatches.a:192", base, base != 0);
	(void)n;
}


/*
 *  Point the fake handle at address 0 somewhere harmless.
 *
 *  'boot' 2 and 'boot' 3 both install a handle whose master pointer is 0, so a
 *  program that dereferences NIL writes over low ROM. On hosts where the ROM
 *  image is write-protected that cannot happen and there is nothing to do; the
 *  patch only exists for hosts with a writable ROM (AmigaOS).
 *
 *  Previously the #if guarding this spanned the 'boot' 3 / 'boot' 2 else-if
 *  boundary, so the entire 'boot' 2 case was compiled out on every platform.
 *  Hoisting it into a function keeps the dispatch reachable -- and visible in
 *  the patch log -- regardless of ROM_IS_WRITE_PROTECTED.
 *
 *  Arguments:
 *    name: log label for this call site.
 *    p:    resource data.
 *    size: resource size in bytes.
 */

static void patch_fake_handle_at_zero(const char *name, uint8 *p, uint32 size)
{
#if ROM_IS_WRITE_PROTECTED
	// Nothing to do: a NIL dereference cannot damage a read-only ROM image.
	(void)p;
	(void)size;
	log_rsrc_patch(name, "$SM/OS/StartMgr/Boot3.a", 0, false);
#else
	// Set fake handle at 0x0000 to some safe place (7.5, 8.0)
	static const uint8 dat[] = {0x20, 0x78, 0x02, 0xae, 0xd1, 0xfc, 0x00, 0x01, 0x00, 0x00, 0x21, 0xc8, 0x00, 0x00};
	uint32 base = find_rsrc_data(p, size, dat, sizeof(dat));
	if (base) {
		uint16 *p16 = (uint16 *)(p + base);
#if defined(AMIGA)
		// Set 0x0000 to scratch memory area
		extern uint32 ScratchMem;
		*p16++ = htons(0x207c);			// move.l	#ScratchMem,a0
		*p16++ = htons(ScratchMem >> 16);
		*p16++ = htons(ScratchMem);
		*p16++ = htons(M68K_NOP);
		*p16 = htons(M68K_NOP);
#else
#error System specific handling for writable ROM is required here
#endif
		FlushCodeCache(p + base, 14);
	}
	log_rsrc_patch(name, "$SM/OS/StartMgr/Boot3.a", base, base != 0);
#endif
}


// Size of the 'sift' -16563 audio dispatch stub, in bytes: 16 words, ending in
// the "rtd #8" that returns to the Component Manager.
#define AUDIO_COMPONENT_STUB_SIZE	32

// Size of the 'ltlk' 0 LocalTalk disable stub, in bytes.
#define LTLK_STUB_SIZE				6


/*
 *  Resource patches via vCheckLoad
 */

void CheckLoad(uint32 type, int16 id, uint8 *p, uint32 size)
{
	uint16 *p16;
	uint32 base;
	D(bug("vCheckLoad %c%c%c%c (%08lx) ID %d, data %08lx, size %ld\n", (char)(type >> 24), (char)((type >> 16) & 0xff), (char )((type >> 8) & 0xff), (char )(type & 0xff), type, id, p, size));

	if (type == 'boot' && id == 3) {
		D(bug(" boot 3 found\n"));

		// Set boot stack pointer (7.5, 7.6, 7.6.1, 8.0).
		//
		// The boot world relocation is $SM/OS/StartMgr/Boot3.a:1066-1106. Note
		// that 7.1 puts the boot stack at MemTop/2 while the signature matched
		// here computes MemTop - MemTop/4; same job, later arithmetic.
		static const uint8 dat[] = {0x22, 0x00, 0xe4, 0x89, 0x90, 0x81, 0x22, 0x40};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base + 6);
			*p16 = htons(M68K_EMUL_OP_FIX_BOOTSTACK);
			FlushCodeCache(p + base + 6, 2);
		}
		log_rsrc_patch("boot 3 stack pointer", "$SM/OS/StartMgr/Boot3.a:1066", base, base != 0);

		patch_fake_handle_at_zero("boot 3 fake handle @0", p, size);

	} else if (type == 'boot' && id == 2) {
		D(bug(" boot 2 found\n"));

		patch_fake_handle_at_zero("boot 2 fake handle @0", p, size);

	} else if (type == 'PTCH' && id == 630) {
		D(bug("PTCH 630 found\n"));

		// Don't replace Time Manager (Classic ROM, 6.0.3)
		// moveq #$58,d0 / _SetTrapAddress -- the _InsTime line of
		// InstallTimeMgrPlusSEII ($SM/OS/TimeMgr/TimeMgrPatch.a:140).
		static const uint8 dat[] = {0x30, 0x3c, 0x00, 0x58, 0xa2, 0x47};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base);
			p16[2] = htons(M68K_NOP);
			p16[7] = htons(M68K_NOP);
			p16[12] = htons(M68K_NOP);
			FlushCodeCache(p + base, 26);
		}
		log_rsrc_patch("PTCH 630 TimeMgr (6.0.3)", "$SM/OS/TimeMgr/TimeMgrPatch.a:140", base, base != 0);

		// Don't replace Time Manager (Classic ROM, 6.0.8)
		static const uint8 dat2[] = {0x70, 0x58, 0xa2, 0x47};
		base = find_rsrc_data(p, size, dat2, sizeof(dat2));
		if (base) {
			p16 = (uint16 *)(p + base);
			p16[1] = htons(M68K_NOP);
			p16[5] = htons(M68K_NOP);
			p16[9] = htons(M68K_NOP);
			FlushCodeCache(p + base, 20);
		}
		log_rsrc_patch("PTCH 630 TimeMgr (6.0.8)", "$SM/OS/TimeMgr/TimeMgrPatch.a:140", base, base != 0);

	} else if (type == 'ptch' && id == 26) {
		D(bug(" ptch 26 found\n"));

		// Trap ABC4 is initialized with absolute ROM address (7.5, 7.6, 7.6.1, 8.0)
		// Trap $ABC4 is _GetPMData; 'ptch' 26 is built from QDciPatchROM.a
		// ($SM/Resources/Sys.r:2555, entry at QDciPatchROM.a:19383) and hardcodes
		// a ROM address that Apple bound per-ROM with the ROMBind macro.
		static const uint8 dat[] = {0x40, 0x83, 0x36, 0x10};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base);
			*p16++ = htons((ROMBaseMac + 0x33610) >> 16);
			*p16 = htons((ROMBaseMac + 0x33610) & 0xffff);
			FlushCodeCache(p + base, 4);
		}
		log_rsrc_patch("ptch 26 GetPMData addr", "$SM/QuickDraw/Patches/QDciPatchROM.a:19383", base, base != 0);

	} else if (type == 'ptch' && id == 34) {
		D(bug(" ptch 34 found\n"));

		// Don't wait for VIA (Classic ROM, 6.0.8)
		// The state-3 spin in InitADB: movea.l VIA,a1 / move.b vBufB(a1),d0 /
		// andi.b #$30,d0 / cmpi.b #$30,d0 / bne.s @wait.
		// $SM/OS/ADBMgr/ADBMgrPatch.a:159-174. NOP the bne so it falls through.
		static const uint8 dat[] = {0x22, 0x78, 0x01, 0xd4, 0x10, 0x11, 0x02, 0x00, 0x00, 0x30};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base + 14);
			*p16 = htons(M68K_NOP);
			FlushCodeCache(p + base + 14, 2);
		}
		log_rsrc_patch("ptch 34 InitADB VIA wait", "$SM/OS/ADBMgr/ADBMgrPatch.a:159", base, base != 0);

		// Don't replace ADBOp() (Classic ROM, 6.0.8)
		// patchADBOp PatchProc _ADBOp ($SM/OS/ADBMgr/ADBMgrPatch.a:110)
		static const uint8 dat2[] = {0x21, 0xc0, 0x05, 0xf0};
		base = find_rsrc_data(p, size, dat2, sizeof(dat2));
		if (base) {
			p16 = (uint16 *)(p + base);
			*p16++ = htons(M68K_NOP);
			*p16 = htons(M68K_NOP);
			FlushCodeCache(p + base, 4);
		}
		log_rsrc_patch("ptch 34 ADBOp replace", "$SM/OS/ADBMgr/ADBMgrPatch.a:110", base, base != 0);

#if !EMULATED_68K
	} else if (CPUIs68060 && (type == 'gpch' && id == 669 || type == 'lpch' && id == 63)) {
		D(bug(" gpch 669/lpch 63 found\n"));

		static uint16 ThPatchSpace[1024];	// Replacement routines are constructed here
		uint16 *q = ThPatchSpace;
		uint32 start;
		int i;

		// Patch Thread Manager thread switcher for 68060 FPU (7.5, 8.0)
		static const uint8 dat[] = {0x22, 0x6f, 0x00, 0x08, 0x20, 0x2f, 0x00, 0x04, 0x67, 0x18};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {	// Skip first routine (no FPU -> no FPU)

			base = find_rsrc_data(p, size - base - 2, dat, sizeof(dat), base + 2);
			if (base) {	// no FPU -> FPU

				p16 = (uint16 *)(p + base);
				start = (uint32)q;
				for (i=0; i<28; i++) *q++ = *p16++;
				*q++ = htons(0x4a2f);		// tst.b 2(sp)				(null FPU state or "FPU state saved" flag set?)
				*q++ = htons(2);
				*q++ = htons(0x6712);		// beq
				*q++ = htons(0x588f);		// addq.l #2,sp				(flag set, skip it)
				*q++ = htons(0xf21f);		// fmove.l (sp)+,fpcr		(restore FPU registers)
				*q++ = htons(0x9000);
				*q++ = htons(0xf21f);		// fmove.l (sp)+,fpsr
				*q++ = htons(0x8800);
				*q++ = htons(0xf21f);		// fmove.l (sp)+,fpiar
				*q++ = htons(0x8400);
				*q++ = htons(0xf21f);		// fmovem.x (sp)+,fp0-fp7
				*q++ = htons(0xd0ff);
				*q++ = htons(0xf35f);		// frestore (sp)+
				*q++ = htons(0x4e75);		// rts

				p16 = (uint16 *)(p + base);
				*p16++ = htons(M68K_JMP);
				*p16++ = htons(start >> 16);
				*p16 = htons(start & 0xffff);
				FlushCodeCache(p + base, 6);
				D(bug("  patch 1 applied\n"));

				static const uint8 dat2[] = {0x22, 0x6f, 0x00, 0x08, 0x20, 0x2f, 0x00, 0x04, 0x67, 0x28};
				base = find_rsrc_data(p, size, dat2, sizeof(dat2));
				if (base) {	// FPU -> FPU

					p16 = (uint16 *)(p + base);
					start = (uint32)q;
					for (i=0; i<4; i++) *q++ = *p16++;
					*q++ = htons(0x6736);		// beq
					*q++ = htons(0xf327);		// fsave -(sp)				(save FPU state frame)
					*q++ = htons(0x4a2f);		// tst.b 2(sp)				(null FPU state?)
					*q++ = htons(2);
					*q++ = htons(0x6716);		// beq
					*q++ = htons(0xf227);		// fmovem.x fp0-fp7,-(sp)	(no, save FPU registers)
					*q++ = htons(0xe0ff);
					*q++ = htons(0xf227);		// fmove.l fpiar,-(sp)
					*q++ = htons(0xa400);
					*q++ = htons(0xf227);		// fmove.l fpsr,-(sp)
					*q++ = htons(0xa800);
					*q++ = htons(0xf227);		// fmove.l fpcr,-(sp)
					*q++ = htons(0xb000);
					*q++ = htons(0x4879);		// pea -1					(push "FPU state saved" flag)
					*q++ = htons(0xffff);
					*q++ = htons(0xffff);
					p16 += 9;
					for (i=0; i<23; i++) *q++ = *p16++;
					*q++ = htons(0x4a2f);		// tst.b 2(sp)				(null FPU state or "FPU state saved" flag set?)
					*q++ = htons(2);
					*q++ = htons(0x6712);		// beq
					*q++ = htons(0x588f);		// addq.l #2,sp				(flag set, skip it)
					*q++ = htons(0xf21f);		// fmove.l (sp)+,fpcr		(restore FPU registers)
					*q++ = htons(0x9000);
					*q++ = htons(0xf21f);		// fmove.l (sp)+,fpsr
					*q++ = htons(0x8800);
					*q++ = htons(0xf21f);		// fmove.l (sp)+,fpiar
					*q++ = htons(0x8400);
					*q++ = htons(0xf21f);		// fmovem.x (sp)+,fp0-fp7
					*q++ = htons(0xd0ff);
					*q++ = htons(0xf35f);		// frestore (sp)+
					*q++ = htons(0x4e75);		// rts

					p16 = (uint16 *)(p + base);
					*p16++ = htons(M68K_JMP);
					*p16++ = htons(start >> 16);
					*p16 = htons(start & 0xffff);
					FlushCodeCache(p + base, 6);
					D(bug("  patch 2 applied\n"));

					base = find_rsrc_data(p, size - base - 2, dat2, sizeof(dat2), base + 2);
					if (base) {	// FPU -> no FPU
	
						p16 = (uint16 *)(p + base);
						start = (uint32)q;
						for (i=0; i<4; i++) *q++ = *p16++;
						*q++ = htons(0x6736);		// beq
						*q++ = htons(0xf327);		// fsave -(sp)				(save FPU state frame)
						*q++ = htons(0x4a2f);		// tst.b 2(sp)				(null FPU state?)
						*q++ = htons(2);
						*q++ = htons(0x6716);		// beq
						*q++ = htons(0xf227);		// fmovem.x fp0-fp7,-(sp)	(no, save FPU registers)
						*q++ = htons(0xe0ff);
						*q++ = htons(0xf227);		// fmove.l fpiar,-(sp)
						*q++ = htons(0xa400);
						*q++ = htons(0xf227);		// fmove.l fpsr,-(sp)
						*q++ = htons(0xa800);
						*q++ = htons(0xf227);		// fmove.l fpcr,-(sp)
						*q++ = htons(0xb000);
						*q++ = htons(0x4879);		// pea -1					(push "FPU state saved" flag)
						*q++ = htons(0xffff);
						*q++ = htons(0xffff);
						p16 += 9;
						for (i=0; i<24; i++) *q++ = *p16++;

						p16 = (uint16 *)(p + base);
						*p16++ = htons(M68K_JMP);
						*p16++ = htons(start >> 16);
						*p16 = htons(start & 0xffff);
						FlushCodeCache(p + base, 6);
						D(bug("  patch 3 applied\n"));
					}
				}
			}
		}

		// Patch Thread Manager thread switcher for 68060 FPU (additional routines under 8.0 for Mixed Mode Manager)
		static const uint8 dat3[] = {0x22, 0x6f, 0x00, 0x08, 0x20, 0x2f, 0x00, 0x04, 0x67, 0x40};
		base = find_rsrc_data(p, size, dat3, sizeof(dat3));
		if (base) {	// Skip first routine (no FPU -> no FPU)

			base = find_rsrc_data(p, size - base - 2, dat3, sizeof(dat3), base + 2);
			if (base) {	// no FPU -> FPU

				p16 = (uint16 *)(p + base);
				start = (uint32)q;
				for (i=0; i<48; i++) *q++ = *p16++;
				*q++ = htons(0x4a2f);		// tst.b 2(sp)				(null FPU state or "FPU state saved" flag set?)
				*q++ = htons(2);
				*q++ = htons(0x6712);		// beq
				*q++ = htons(0x588f);		// addq.l #2,sp				(flag set, skip it)
				*q++ = htons(0xf21f);		// fmove.l (sp)+,fpcr		(restore FPU registers)
				*q++ = htons(0x9000);
				*q++ = htons(0xf21f);		// fmove.l (sp)+,fpsr
				*q++ = htons(0x8800);
				*q++ = htons(0xf21f);		// fmove.l (sp)+,fpiar
				*q++ = htons(0x8400);
				*q++ = htons(0xf21f);		// fmovem.x (sp)+,fp0-fp7
				*q++ = htons(0xd0ff);
				p16 += 7;
				for (i=0; i<20; i++) *q++ = *p16++;

				p16 = (uint16 *)(p + base);
				*p16++ = htons(M68K_JMP);
				*p16++ = htons(start >> 16);
				*p16 = htons(start & 0xffff);
				FlushCodeCache(p + base, 6);
				D(bug("  patch 4 applied\n"));

				static const uint8 dat4[] = {0x22, 0x6f, 0x00, 0x08, 0x20, 0x2f, 0x00, 0x04, 0x67, 0x50};
				base = find_rsrc_data(p, size, dat4, sizeof(dat4));
				if (base) {	// FPU -> FPU

					p16 = (uint16 *)(p + base);
					start = (uint32)q;
					for (i=0; i<4; i++) *q++ = *p16++;
					*q++ = htons(0x675e);		// beq
					p16++;
					for (i=0; i<21; i++) *q++ = *p16++;
					*q++ = htons(0x4a2f);		// tst.b 2(sp)				(null FPU state?)
					*q++ = htons(2);
					*q++ = htons(0x6716);		// beq
					*q++ = htons(0xf227);		// fmovem.x fp0-fp7,-(sp)	(no, save FPU registers)
					*q++ = htons(0xe0ff);
					*q++ = htons(0xf227);		// fmove.l fpiar,-(sp)
					*q++ = htons(0xa400);
					*q++ = htons(0xf227);		// fmove.l fpsr,-(sp)
					*q++ = htons(0xa800);
					*q++ = htons(0xf227);		// fmove.l fpcr,-(sp)
					*q++ = htons(0xb000);
					*q++ = htons(0x4879);		// pea -1					(push "FPU state saved" flag)
					*q++ = htons(0xffff);
					*q++ = htons(0xffff);
					p16 += 7;
					for (i=0; i<23; i++) *q++ = *p16++;
					*q++ = htons(0x4a2f);		// tst.b 2(sp)				(null FPU state or "FPU state saved" flag set?)
					*q++ = htons(2);
					*q++ = htons(0x6712);		// beq
					*q++ = htons(0x588f);		// addq.l #2,sp				(flag set, skip it)
					*q++ = htons(0xf21f);		// fmove.l (sp)+,fpcr		(restore FPU registers)
					*q++ = htons(0x9000);
					*q++ = htons(0xf21f);		// fmove.l (sp)+,fpsr
					*q++ = htons(0x8800);
					*q++ = htons(0xf21f);		// fmove.l (sp)+,fpiar
					*q++ = htons(0x8400);
					*q++ = htons(0xf21f);		// fmovem.x (sp)+,fp0-fp7
					*q++ = htons(0xd0ff);
					p16 += 7;
					for (i=0; i<20; i++) *q++ = *p16++;

					p16 = (uint16 *)(p + base);
					*p16++ = htons(M68K_JMP);
					*p16++ = htons(start >> 16);
					*p16 = htons(start & 0xffff);
					FlushCodeCache(p + base, 6);
					D(bug("  patch 5 applied\n"));

					base = find_rsrc_data(p, size - base - 2, dat4, sizeof(dat4), base + 2);
					if (base) {	// FPU -> no FPU

						p16 = (uint16 *)(p + base);
						start = (uint32)q;
						for (i=0; i<4; i++) *q++ = *p16++;
						*q++ = htons(0x675e);		// beq
						p16++;
						for (i=0; i<21; i++) *q++ = *p16++;
						*q++ = htons(0x4a2f);		// tst.b 2(sp)				(null FPU state?)
						*q++ = htons(2);
						*q++ = htons(0x6716);		// beq
						*q++ = htons(0xf227);		// fmovem.x fp0-fp7,-(sp)	(no, save FPU registers)
						*q++ = htons(0xe0ff);
						*q++ = htons(0xf227);		// fmove.l fpiar,-(sp)
						*q++ = htons(0xa400);
						*q++ = htons(0xf227);		// fmove.l fpsr,-(sp)
						*q++ = htons(0xa800);
						*q++ = htons(0xf227);		// fmove.l fpcr,-(sp)
						*q++ = htons(0xb000);
						*q++ = htons(0x4879);		// pea -1					(push "FPU state saved" flag)
						*q++ = htons(0xffff);
						*q++ = htons(0xffff);
						p16 += 7;
						for (i=0; i<42; i++) *q++ = *p16++;

						p16 = (uint16 *)(p + base);
						*p16++ = htons(M68K_JMP);
						*p16++ = htons(start >> 16);
						*p16 = htons(start & 0xffff);
						FlushCodeCache(p + base, 6);
						D(bug("  patch 6 applied\n"));
					}
				}
			}
		}

		FlushCodeCache(ThPatchSpace, 1024);

		// Patch Thread Manager FPU init for 68060 FPU (7.5, 8.0)
		static const uint8 dat5[] = {0x4a, 0x28, 0x00, 0xa4, 0x67, 0x0a, 0x4a, 0x2c, 0x00, 0x40};
		base = find_rsrc_data(p, size, dat5, sizeof(dat5));
		if (base) {
			p16 = (uint16 *)(p + base + 6);
			*p16++ = htons(M68K_JSR);
			*p16++ = htons((uint32)ThInitFPUPatch >> 16);
			*p16++ = htons((uint32)ThInitFPUPatch & 0xffff);
			*p16++ = htons(M68K_NOP);
			*p16 = htons(M68K_NOP);
			FlushCodeCache(p + base + 6, 10);
			D(bug("  patch 7 applied\n"));
		}
#endif

	} else if (type == 'gpch' && id == 750) {
		D(bug(" gpch 750 found\n"));

		// Don't use PTEST instruction in BlockMove() (7.5, 7.6, 7.6.1, 8.0)
		static const uint8 dat[] = {0xa0, 0x8d, 0x0c, 0x81, 0x00, 0x00, 0x0c, 0x00, 0x65, 0x06, 0x4e, 0x71, 0xf4, 0xf8};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base + 8);
			*p16 = htons(M68K_NOP);
			FlushCodeCache(p + base + 8, 2);
		}
		log_rsrc_patch("gpch 750 BlockMove PTEST", "$SM/OS/MemoryMgr/BlockMove.a:435", base, base != 0);

		// Patch SynchIdleTime()
		patch_idle_time(p, size, 2);

	} else if (type == 'lpch' && id == 24) {
		D(bug(" lpch 24 found\n"));

		// Don't replace Time Manager (7.0.1, 7.1, 7.5, 7.6, 7.6.1, 8.0)
		// lpch 24 == 0b11000 == Portable+IIci, matching
		// "InstallTimeMgrPortableIIci InstallProc (Portable,IIci,notAUX)"
		// ($SM/OS/TimeMgr/TimeMgrPatch.a:184). The signature is its
		// "moveq #$59,d0 / _SetTrapAddress" (_RmvTime) at :190-191; the three
		// NOPs suppress the _RmvTime, _PrimeTime and __Microseconds
		// installations. The Lvl1DT write at :198 is deliberately left alone.
		static const uint8 dat[] = {0x70, 0x59, 0xa2, 0x47};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base + 2);
			*p16++ = htons(M68K_NOP);
			p16 += 3;
			*p16++ = htons(M68K_NOP);
			p16 += 7;
			*p16 = htons(M68K_NOP);
			FlushCodeCache(p + base + 2, 28);
		}
		log_rsrc_patch("lpch 24 TimeMgr replace", "$SM/OS/TimeMgr/TimeMgrPatch.a:184", base, base != 0);

	} else if (type == 'lpch' && id == 31) {
		D(bug(" lpch 31 found\n"));

		// Don't write to VIA in vSoundDead() (7.0.1, 7.1, 7.5, 7.6, 7.6.1, 8.0)
		// jSoundDead is the low-memory vector at $06E0
		// ($SM/Interfaces/AIncludes/Private.a:411); the Memory Manager calls
		// through it at $SM/OS/MemoryMgr/MemoryMgr.a:993.
		static const uint8 dat[] = {0x20, 0x78, 0x01, 0xd4, 0x08, 0xd0, 0x00, 0x07, 0x4e, 0x75};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base);
			*p16 = htons(M68K_RTS);
			FlushCodeCache(p + base, 2);
		}
		log_rsrc_patch("lpch 31 vSoundDead VIA", "$SM/OS/MemoryMgr/MemoryMgr.a:993", base, base != 0);

		// Don't replace SCSI manager (7.1, 7.5, 7.6.1, 8.0)
		// lpch 31 == 0b11111 == all five pre-SuperMario ROMs, matching
		// "SCSIDispatchCommon PatchProc _SCSIDispatch,(Plus,SE,Portable,II,IIci,notAUX)"
		// ($SM/OS/SCSIMgr/SCSILinkPatch.a:221).
		static const uint8 dat2[] = {0x0c, 0x6f, 0x00, 0x0e, 0x00, 0x04, 0x66, 0x0c};
		base = find_rsrc_data(p, size, dat2, sizeof(dat2));
		if (base) {
			p16 = (uint16 *)(p + base);
			*p16++ = htons(M68K_EMUL_OP_SCSI_DISPATCH);
			*p16++ = htons(0x2e49);		// move.l	a1,a7
			*p16 = htons(M68K_JMP_A0);
			FlushCodeCache(p + base, 6);
		}
		log_rsrc_patch("lpch 31 SCSIDispatch", "$SM/OS/SCSIMgr/SCSILinkPatch.a:221", base, base != 0);

		// Patch SynchIdleTime()
		patch_idle_time(p, size, 3);

#if !EMULATED_68K
	} else if (CPUIs68060 && type == 'scod' && (id == -16463 || id == -16464)) {
		D(bug(" scod -16463/-16464 found\n"));

		// Correct 68060 FP frame handling in Process Manager task switches (7.1, 7.5, 8.0)
		static const uint8 dat[] = {0xf3, 0x27, 0x4a, 0x17};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base);
			*p16++ = htons(M68K_JMP);
			*p16++ = htons((uint32)Scod060Patch1 >> 16);
			*p16 = htons((uint32)Scod060Patch1 & 0xffff);
			FlushCodeCache(p + base, 6);
			D(bug("  patch 1 applied\n"));
		}

		// Even a null FP frame is 3 longwords on the 68060 (7.1, 7.5, 8.0)
		static const uint8 dat2[] = {0xf3, 0x5f, 0x4e, 0x75};
		base = find_rsrc_data(p, size, dat2, sizeof(dat2));
		if (base) {
			p16 = (uint16 *)(p + base - 2);
			*p16++ = htons(M68K_JMP);
			*p16++ = htons((uint32)Scod060Patch2 >> 16);
			*p16 = htons((uint32)Scod060Patch2 & 0xffff);
			FlushCodeCache(p + base - 2, 6);
			D(bug("  patch 2 applied\n"));
		}
#endif

	} else if (type == 'thng' && id == -16563) {
		D(bug(" thng -16563 found\n"));

		// Set audio component flags (7.5, 7.6, 7.6.1, 8.0).
		// No SuperMario source: the Sound Manager ships as SoundMgr.rsrc;
		// componentFlags is the ComponentDescription field at +12.
		//
		// This writes at a fixed offset with no signature to find, so unlike
		// every other patch here a short resource is a write past the end of the
		// handle rather than a missed match. Refuse instead: sound not working
		// is recoverable, a corrupted Mac heap is not.
		if (size < componentFlags + 4) {
			log_rsrc_patch("thng -16563 audio flags", NULL, 0, false);
		} else {
			*(uint32 *)(p + componentFlags) = htonl(audio_component_flags);
			log_rsrc_patch("thng -16563 audio flags", NULL, componentFlags, true);
		}

	} else if (type == 'sift' && id == -16563) {
		D(bug(" sift -16563 found\n"));

		// Replace audio component (7.5, 7.6, 7.6.1, 8.0).
		// Same unconditional-write hazard as thng -16563 above: the stub is 32
		// bytes and the resource is never scanned, so check it fits first.
		if (size < AUDIO_COMPONENT_STUB_SIZE) {
			log_rsrc_patch("sift -16563 audio component", NULL, 0, false);
		} else {
			p16 = (uint16 *)p;
			*p16++ = htons(0x4e56); *p16++ = htons(0x0000);	// link		a6,#0
			*p16++ = htons(0x48e7); *p16++ = htons(0x8018);	// movem.l	d0/a3-a4,-(sp)
			*p16++ = htons(0x266e); *p16++ = htons(0x000c);	// movea.l	12(a6),a3
			*p16++ = htons(0x286e); *p16++ = htons(0x0008);	// movea.l	8(a6),a4
			*p16++ = htons(M68K_EMUL_OP_AUDIO);
			*p16++ = htons(0x2d40); *p16++ = htons(0x0010);	// move.l	d0,16(a6)
			*p16++ = htons(0x4cdf); *p16++ = htons(0x1801);	// movem.l	(sp)+,d0/a3-a4
			*p16++ = htons(0x4e5e);							// unlk		a6
			*p16++ = htons(0x4e74); *p16++ = htons(0x0008);	// rtd		#8
			FlushCodeCache(p, AUDIO_COMPONENT_STUB_SIZE);
			log_rsrc_patch("sift -16563 audio component", NULL, 0, true);
		}

	} else if (type == 'inst' && id == -19069) {
		D(bug(" inst -19069 found\n"));

		// Don't replace Microseconds (QuickTime 2.0)
		// QuickTime replacing _Microseconds ($A193). Apple's own fix was the
		// gestaltFixedMicroseconds bit ($SM/OS/Gestalt/GestaltExtensions.a:30).
		static const uint8 dat[] = {0x30, 0x3c, 0xa1, 0x93, 0xa2, 0x47};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base + 4);
			*p16 = htons(M68K_NOP);
			FlushCodeCache(p + base + 4, 2);
		}
		log_rsrc_patch("inst -19069 Microseconds", "$SM/OS/Gestalt/GestaltExtensions.a:30", base, base != 0);

	} else if (type == 'DRVR' && id == -20066) {
		D(bug("DRVR -20066 found\n"));

		// Don't access SCC in .Infra driver
		static const uint8 dat[] = {0x28, 0x78, 0x01, 0xd8, 0x48, 0xc7, 0x20, 0x0c, 0xd0, 0x87, 0x20, 0x40, 0x1c, 0x10};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base + 12);
			*p16 = htons(0x7a00);	// moveq #0,d6
			FlushCodeCache(p + base + 12, 2);
		}
		log_rsrc_patch("DRVR -20066 .Infra SCC", NULL, base, base != 0);

	} else if (type == 'ltlk' && id == 0) {
		// Disable LocalTalk (7.0.1, 7.5, 7.6, 7.6.1, 8.0) unless ltoudp is
		// enabled. No SuperMario source: AppleTalk ships as AppleTalk.rsrc,
		// built into the System as 'lmgr' 0 ($SM/Resources/Sys.r:1092).
		// Another fixed-offset write with no signature to find, so bound it
		// against the resource size the way the audio component patches are.
		bool disable = !PrefsFindBool("ltoudp") && size >= LTLK_STUB_SIZE;
		if (disable) {
			p16 = (uint16 *)p;
			*p16++ = htons(M68K_JMP_A0);
			*p16++ = htons(0x7000);
			*p16 = htons(M68K_RTS);
			FlushCodeCache(p, LTLK_STUB_SIZE);
		}
		log_rsrc_patch(disable ? "ltlk 0 disable LocalTalk" : "ltlk 0 kept for LToUDP", NULL, 0, disable);
	}
}
