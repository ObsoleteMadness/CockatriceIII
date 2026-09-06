# JIT / Memory Model Investigation

This documents two related debugging passes across the swappable CPU
engines (Musashi, UAE/Amiberry, Emu68, syn68k): the shared memory-model
crash all three JIT-ish engines hit on boot, and the follow-up isolation
test for the specific "opcode 0x773F not implemented" symptom seen on
Emu68.

---

## 1. Shared memory-model bug (UAE/Amiberry JIT crash, Emu68 stuck loop)

### Symptom

- **UAE/Amiberry JIT**: segfaulted inside `compile_block()` within seconds
  of boot, with a wild pointer in a callee-saved register.
- **Emu68**: looped forever printing `[MEM] emu68 guest hole at 0xFFFFFFFC
  (PC=0x0000FC00)`.
- **syn68k**: never reached `VIDEO_OPEN`; different symptom, see §3.

### Root cause

`BasiliskII/memory.cpp` reserved the 4GB guest window as `PROT_NONE`
everywhere except RAM/ROM/framebuffer. Any other access raised SIGSEGV,
which `memory_guard_enter()` / `MEMORY_FAULT_SETJMP` converted into a
`siglongjmp()` back into the CPU run loop, which then injected 680x0
vector 2.

That's safe for Musashi's one-instruction-at-a-time interpreter, but both
JIT compilers mutate global, non-reentrant state while translating a
block:

- UAE/Amiberry's `compile_block()` (`BasiliskII/amiberry/src/jit/arm/compemu_support_arm.cpp`)
  advances file-static cursors (`current_compile_p`, `comp_pc_p`) and
  register-allocation tables incrementally.
- Emu68's `M68K_GetTranslationUnit()` owns a similar bump-allocated
  ARM64 code-cache cursor.

A `siglongjmp` that fires mid-translation abandons that state half
written. The next call to the same compiler resumes writing on top of the
stale cursor instead of discarding the half-built block, corrupting the
JIT code cache. That is what produced the wild-pointer crash in
`compile_block` and (via a related but not identical path) Emu68's
refaulting loop.

Real NuBus hardware-probe bus errors, which would otherwise be the reason
to keep genuine holes, are already routed around by ROM patches
(`InstallSlotROM()` / `patch_rom_32()`, see
`docs/basilisk-ii-boot-and-patch.md` §9) — nothing in the normal boot path
depends on an unmapped address raising a real bus error.

### Fix

`memory_init()` now commits the entire 4GB window as RW dummy-backed
memory up front (demand-paged, so it costs no real RSS until touched)
instead of leaving non-RAM/ROM/framebuffer addresses `PROT_NONE`. RAM,
ROM, and the framebuffer are still tracked as their own committed ranges
for `memory_is_mapped()` bookkeeping. The guard/longjmp/vector-2 path is
unchanged and still exists as a safety net for addresses that fall
entirely outside the 4GB window — it just no longer fires for ordinary
holes.

### Verified

Built the real app and ran it against the maintainer's own
`BasiliskII/OSX64/CockatriceIII_Prefs` (real Quadra 800 ROM + real SCSI
disk image), one engine at a time:

- **UAE/Amiberry JIT**: no crash, zero `guest hole` messages over 15s+
  (previously crashed in ~2s).
- **Emu68**: zero `guest hole` messages (previously infinite). Left
  executing — see §2, the opcode-decode loop is a separate bug.
- **syn68k**: no crash/hole, but a different hang — see §3.

All 266 pre-existing unit tests in `BasiliskII/Musashi/test_integration.cpp`
still pass; three of them were updated to assert the new dummy-backed
invariant instead of the old PROT_NONE-hole invariant (§4 has the general
"what changed in the tests" note).

---

## 2. Emu68 "opcode 0x773F not implemented" — isolated, and it's fine

### What 0x773F actually is

`0x773F` is in 68k "line 7" (`0111 ddd0 dddddddd` = MOVEQ). Bit 8 must be
clear for MOVEQ; `0x773F` has it set (`0111 0111 0011 1111`), which is a
**permanently reserved/illegal encoding on every 680x0**, not a
generation-dependent gap. Real hardware takes Vector 4 (Illegal
Instruction) on it. Emu68's `EMIT_moveq()`
(`BasiliskII/Emu68/upstream/src/M68k_MOVE.c:24-39`) already checks
`opcode & 0x100` and calls `EMIT_Exception(ctx, VECTOR_ILLEGAL_INSTRUCTION, 0)`
for exactly this case.

### The question

Does that Vector 4 path actually work standalone, or was the live-boot
loop caused by the illegal-instruction handler itself failing to
redirect control (e.g. leaving the JIT mid-translation the way §1
describes, or never actually reaching the guest's Vector 4 handler)?

### New test: `test_illegal_opcode_exception()`

Added to `BasiliskII/Musashi/test_integration.cpp`, run for every engine
except syn68k (see below), right after `test_rom_boot_after_reset()`. It
pokes `0x773F` directly at a scratch address — independent of whatever
boot-sequence state produced it live — installs a Vector 4 handler, and
checks two things:

1. The pushed fault PC equals the *faulting* instruction's own address
   (not the next one) — Illegal Instruction is not a "resume after"
   exception like TRAP/TRAPV/DIVS (which `test_exception_traps()`
   already covered).
2. The handler runs and returns cleanly via RTE.

### Result: it passes on every engine, including Emu68

Musashi, UAE (interpreter), UAE+JIT, UAE+JIT+JITFPU, and Emu68 all
correctly vector to the handler and return. **The illegal-instruction
exception path is not the bug.** The live-boot loop must be caused by
something upstream landing real PC at `0x0000FC00`/`0x0000ffd0` in the
first place — a wrong jump/branch target somewhere earlier in the real
ROM's boot sequence under Emu68 specifically (the synthetic
`test_rom_boot_after_reset()` RESET/4EFA/JMP sequence already passes for
Emu68, so the bug is further along than that). **Not yet found — this is
the open follow-up**, and it's a different, narrower search than "does
exception handling work" now that this test has answered that question.

### Gap this test caught in itself, worth recording

The first version of this test crashed the whole suite — not because of
an emulator bug, but because of a bug in the *test*: the Vector 4 handler
tried to skip the illegal opcode with `ADDQ.W #2,(2,A7)`. The pushed PC
is a 32-bit big-endian field at `(2,A7)` (format-0 frame: `SR` at
`(0,A7)`, `PC` at `(2,A7)`, format/vector word at `(6,A7)`); a **word**-sized
add at that address only touches the PC's *high* 16 bits, silently
corrupting it instead of incrementing it. RTE then resumed at garbage,
which (thanks to the new all-RAM-reads-as-zero memory model from §1)
didn't fault — it just executed `ORI.B #0,D0` in a straight line through
zeroed memory for up to ~4GB until it ran off the end of the reserved
host window and crashed for real. Fixed by using `ADDQ.L #2,(2,A7)`
instead, which operates on the full 32-bit field.

This is worth keeping in mind for any other hand-assembled exception
handler added to this test file: **fix up a stacked PC with a long-sized
op, never a word-sized one.**

---

## 2b. Found it: `JMP (d8,PC,Xn.L)` drops the PC base in a large superblock

Root-caused the live-boot loop with a build note first: **the OSX64 app
does not compile Emu68's translator from `BasiliskII/Emu68/upstream/src/`
directly.** `OSX64/Makefile`'s `$(EMU68_GEN)/%.c: $(EMU68_UPSTREAM)/src/%.c`
rule runs every upstream `.c` file through
`BasiliskII/Emu68/hosted/prep_translator.py` into `obj/emu68_gen/` first
(mechanical Darwin/hosted fixups — GNU alias trampolines, WFI/WFE hangs,
little-endian `movz`/`movk` halfword order for host function pointers,
etc. — see the script's own docstring). `BasiliskII/Musashi/emu68_gen/`
is a *separate*, hand-maintained snapshot used only by
`test_integration`'s Makefile target — it is not regenerated and had
already drifted from upstream by exactly one of those fixups. Any Emu68
codegen change belongs in `Emu68/upstream/src/`, written in upstream's
existing convention (`u.u16[3]` first for a 64-bit pointer split); the
prep script's blind text substitution flips it to the correct
little-endian order when it copies into `obj/emu68_gen/`. Editing the
generated copy directly is a dead end — the next `make` regenerates it
from upstream and silently discards the edit.

With that sorted out, instrumented `EMIT_JMP()`
(`BasiliskII/Emu68/upstream/src/M68k_LINE4.c`) to call a temporary C
helper with the runtime-computed jump target, gated to the exact
addressing mode at fault (`opcode & 0x3f == 073`, i.e. `(d8,PC,Xn)`).
Real boot, live trace:

```
0x408000EC: lea     $b190.l, A5
0x408000F2: jmp     (-$8,PC,A5.l)
[Emu68 DEBUG] indexed-PC JMP computed target = 0x0000B1C2
```

Correct target per 68k semantics is `(address of extension word) + (-8) +
A5` = `0x408000F4 - 8 + 0x0000B190` = `0x4080B27C` — well inside ROM. The
JIT instead produced `0x0000B1C2`, which is just `A5 + 0x32`: **the PC
component of the sum is missing.** `0x0000B190`/`0x0000B1C2`/`0x0000FFD0`/
`0x0000FC00` — every wrong address seen across every prior run — are all
small values in this same shape, consistent with the same root cause:
by the time this instruction executes, `REG_PC` (ARM `w18`, see
`BasiliskII/Emu68/upstream/include/M68k.h:457`) is not holding the block's
true absolute m68k address, just a small residual.

This is a real, load-bearing bug, not a re-run of §1 — it reproduces
identically whether the memory model faults or not; §1 only changed what
happens *after* PC goes wrong (silent zero-fill execution instead of an
immediate SIGSEGV/guest-hole).

**Ruled out:** the ARM64 GPR allocator (`RA_AllocARMRegister()` in
`BasiliskII/Emu68/upstream/src/aarch64/RegisterAllocator64.c`) only ever
hands out `x0`-`x11`; `REG_PC` (18) and the `REG_A0`-`REG_A7` (20-27)
constants are fixed, dedicated GPRs it can never allocate over, so this
isn't a temp-register clobbering `w18`.

**Confirmed correct in isolation:** the compile-time C logic in both
`load_reg_from_addr_offset()` (used by the earlier, working `(d16,PC)`
JMP at ROM+0x2A) and `load_reg_from_addr()` (used by this `(d8,PC,Xn)`
JMP) both handle `size==0` as "compute the address, don't dereference
memory" — correctly, on paper, in `BasiliskII/Emu68/upstream/src/M68k_EA.c`.

**Leading suspect, not yet confirmed:** the block containing this JMP is
a 23-m68k-instruction / 132-ARM-instruction superblock (two `MOVEM.L`
saves, two PC-relative `LEA`s, this indexed `JMP`) — much longer than the
two single-instruction blocks that precede it and that *do* land
correctly. `w18` is advanced incrementally through the block via
`EMIT_AdvancePC()`'s `_pc_rel` delta accumulator
(`BasiliskII/Emu68/upstream/src/M68k_Translator.c`), flushed to the real
GPR at various points via `EMIT_GetOffsetPC()`/`EMIT_FlushPC()`. The next
step is tracing whether `_pc_rel` (or the block-entry seed of `w18`)
survives correctly across the two `MOVEM.L`/`LEA` instructions earlier in
*this specific* block — not yet done.

The temporary instrumentation (the debug helper in `emu68_glue.cpp`, the
gated `EMIT()` block in `EMIT_JMP()`, the widened word dump in
`emu68_dump_jit_unit()`) has been removed; nothing from this section is
left in the tree. 276 tests still pass.

---

## 3. syn68k — excluded from the new test, separate bug

Running the live app against the same real ROM/prefs with
`cpu_emulator syn68k`:

```
[CPU-ILLEGAL] syn68k: Forwarding illegal opcode 0x0000 [0x0000 0x0000] at PC=0x5A436CBF to guest Vector 4 at 0x408026F4 ...
```

`test_illegal_opcode_exception()` reproduces the same shape of failure in
isolation: syn68k forwards a *different* opcode (`0x0000`) from a PC
nowhere near `code_addr`, then hangs rather than reaching the handler.
This points at a bug in syn68k's own recompiler/dispatch (wrong branch
target computation, not the shared memory-model issue from §1 — nothing
faults, it just computes the wrong PC). `test_all_cpu_engines()` skips
`test_illegal_opcode_exception()` for `syn68k` with a comment explaining
why, so it doesn't hang the whole suite. **Not fixed — separate follow-up.**

---

## 4. Test suite changes at a glance

- `BasiliskII/Musashi/test_integration.cpp`:
  - Updated the "unmapped ranges" assertions in `test_memory_banking()`
    to match §1's new dummy-backed-everything invariant (previously
    asserted `PROT_NONE` holes at `0x20000000` and the framebuffer slot
    before `VideoInit()`; now asserts they're mapped and read as zero).
  - Added `test_illegal_opcode_exception()`, wired into
    `test_all_cpu_engines()` for every engine except `syn68k`.
- `BasiliskII/memory.cpp`, `docs/basilisk-ii-boot-and-patch.md`: see §1.
