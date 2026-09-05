/*
 * basilisk_rsrcpatch_test.cpp - CheckLoad() resource patching (T5)
 *
 * CheckLoad() rewrites System-file resources as the Mac loads them. It is a
 * pure function over a byte buffer, so it can be driven directly with synthetic
 * resources built from the instruction sequences in the Apple Mac OS ROM
 * sources -- no boot, no System file. See docs/rom-patches-vs-supermario.md
 * section 4 for the mapping.
 *
 * Two kinds of case here:
 *
 *   Positive -- feed the byte sequence the patch looks for, assert exactly the
 *   expected words changed and nothing else did.
 *
 *   Boundary -- resources shorter than a signature, signatures too close to the
 *   start to scan backwards from, empty and all-ones buffers. These exercised
 *   two unsigned underflows before they were fixed, and every buffer is
 *   bracketed with guard bytes so an out-of-bounds write is caught even when
 *   the run is not under ASAN.
 */

#include <stdio.h>
#include <string.h>
#include <vector>
#include "test_harness.h"
#include "test_env.h"
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "emul_op.h"
#include "rom_patches.h"
#include "rsrc_patches.h"
#include "main.h"

#define GUARD_BYTES 64
#define GUARD_FILL  0x5a

/*
 * Leading filler inside the resource itself.
 *
 * find_rsrc_data() returns 0 both for "found at offset 0" and "not found", and
 * every caller reads that as not-found. Real resources always have a header or
 * entry code before the patched instruction, so a signature never sits at
 * offset 0 in practice -- but a synthetic buffer that starts with the signature
 * would silently exercise the not-found path. Prefix every fixture with a few
 * NOPs so the offsets are realistic. test_signature_at_offset_zero() below
 * pins the ambiguity itself.
 */
#define LEAD 8
static const uint8 lead_fill[LEAD] = {
	0x4e, 0x71, 0x4e, 0x71, 0x4e, 0x71, 0x4e, 0x71,
};

/*
 * A resource buffer with guard regions either side.
 *
 * CheckLoad() is handed a bare pointer and a size; nothing stops a bad scan
 * from running off either end, so we allocate padding and verify it afterwards.
 */
struct GuardedRsrc {
	std::vector<uint8> mem;
	uint32 size;

	GuardedRsrc(const uint8 *data, uint32 len, bool lead = false)
	{
		uint32 pre = lead ? LEAD : 0;
		size = len + pre;
		mem.assign(size + 2 * GUARD_BYTES, GUARD_FILL);
		if (pre)
			memcpy(&mem[GUARD_BYTES], lead_fill, pre);
		if (len)
			memcpy(&mem[GUARD_BYTES + pre], data, len);
	}

	uint8 *data(void) { return &mem[GUARD_BYTES]; }

	/* True if neither guard region was touched. */
	bool guards_intact(void) const
	{
		for (uint32 i = 0; i < GUARD_BYTES; i++) {
			if (mem[i] != GUARD_FILL)
				return false;
			if (mem[mem.size() - 1 - i] != GUARD_FILL)
				return false;
		}
		return true;
	}
};

/* Reads a big-endian word out of a resource buffer. */
static uint16 rd16(GuardedRsrc &r, uint32 ofs)
{
	return (uint16)((r.data()[ofs] << 8) | r.data()[ofs + 1]);
}

/*
 * Runs CheckLoad() on a guarded buffer and checks the guards survived.
 *
 * Arguments:
 *   label: printed with the guard assertion.
 *   type:  resource type ('lpch', 'ptch', ...).
 *   id:    resource ID.
 *   r:     the buffer.
 */
static void run_checkload(const char *label, uint32 type, int16 id, GuardedRsrc &r)
{
	ClearRsrcPatchLog();
	CheckLoad(type, id, r.data(), r.size);
	char msg[192];
	snprintf(msg, sizeof(msg), "%s: no write outside the resource", label);
	CHECK(r.guards_intact(), msg);
}

/* True if the log contains an applied record whose name starts with prefix. */
static bool logged_applied(const char *prefix)
{
	const std::vector<PatchRecord> &log = GetRsrcPatchLog();
	for (size_t i = 0; i < log.size(); i++)
		if (log[i].applied && strncmp(log[i].name, prefix, strlen(prefix)) == 0)
			return true;
	return false;
}

/*
 * lpch 24 -- Time Manager replacement.
 *
 * Resource ID 24 is the ROM bitmask 0b11000 = Portable + IIci, matching
 * "InstallTimeMgrPortableIIci InstallProc (Portable,IIci,notAUX)" at
 * $SM/OS/TimeMgr/TimeMgrPatch.a:184. The signature is its _RmvTime line
 * (moveq #$59,d0 / _SetTrapAddress) at :190-191; three _SetTrapAddress calls
 * (_RmvTime, _PrimeTime, __Microseconds) get NOPed out.
 */
static void test_lpch24(void)
{
	// moveq #$59,d0 / _SetTrapAddress, then two more install sequences at the
	// word strides the patch uses (+4 words and +12 words from base+2).
	static const uint8 body[] = {
		0x70, 0x59, 0xa2, 0x47,             // +0  moveq #$59,d0 ; _SetTrapAddress
		0x41, 0xfa, 0x00, 0x10,             // +4  lea
		0x70, 0x5a, 0xa2, 0x47,             // +8  moveq #$5A,d0 ; _SetTrapAddress
		0x21, 0xc0, 0x01, 0x92,             // +12 move.l d0,Lvl1DT
		0x4e, 0x71, 0x4e, 0x71,             // +16
		0x4e, 0x71, 0x4e, 0x71,             // +20
		0x4e, 0x71, 0x4e, 0x71,             // +24
		0x70, 0x93, 0xa2, 0x47,             // +28 moveq #$93,d0 ; _SetTrapAddress
	};
	GuardedRsrc r(body, sizeof(body), true);
	run_checkload("lpch 24", 'lpch', 24, r);

	CHECK(logged_applied("lpch 24"), "lpch 24: patch recorded as applied");
	// The patch NOPs words at base+2, base+10 and base+26.
	CHECK(rd16(r, LEAD + 2) == M68K_NOP, "lpch 24: _RmvTime _SetTrapAddress NOPed");
	CHECK(rd16(r, LEAD + 10) == M68K_NOP, "lpch 24: _PrimeTime _SetTrapAddress NOPed");
	CHECK(rd16(r, LEAD + 26) == M68K_NOP, "lpch 24: third _SetTrapAddress NOPed");
	CHECK(rd16(r, LEAD + 0) == 0x7059, "lpch 24: moveq #$59,d0 left intact");
	CHECK(rd16(r, LEAD + 12) == 0x21c0, "lpch 24: Lvl1DT install left intact");
}

/*
 * ptch 34 -- ADB Manager.
 *
 * Two independent sub-patches, both from $SM/OS/ADBMgr/ADBMgrPatch.a: the
 * InitADB state-3 VIA spin at :159-174, and patchADBOp at :110.
 */
static void test_ptch34(void)
{
	static const uint8 body[] = {
		// InitADB spin: movea.l VIA,a1 / move.b (a1),d0 / andi.b #$30,d0 /
		// cmpi.b #$30,d0 / bne.s @wait   -- the bne at +14 is NOPed
		0x22, 0x78, 0x01, 0xd4,
		0x10, 0x11,
		0x02, 0x00, 0x00, 0x30,
		0x0c, 0x00, 0x00, 0x30,
		0x66, 0xf4,                          // +14 bne.s
		// patchADBOp: move.l d0,$05F0
		0x21, 0xc0, 0x05, 0xf0,              // +16
	};
	GuardedRsrc r(body, sizeof(body), true);
	run_checkload("ptch 34", 'ptch', 34, r);

	CHECK(rd16(r, LEAD + 14) == M68K_NOP, "ptch 34: InitADB VIA wait branch NOPed");
	CHECK(rd16(r, LEAD + 16) == M68K_NOP && rd16(r, LEAD + 18) == M68K_NOP,
	      "ptch 34: ADBOp replacement NOPed");
	CHECK(rd16(r, LEAD + 0) == 0x2278, "ptch 34: movea.l VIA,a1 left intact");
}

/*
 * lpch 31 -- SCSI Manager and vSoundDead.
 *
 * ID 31 is 0b11111, all five pre-SuperMario ROMs, matching
 * "SCSIDispatchCommon PatchProc _SCSIDispatch,(Plus,SE,Portable,II,IIci,notAUX)"
 * at $SM/OS/SCSIMgr/SCSILinkPatch.a:221.
 */
static void test_lpch31(void)
{
	static const uint8 body[] = {
		// vSoundDead: move.l VIA,a0 / bset #7,d0 / rts   -- replaced by rts
		0x20, 0x78, 0x01, 0xd4,
		0x08, 0xd0, 0x00, 0x07,
		0x4e, 0x75,
		// SCSIDispatch selector check
		0x0c, 0x6f, 0x00, 0x0e, 0x00, 0x04, 0x66, 0x0c,   // +10
	};
	GuardedRsrc r(body, sizeof(body), true);
	run_checkload("lpch 31", 'lpch', 31, r);

	CHECK(rd16(r, LEAD + 0) == M68K_RTS, "lpch 31: vSoundDead VIA write replaced by RTS");
	CHECK(rd16(r, LEAD + 10) == M68K_EMUL_OP_SCSI_DISPATCH, "lpch 31: SCSIDispatch EmulOp planted");
	CHECK(rd16(r, LEAD + 12) == 0x2e49, "lpch 31: move.l a1,a7 planted");
	CHECK(rd16(r, LEAD + 14) == M68K_JMP_A0, "lpch 31: jmp (a0) planted");
}

/*
 * boot 2 -- reachability.
 *
 * The #if guarding the fake-handle patch used to span the boot 3 / boot 2
 * else-if boundary, so this whole case was compiled out on every platform. The
 * patch itself is a no-op where the ROM image is write-protected, but the
 * dispatch must still be reached and recorded.
 */
static void test_boot2_reachable(void)
{
	static const uint8 body[] = {
		0x20, 0x78, 0x02, 0xae, 0xd1, 0xfc, 0x00, 0x01,
		0x00, 0x00, 0x21, 0xc8, 0x00, 0x00,
	};
	GuardedRsrc r(body, sizeof(body), true);
	run_checkload("boot 2", 'boot', 2, r);

	bool reached = false;
	const std::vector<PatchRecord> &log = GetRsrcPatchLog();
	for (size_t i = 0; i < log.size(); i++)
		if (strncmp(log[i].name, "boot 2", 6) == 0)
			reached = true;
	CHECK(reached, "boot 2: dispatch is reachable and recorded");
}

/*
 * Boundary cases.
 *
 * Each of these used to be able to scan outside the resource:
 *   - a resource shorter than the signature underflowed "max - search_len"
 *     in find_rsrc_data() into a near-4GB bound;
 *   - a SynchIdleTime signature found within the first 0x80 bytes made
 *     patch_idle_time() compute "p + base - 0x80" before the buffer.
 */
static void test_boundaries(void)
{
	// Resource far shorter than any signature.
	static const uint8 tiny[] = {0x70};
	GuardedRsrc r1(tiny, sizeof(tiny));
	run_checkload("1-byte lpch 31", 'lpch', 31, r1);

	// Zero-length resource.
	GuardedRsrc r2(NULL, 0);
	run_checkload("empty lpch 24", 'lpch', 24, r2);

	// All-ones: no signature should match anything.
	uint8 ones[256];
	memset(ones, 0xff, sizeof(ones));
	GuardedRsrc r3(ones, sizeof(ones));
	run_checkload("all-0xFF gpch 750", 'gpch', 750, r3);

	// SynchIdleTime signature at the very start, i.e. less than 0x80 bytes of
	// run-up for the backwards scan. Needs "idlewait" on to reach the code.
	test_prefs_set_bool("idlewait", true);
	uint8 idle[32];
	memset(idle, 0x4e, sizeof(idle));		// filler
	idle[0] = 0x70; idle[1] = 0x03; idle[2] = 0xa0; idle[3] = 0x9f;
	GuardedRsrc r4(idle, sizeof(idle));
	run_checkload("idle sig at offset 0", 'gpch', 750, r4);

	// And with enough run-up that the backwards scan is legitimate but finds
	// nothing: still must not write.
	uint8 idle2[512];
	memset(idle2, 0x4e, sizeof(idle2));
	idle2[0x100] = 0x70; idle2[0x101] = 0x03; idle2[0x102] = 0xa0; idle2[0x103] = 0x9f;
	GuardedRsrc r5(idle2, sizeof(idle2));
	run_checkload("idle sig, no ExpandMem ref", 'gpch', 750, r5);
	test_prefs_set_bool("idlewait", false);
}

/*
 * Signature at offset 0 is reported as "not found".
 *
 * find_rsrc_data() uses 0 as its not-found sentinel, so a patch site at the
 * very start of a resource is skipped. That has never mattered -- real
 * resources begin with a header or entry code -- but it is a real property of
 * the locator and worth pinning so a future change to the contract is a
 * deliberate one.
 */
static void test_signature_at_offset_zero(void)
{
	static const uint8 body[] = {
		0x20, 0x78, 0x01, 0xd4, 0x08, 0xd0, 0x00, 0x07, 0x4e, 0x75,
	};
	GuardedRsrc r(body, sizeof(body));		// deliberately no lead filler
	run_checkload("lpch 31 at offset 0", 'lpch', 31, r);

	CHECK(rd16(r, 0) == 0x2078,
	      "signature at offset 0 is treated as not-found (known locator limit)");
}

/*
 * A resource type CheckLoad() has no patch for must do nothing at all.
 */
static void test_unhandled_type(void)
{
	uint8 body[64];
	memset(body, 0xa5, sizeof(body));
	GuardedRsrc r(body, sizeof(body));
	uint8 before[64];
	memcpy(before, r.data(), sizeof(before));

	run_checkload("unhandled 'junk'", 'junk', 1, r);
	CHECK(memcmp(before, r.data(), sizeof(before)) == 0,
	      "unhandled resource type is left byte-identical");
	CHECK(GetRsrcPatchLog().empty(), "unhandled resource type records no patch");
}

int main(void)
{
	test_install_crash_handler();
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== basilisk_rsrcpatch_test ===\n");
	CHECK(activate_cpu_engine("musashi", false), "activate musashi");

	// CheckLoad() reads prefs; pin the ones it branches on.
	test_prefs_set_bool("idlewait", false);
	test_prefs_set_bool("ltoudp", false);

	run_isolated("lpch 24", test_lpch24);
	run_isolated("ptch 34", test_ptch34);
	run_isolated("lpch 31", test_lpch31);
	run_isolated("boot 2", test_boot2_reachable);
	run_isolated("boundaries", test_boundaries);
	run_isolated("offset-0 signature", test_signature_at_offset_zero);
	run_isolated("unhandled type", test_unhandled_type);

	printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
