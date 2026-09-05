/*
 * basilisk_stubabi_test.cpp - Execute the stubs PatchROM() plants and check
 *                             their register contracts (T4)
 *
 * The golden manifest (basilisk_patches_test) proves a patch landed at the
 * offset we expect. It says nothing about whether the bytes we wrote there
 * behave the way the ROM's callers require. That gap is not theoretical: commit
 * 23e7721 replaced the Microseconds stub with one that wrote an UnsignedWide
 * through A0, which is a perfectly well-formed patch at the right offset, and
 * it bombed the 32-bit Quadra boot with a Type 10 at 0x65AAx.
 *
 * So this suite runs each planted stub on the 680x0 core and asserts what the
 * Mac OS ROM sources say the caller may assume:
 *
 *   Microseconds  $SM/OS/TimeMgr/TimeMgr.a:736-751 -- A0 = high, D0 = low,
 *                 "a0-a2/d1-d2 saved by dispatcher"
 *   InsTime/RmvTime/PrimeTime
 *                 $SM/OS/TimeMgr/TimeMgr.a; the "move sr,-(sp) / ori #$0700,sr"
 *                 wrapper must leave the caller's interrupt mask as it found it
 *   BlockMove     $SM/OS/MemoryMgr/BlockMove.a -- copies D0 bytes A0 -> A1,
 *                 handles overlap, returns noErr in D0
 *   SCSIDispatch  the hand-emulated "rtd" in emul_op.cpp must remove the
 *                 selector and the arguments and leave the result in the
 *                 caller's Pascal result slot
 *
 * Stub offsets come from GetPatchLog() rather than being hardcoded, so this
 * suite and the manifest cannot drift apart.
 *
 * This is also the tier Phase 3 rests on: moving a trap from a ROM byte-patch
 * to _SetTrapAddress must not change its register contract, and short of a full
 * boot this is the only thing that says so.
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
#include "main.h"

/* Scratch RAM, clear of the 0x10000 supervisor stack Execute68k runs on. */
#define CODE_BASE 0x20000
#define DATA_BASE 0x28000

/*
 * Loads dist/Quadra800.rom and runs a full patch pass.
 *
 * Returns:
 *   true if the ROM was available and patched; callers skip otherwise.
 */
static bool setup_patched_rom(void)
{
	if (test_load_quadra_rom(NULL) < 16) {
		printf("  [SKIP] needs dist/Quadra800.rom\n");
		return false;
	}
	test_prefs_set_bool("ltoudp", false);
	test_prefs_set_int32("modelid", 29);	// Quadra 800
	if (!CheckROM()) {
		CHECK(false, "CheckROM() accepts the test ROM");
		return false;
	}
	if (!PatchROM()) {
		CHECK(false, "PatchROM() succeeds on the test ROM");
		return false;
	}
	return true;
}

/*
 * Looks a patch up in the log by the name PatchROM() recorded it under.
 *
 * Arguments:
 *   name: manifest name, e.g. "Microseconds ($A093)".
 *
 * Returns:
 *   Macintosh address of the patch site, or 0 if the patch is absent or missed
 *   (both of which the manifest test already reports, so callers just skip).
 */
static uint32 stub_addr(const char *name)
{
	const std::vector<PatchRecord> &log = GetPatchLog();
	for (size_t i = 0; i < log.size(); i++)
		if (strcmp(log[i].name, name) == 0 && log[i].applied)
			return ROMBaseMac + log[i].offset;
	return 0;
}

/* Cursor for the little 680x0 assembler below. */
static uint32 emit_pc;

static void emit_begin(uint32 addr) { emit_pc = addr; }
static void emit(uint16 op) { WriteMacInt16(emit_pc, op); emit_pc += 2; }
static void emit32(uint32 val) { WriteMacInt32(emit_pc, val); emit_pc += 4; }

/* jsr (addr).L */
static void emit_jsr(uint32 addr) { emit(0x4eb9); emit32(addr); }

/*
 *  Microseconds ($A093): A0 = high, D0 = low
 *
 *  $SM/OS/TimeMgr/TimeMgr.a:736-751 states the contract:
 *
 *      ;  Routine:     MicroSeconds
 *      ;  Outputs:     A0/D0 - 64 bit counter (A0=High, D0=Low)
 *      __MicroSeconds: proc  export   ; a0-a2/d1-d2 saved by dispatcher
 *
 *  The test_stubs.cpp Microseconds() returns a fixed 0:1000 so both halves are
 *  checkable. The A0-is-not-a-pointer assertion is the direct regression test
 *  for 23e7721 / fa6deb0 -- see docs/quadra-32bit-boot-crashes.md.
 */
static void test_microseconds_abi(void)
{
	if (!setup_patched_rom())
		return;
	uint32 stub = stub_addr("Microseconds ($A093)");
	if (!stub) {
		printf("  [SKIP] Microseconds stub not in the patch log\n");
		return;
	}

	// A plausible UnsignedWide* for A0: if the stub writes through it, we see it.
	const uint32 wide = DATA_BASE;
	WriteMacInt32(wide, 0xdeadbeef);
	WriteMacInt32(wide + 4, 0xdeadbeef);

	M68kRegisters r;
	memset(&r, 0, sizeof(r));
	r.a[0] = wide;
	r.a[1] = 0xa1a1a1a1;
	r.a[2] = 0xa2a2a2a2;
	r.d[0] = 0xd0d0d0d0;
	r.d[1] = 0xd1d1d1d1;
	r.d[2] = 0xd2d2d2d2;
	Execute68k(stub, &r);

	CHECK(r.a[0] == 0, "Microseconds: A0 = high word (TimeMgr.a:741)");
	CHECK(r.d[0] == 1000, "Microseconds: D0 = low word (TimeMgr.a:741)");
	CHECK(ReadMacInt32(wide) == 0xdeadbeef && ReadMacInt32(wide + 4) == 0xdeadbeef,
	      "Microseconds: does not write an UnsignedWide through A0 (regression: 23e7721)");
	CHECK(r.a[1] == 0xa1a1a1a1 && r.a[2] == 0xa2a2a2a2,
	      "Microseconds: A1/A2 untouched (TimeMgr.a:741 'a0-a2 saved by dispatcher')");
	CHECK(r.d[1] == 0xd1d1d1d1 && r.d[2] == 0xd2d2d2d2,
	      "Microseconds: D1/D2 untouched (TimeMgr.a:741 'd1-d2 saved by dispatcher')");
}

/*
 *  The Time Manager stubs and their interrupt-mask wrapper
 *
 *  RmvTime and PrimeTime run at IPL 7 so the Time Manager queue cannot be
 *  re-entered from the 60 Hz interrupt while the host is walking it -- the same
 *  reason Apple's own Time Manager swap raises the mask
 *  ($SM/OS/TimeMgr/TimeMgrPatch.a:186-187). The wrapper is only correct if it
 *  puts the caller's mask back, which is what the execution check below proves:
 *  the trampoline sets IPL 0, calls the stub, and reads SR back.
 *
 *  InsTime deliberately has no wrapper (it only links a task record), so it is
 *  checked for result propagation alone.
 */
static void test_time_mgr_stubs(void)
{
	if (!setup_patched_rom())
		return;

	static const struct { const char *name; bool wrapped; } stubs[] = {
		{ "InsTime ($A058)",   false },
		{ "RmvTime ($A059)",   true  },
		{ "PrimeTime ($A05A)", true  },
	};

	for (size_t i = 0; i < sizeof(stubs) / sizeof(stubs[0]); i++) {
		uint32 stub = stub_addr(stubs[i].name);
		char msg[192];
		if (!stub) {
			snprintf(msg, sizeof(msg), "%s: in the patch log", stubs[i].name);
			CHECK(false, msg);
			continue;
		}

		// Static shape: the wrapper must be the exact six words we planted.
		if (stubs[i].wrapped) {
			bool shape = ReadMacInt16(stub + 0) == 0x40e7 &&		// move  sr,-(sp)
			             ReadMacInt16(stub + 2) == 0x007c &&		// ori   #$0700,sr
			             ReadMacInt16(stub + 4) == 0x0700 &&
			             ReadMacInt16(stub + 8) == 0x46df &&		// move  (sp)+,sr
			             ReadMacInt16(stub + 10) == M68K_RTS;
			snprintf(msg, sizeof(msg),
			         "%s: sr save / ori #$0700,sr / sr restore wrapper intact "
			         "($SM/OS/TimeMgr/TimeMgrPatch.a:186)", stubs[i].name);
			CHECK(shape, msg);
		}

		/*
		 *   move.w  #$2000,sr    supervisor, interrupt mask 0
		 *   jsr     (stub).L
		 *   move.w  sr,d7        what the stub left the mask at
		 *   rts
		 */
		emit_begin(CODE_BASE);
		emit(0x46fc); emit(0x2000);
		emit_jsr(stub);
		emit(0x40c7);
		emit(M68K_RTS);

		M68kRegisters r;
		memset(&r, 0, sizeof(r));
		r.d[0] = 0x33333333;	// so "result propagated" is distinguishable
		r.d[7] = 0xffffffff;
		Execute68k(CODE_BASE, &r);

		snprintf(msg, sizeof(msg), "%s: returns noErr in D0", stubs[i].name);
		CHECK(r.d[0] == 0, msg);

		snprintf(msg, sizeof(msg),
		         "%s: caller's interrupt mask restored (SR=%04x)",
		         stubs[i].name, (unsigned)(r.d[7] & 0xffff));
		CHECK((r.d[7] & 0x0700) == 0, msg);
	}
}

/*
 *  BlockMove ($A02E): copy D0 bytes from A0 to A1, noErr in D0
 *
 *  $SM/OS/MemoryMgr/BlockMove.a. The overlap case is checked because callers
 *  rely on it. Be clear about what it does and does not catch: swapping the
 *  memmove() in emul_op.cpp for memcpy() does *not* fail it, because Apple's
 *  libc memcpy handles these overlaps anyway. It does catch the realistic
 *  regression -- a hand-rolled forward byte loop, which smears the first byte
 *  across the destination.
 */
static void test_blockmove_stub(void)
{
	if (!setup_patched_rom())
		return;
	uint32 stub = stub_addr("BlockMove ($A02E)");
	if (!stub) {
		CHECK(false, "BlockMove ($A02E): in the patch log");
		return;
	}

	const uint32 src = DATA_BASE;
	const uint32 dst = DATA_BASE + 0x100;
	const uint32 len = 64;
	for (uint32 i = 0; i < len; i++)
		WriteMacInt8(src + i, (uint8)(i * 7 + 1));
	for (uint32 i = 0; i < len; i++)
		WriteMacInt8(dst + i, 0);

	M68kRegisters r;
	memset(&r, 0, sizeof(r));
	r.a[0] = src;
	r.a[1] = dst;
	r.d[0] = len;
	Execute68k(stub, &r);

	bool copied = true;
	for (uint32 i = 0; i < len; i++)
		if (ReadMacInt8(dst + i) != (uint8)(i * 7 + 1))
			copied = false;
	CHECK(copied, "BlockMove: D0 bytes copied A0 -> A1 ($SM/OS/MemoryMgr/BlockMove.a)");
	CHECK(r.d[0] == 0, "BlockMove: returns noErr in D0");

	// Overlapping by one byte, destination above source: a forward byte loop
	// smears byte 0 across the whole destination.
	const uint32 ov = DATA_BASE + 0x200;
	for (uint32 i = 0; i < len; i++)
		WriteMacInt8(ov + i, (uint8)(i + 1));
	memset(&r, 0, sizeof(r));
	r.a[0] = ov;
	r.a[1] = ov + 1;
	r.d[0] = len - 1;
	Execute68k(stub, &r);

	bool overlapped = true;
	for (uint32 i = 0; i < len - 1; i++)
		if (ReadMacInt8(ov + 1 + i) != (uint8)(i + 1))
			overlapped = false;
	CHECK(overlapped, "BlockMove: overlapping move keeps memmove semantics");
}

/*
 *  Calls SCSIDispatch through its ROM stub with a Pascal-style argument frame.
 *
 *  The trap's callers push a result slot, then the arguments, then the
 *  selector, and expect a "rtd" that removes selector and arguments but leaves
 *  the result slot. emul_op.cpp emulates that by hand
 *  (M68K_EMUL_OP_SCSI_DISPATCH: a0 = return address, a1 = new stack pointer)
 *  and the stub finishes it with "move.l a1,a7 / jmp (a0)". Getting the
 *  adjustment wrong by two bytes desynchronises the caller's stack, which is
 *  exactly the kind of fault that boots fine and bombs later.
 *
 *  Arguments:
 *    stub:     Macintosh address of the patched trap.
 *    selector: SCSIDispatch selector.
 *    arg:      one word argument, pushed below the selector.
 *    has_arg:  false for the no-argument selectors.
 *    result:   receives the word left in the caller's result slot.
 *
 *  Returns:
 *    true if the stack pointer came back exactly where the caller left it.
 */
static bool call_scsi(uint32 stub, uint16 selector, uint16 arg, bool has_arg, uint16 *result)
{
	/*
	 *   movea.l a7,a3          remember the caller's stack pointer
	 *   clr.w   -(sp)          Pascal result slot
	 *  [move.w  #arg,-(sp)]    argument, if this selector takes one
	 *   move.w  #sel,-(sp)     selector
	 *   jsr     (stub).L
	 *   move.w  (sp)+,d4       the result the rtd emulation left behind
	 *   movea.l a7,a4          stack pointer afterwards
	 *   rts
	 */
	emit_begin(CODE_BASE);
	emit(0x264f);
	emit(0x4267);
	if (has_arg) {
		emit(0x3f3c); emit(arg);
	}
	emit(0x3f3c); emit(selector);
	emit_jsr(stub);
	emit(0x381f);
	emit(0x284f);
	emit(M68K_RTS);

	M68kRegisters r;
	memset(&r, 0, sizeof(r));
	Execute68k(CODE_BASE, &r);
	*result = (uint16)r.d[4];
	return r.a[3] == r.a[4];
}

static void test_scsi_dispatch_stub(void)
{
	if (!setup_patched_rom())
		return;
	uint32 stub = stub_addr("SCSIDispatch ($A815)");
	if (!stub) {
		CHECK(false, "SCSIDispatch ($A815): in the patch log");
		return;
	}

	uint16 res = 0xffff;

	// 0 = SCSIReset: no arguments, resets the bus to PH_FREE.
	CHECK(call_scsi(stub, 0, 0, false, &res), "SCSIReset: stack pointer restored (rtd, 6 bytes)");
	CHECK(res == 0, "SCSIReset: noErr in the caller's result slot");

	// 1 = SCSIGet: arbitrates, so the following calls have observable state.
	CHECK(call_scsi(stub, 1, 0, false, &res), "SCSIGet: stack pointer restored");
	CHECK(res == 0, "SCSIGet: noErr from PH_FREE");

	// 10 = SCSIStat: 0x0040 is "bus busy", set by SCSIGet above.
	CHECK(call_scsi(stub, 10, 0, false, &res), "SCSIStat: stack pointer restored");
	CHECK(res == 0x0040, "SCSIStat: reports bus busy after SCSIGet");

	// 14 = SCSIMgrBusy: true while the phase is not PH_FREE.
	CHECK(call_scsi(stub, 14, 0, false, &res), "SCSIMgrBusy: stack pointer restored");
	CHECK(res == 1, "SCSIMgrBusy: busy while arbitrated");

	/*
	 * 2 = SCSISelect takes a word argument, so this is the case that actually
	 * exercises the variable part of the rtd adjustment (6 + 2 bytes). No
	 * targets are configured in the test harness, so the selection times out
	 * with scCommErr (2, BasiliskII/scsi.cpp:113).
	 */
	CHECK(call_scsi(stub, 2, 6, true, &res), "SCSISelect: stack pointer restored (rtd, 6 + 2 bytes)");
	CHECK(res == 2, "SCSISelect: scCommErr for an absent target");

	// ...and the phase went back to PH_FREE, proving the call really ran.
	CHECK(call_scsi(stub, 14, 0, false, &res), "SCSIMgrBusy: stack pointer restored");
	CHECK(res == 0, "SCSIMgrBusy: idle again after the failed selection");
}

/*
 *  The .Sony driver header must still point at the four EmulOps
 *
 *  rom_patches.cpp memcpy()s sony_driver[] over the ROM's DRVR 4 and then
 *  writes several other things into the same region (icons at +0x400..+0x800,
 *  the CheckLoad trampoline at +0x300, the PutScrap patch at +0xc00). A
 *  mis-sized or mis-placed one of those lands on the driver itself, so check
 *  the header offsets resolve to the opcodes they are supposed to.
 *
 *  Header layout and entry points: $SM/Drivers/Sony/Sony.a (DiskOpen :253,
 *  DiskPrime :199, CtlTbl :495).
 */
static void test_sony_driver_entries(void)
{
	if (!setup_patched_rom())
		return;
	uint32 sony = stub_addr(".Sony driver (DRVR 4)");
	if (!sony) {
		CHECK(false, ".Sony driver (DRVR 4): in the patch log");
		return;
	}

	static const struct { uint32 hdr; uint16 want_off; uint16 op; const char *what; } entries[] = {
		{ 8,  0x18, M68K_EMUL_OP_SONY_OPEN,    "Open"    },
		{ 10, 0x1c, M68K_EMUL_OP_SONY_PRIME,   "Prime"   },
		{ 12, 0x20, M68K_EMUL_OP_SONY_CONTROL, "Control" },
		{ 14, 0x2c, M68K_EMUL_OP_SONY_STATUS,  "Status"  },
	};

	for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		char msg[192];
		uint16 off = ReadMacInt16(sony + entries[i].hdr);
		snprintf(msg, sizeof(msg), ".Sony %s offset is $%02x", entries[i].what, entries[i].want_off);
		CHECK(off == entries[i].want_off, msg);
		snprintf(msg, sizeof(msg), ".Sony %s entry holds its EmulOp", entries[i].what);
		CHECK(ReadMacInt16(sony + off) == entries[i].op, msg);
	}

	CHECK(ReadMacInt8(sony + 18) == 5 && ReadMacInt8(sony + 19) == '.' &&
	      ReadMacInt8(sony + 20) == 'S',
	      ".Sony driver name still reads \"\\p.Sony\"");
}

/*
 *  The CheckLoad trampoline calls through jCheckLoad ($07F0)
 *
 *  rom_patches.cpp redirects vCheckLoad to a stub squatting in the .Sony
 *  resource. The stub saves the resource type, calls the ROM's own jCheckLoad
 *  vector, restores the type and then enters EMUL_OP_CHECKLOAD. Phase 3a
 *  replaces the byte-patch that reaches this stub with an install into $07F0
 *  itself ($SM/Patches/BeforePatches.a:690-697), and this assertion is what
 *  pins the behaviour that must survive that change.
 */
static void test_checkload_trampoline(void)
{
	if (!setup_patched_rom())
		return;
	uint32 sony = stub_addr(".Sony driver (DRVR 4)");
	uint32 hook = stub_addr("vCheckLoad hook");
	if (!sony || !hook) {
		CHECK(false, "CheckLoad trampoline: .Sony and vCheckLoad hook in the patch log");
		return;
	}

	// The ROM hook is "jmp (trampoline).L".
	uint32 tramp = sony + 0x300;
	CHECK(ReadMacInt16(hook) == M68K_JMP && ReadMacInt32(hook + 2) == tramp,
	      "vCheckLoad hook jumps to the CheckLoad trampoline");

	CHECK(ReadMacInt16(tramp + 0) == 0x2f03, "CheckLoad trampoline saves the resource type (move.l d3,-(sp))");
	CHECK(ReadMacInt16(tramp + 2) == 0x2078 && ReadMacInt16(tramp + 4) == 0x07f0,
	      "CheckLoad trampoline loads jCheckLoad ($07F0) ($SM/Interfaces/AIncludes/Private.a:386)");
	CHECK(ReadMacInt16(tramp + 6) == M68K_JSR_A0,
	      "CheckLoad trampoline calls the ROM's own vCheckLoad first");
	CHECK(ReadMacInt16(tramp + 8) == 0x221f, "CheckLoad trampoline restores the type into D1");
	CHECK(ReadMacInt16(tramp + 10) == M68K_EMUL_OP_CHECKLOAD, "CheckLoad trampoline enters EMUL_OP_CHECKLOAD");
	CHECK(ReadMacInt16(tramp + 12) == M68K_RTS, "CheckLoad trampoline returns");
}

int main(void)
{
	test_install_crash_handler();
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== basilisk_stubabi_test ===\n");
	CHECK(activate_cpu_engine("musashi", false), "activate musashi");

	run_isolated("Microseconds ABI", test_microseconds_abi);
	run_isolated("Time Manager stubs", test_time_mgr_stubs);
	run_isolated("BlockMove stub", test_blockmove_stub);
	run_isolated("SCSIDispatch stub", test_scsi_dispatch_stub);
	run_isolated(".Sony driver entries", test_sony_driver_entries);
	run_isolated("CheckLoad trampoline", test_checkload_trampoline);

	printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
