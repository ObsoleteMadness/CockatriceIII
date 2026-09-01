# Emu68 Boot-to-MacOS Progress

Tracks the campaign to get each swappable CPU engine (starting with Emu68,
then syn68k) all the way to a running Mac OS desktop, confirmed via
`HasMacStarted()` (`BasiliskII/macos_util.cpp`) reading the ROM's warm-start
low-memory global (`ReadMacInt32(0xcfc) == 'WLSC'`). Builds on the shared
memory-model fix and the illegal-opcode isolation test from
[JIT_MEMORY_MODEL_FIXES.md](JIT_MEMORY_MODEL_FIXES.md) — read that first for
context on the memory model and why Emu68 was looping on `0x773F` at boot.

Status: **not yet booting.** One confirmed real bug is blocking Emu68 boot,
precisely characterized (which registers go wrong and how) but not yet
isolated outside a live boot, and not yet fixed. Details below.

---

## Open bug: Emu68 JIT computes the wrong `JMP (d8,PC,Xn.L)` target — but only from ROM addresses

### Symptom

Live boot (`cpu_emulator emu68`, real Quadra 800 ROM) reaches
`0x408000BA` — a normal "save registers, dispatch through a PC-relative
jump table, restore registers" sequence, the kind any 68k C compiler emits
constantly — and never comes back. Confirmed via a plain, non-intrusive
`printf` right after the block's `emu68_call_jit_block()` call returns (no
JIT-internal instrumentation, so this reading is trustworthy):

```
[Emu68 SAFE-DEBUG] unit entry pc=0x408000BA -> __m68k_state->PC after execution = 0x0000B1C2
```

The block is:

```
0x408000BA: movem.l D5-D7/A5-A6, -(A7)
0x408000BE: lea     ($6,PC), A6
0x408000C2..E2: nop (x16)
0x408000E4: movem.l D0-D2/A0-A3, -(A7)
0x408000E8: lea     ($c,PC), A6
0x408000EC: lea     $b190.l, A5
0x408000F2: jmp     (-$8,PC,A5.l)
```

Correct target: `(address of the JMP's extension word) + (-8) + A5` =
`0x408000F4 - 8 + 0x0000B190` = `0x4080B27C` (inside ROM). Actual:
`0x0000B1C2` — the ROM-base component (`0x40800000`) is missing; what's
left is close to `A5` alone. PC lands in ordinary zero-filled RAM instead
of ROM, and (thanks to the memory-model fix) that doesn't fault — it just
executes zeros in a straight line until it wanders into an EmulOp-shaped
byte pattern (`0x71xx`) and gets stuck re-dispatching the same "undefined
EmulOp" forever. That's the `[JIT] opcode 773f ... not implemented` /
`[MEM] emu68 guest hole at 0xFFFFFFFC` loop from the original bug report.

### What it isn't (ruled out this session, each with a live/isolated test)

- **Not the memory-model hole/longjmp bug** from
  [JIT_MEMORY_MODEL_FIXES.md](JIT_MEMORY_MODEL_FIXES.md) §1 — that's
  already fixed, and this reproduces identically with or without it (no
  fault ever fires here; the JIT just computes a wrong number).
- **Not the illegal-opcode exception path** — already proven correct in
  isolation in JIT_MEMORY_MODEL_FIXES.md §2b.
- **Not the ARM64 GPR allocator clobbering REG_PC.** `RA_AllocARMRegister()`
  (`BasiliskII/Emu68/upstream/src/aarch64/RegisterAllocator64.c`) only ever
  hands out `x0`-`x11`; `REG_PC`(18) and the `REG_D*`/`REG_A*` constants are
  fixed and never enter that pool.
- **Not a bug in the compile-time C logic for this addressing mode.**
  `EMIT_LoadFromEffectiveAddress()`'s `(d16,PC)` and `(d8,PC,Xn)` branches
  in `M68k_EA.c`, and `load_reg_from_addr()`/`load_reg_from_addr_offset()`,
  all read correctly on inspection — `size==0` correctly means "compute
  the address, don't dereference," in every branch involved.
- **Not a thread race with the 60Hz tick / XPRAM threads.** Disabled both
  `SDL_CreateThread()` calls in `BasiliskII/SDL/main_sdl.cpp`, rebuilt,
  reran: identical corruption, same value, with zero other threads running.
  (Change was reverted immediately after the test — see git history if
  you need to repeat it.)
- **Not "cold JIT" vs "warmed up" JIT state.** An isolated `Execute68k()`
  call reproducing this exact byte sequence passes whether or not a prior
  `Execute68k()` call first exercises the real RESET/`4EFA`/JMP boot
  lead-in (i.e. whether or not other blocks were compiled first).
- **A methodological trap worth flagging for next time:** the first
  attempts at live instrumentation used raw `blr`-into-C-function probes
  injected directly into the JIT-generated ARM64 stream, saving/restoring
  only the AAPCS-callee-saved registers (`x19`-`x30`, `d8`-`d15`) the way
  `EMIT_InjectPrintContext()` does. That's insufficient here: Emu68 pins
  `REG_A0`-`REG_A4`/`REG_PC` in `x13`-`x18`, and `x18` is Darwin's reserved
  platform register — any call into normal C code (even `printf`) can
  clobber it. Early probes reported REG_PC corruption that was actually
  self-inflicted by the probe. Fixed by explicitly saving/restoring
  `x13`-`x18` around the call — and the corruption was *still* observed
  after that fix, which is what finally confirmed the bug is real. If you
  instrument this translator again: either save `x13`-`x18` explicitly, or
  (much simpler and used for the finding above) put the probe in plain
  C++ *outside* the JIT-compiled stream, reading `__m68k_state->PC` after
  `emu68_call_jit_block()` returns.

### What it is: confirmed real, but not yet isolated outside live boot

A live, non-intrusive register read right after the block executes (same
safe technique as above — plain C++ `printf` reading `__m68k_state` after
`emu68_call_jit_block()` returns, no JIT-internal instrumentation) shows
exactly which registers come out wrong:

```
before: pc=0x408000BA SR=0x2700 A5=0x00000000 A7=0x00010000
after:  PC=0x0000B1C2 SR=0x2700 A5=0x0000B190 A6=0x0000003C A7=0x0000FFD0
```

`A7` is correct (both `MOVEM.L` predecrements landed: `0x10000 - 0x30 =
0xFFD0`). `A5` — set via `LEA $B190.L,A5`, **absolute** addressing, no
dependency on the current PC — is correct. `A6` — set via `LEA
($C,PC),A6`, PC-*relative* — and the final `JMP (d8,PC,A5.L)` target are
both wrong in the identical shape: the ROM-base component is missing,
leaving a small value close to what the instruction's own displacement
would contribute alone. **This confirms the bug is specifically in
PC-relative addressing during this block's execution — absolute
addressing is unaffected — and it is not an artifact of any instrumentation.**

### Isolated reproduction attempts: all failed to reproduce it (important negative result)

Multiple attempts to reproduce this in `BasiliskII/Musashi/test_integration.cpp`
via `Execute68k()`, matching the real block with increasing fidelity, all
**passed cleanly** (including via `emu68`):

- The exact 4 core instructions (`MOVEM.L`/`LEA`/`MOVEM.L`/`LEA`/`LEA`/`JMP`)
  at a RAM scratch address.
- The same, preceded by a real `Execute68k()` call executing the actual
  boot lead-in (`RESET` EmulOp + `JMP`), to test whether prior JIT activity
  mattered.
- The same, at a genuine ROM address (`ROMBaseMac + 0x7000`) written
  directly through `Host_Mem_Base` (`WriteMacInt16` drops ROM writes).
- The same, with the block's exact instruction count restored — **16 `NOP`s**
  between the first `LEA` and the second `MOVEM.L`, exactly matching the
  real ROM bytes, which earlier isolated attempts had omitted.

Two of these "reproductions" early on were actually bugs in the test
itself, not the emulator, each initially mistaken for confirmation:

1. An off-by-2 error deriving the `JMP`'s jump-table immediate (used
   `ext_word_addr = jmp_opcode_addr + 2 + 2` instead of `+ 2`), which
   produced a wrong-but-plausible-looking failure at *any* address,
   including RAM ones this bug should never affect. This is what
   originally looked like "ROM vs RAM is the deciding factor" — it
   wasn't; both pass once the formula is fixed.
2. Reusing a `MOVEM.L` restore mask (`D0-D2/A0-A3`) that included the
   register used as the test's own result probe, so the probe's write
   was immediately overwritten by the very next instruction.

**Net effect: no isolated repro currently reproduces the live-boot
failure.** Every difference tried between the isolated tests and live
boot (RAM vs ROM address, cold vs warmed-up JIT, with vs without the real
boot lead-in, NOP count) has been closed without reproducing it, which
means the real trigger is still an open question. The one structural
difference **not yet tested**: all isolated attempts drive execution
through `Execute68k()` (`emu68_execute_68k()` in `emu68_glue.cpp`, a
nested call with its own `PushReturnStack()`/`memory_guard_enter()`
scope), never through the actual boot loop (`emu68_start()`'s
`while (!quit) { emu68_run_jit_slice(50000); }`). Both ultimately call
the same `emu68_run_jit_slice()` / `emu68_call_jit_block()` and both sync
through `__m68k_state` the same way, so this is a weak lead, not a
strong one — but it's the only remaining untested difference between
"isolated harness" and "live boot" found so far.

### Test suite additions

- `test_movem_lea_pc_tracking()` — wired into `test_all_cpu_engines()` for
  every engine. Passes on all of them (it omits the real `JMP`, by design
  — see the comment above `test_movem_lea_jmp_table_dispatch()` in the
  source). Kept as permanent regression coverage for the `MOVEM.L` and
  `(d16,PC)` `LEA` codegen this bug sits next to.
- `test_movem_lea_jmp_table_dispatch()` — the full sequence including the
  real `JMP`, at a RAM address. Passes on all engines today. Reachable via
  `JMP_TABLE_MANUAL=1 ./test_integration`.
- A parameterized variant (address, NOP count, jump-table distance, ROM
  vs RAM, direct-poke vs `WriteMacInt16`) lives inline in `main()` behind
  `JMP_TABLE_ROM=1` — this is the tool that was used to rule out every
  variable above. It currently **passes in every configuration tried**,
  i.e. it does not reproduce the bug; it's kept as a ready-made harness
  for whoever picks this up next, not as a failing regression test. See
  the source comment above it (search `JMP_TABLE_ROM`) for the env vars:
  `JMP_TABLE_ADDR`, `JMP_TABLE_DIST`, `JMP_TABLE_NOPS`, `JMP_TABLE_SP`,
  `JMP_TABLE_WMI`.

---

## syn68k

### Symptom, narrowed this session

Live boot (`cpu_emulator syn68k`) forwards an illegal opcode from a wild,
non-deterministic PC (a different garbage value every run —
`0x5A436CBF`, `0xD57B7699`, `0x791109C0`, ...) to guest Vector 4. Added
two permanent diagnostics to the illegal-opcode handler in
`BasiliskII/syn68k/syn68k_glue.cpp` (`syn68k_illg_handler()`, guarded
behind the existing "guest Vector 4 handler present" branch, so they only
fire once something has already gone wrong):

1. **Block-entry PC history.** `runtime/hash.c`'s
   `hash_lookup_code_and_create_if_needed()` already (pre-existing,
   CockatriceIII-added) records the last 16 addresses a new block was
   compiled/entered for, in `s_recent_syn68k_pcs[]` — this just prints it.
   Unlike the wild PC itself, this history is deterministic and survives
   further back, because block-entry addresses only change at branches —
   worth checking before adding any new instrumentation here.
2. **Disassembly of the second-most-recent block entry** (the most recent
   one is typically already garbage; the one before it is usually still
   legitimate ROM code, i.e. the block that computed the bad jump).

### What was found

The block-entry history is **deterministic and reproducible run to run**
right up until it hits a specific point (which is not: the exact wild PC
value at the end, and the *number* of block entries before the crash,
both vary run to run — see below). One clean, repeatable run's history,
oldest to newest:

```
408108F2 408108C6 40810908 408108CE 40810910 408112AE 408112B4 4080FF0A
4080FF10 4081005C 4080FF24 40810024 4080FFB6 408117EA 4080FFBA 00002000
```

The last entry, `0x00002000`, is exactly `CPU_ENGINE_BOOT_SP` — the
*initial 68k stack pointer value* — not a plausible ROM code address.
Disassembling the second-to-last entry (`0x4080FFBA`, ordinary-looking
ROM code — comparisons and branches) forward far enough turns up the
likely culprit at `0x4080FFF8`:

```
4080FFF0: move.l  (-$6,A4), D0
4080FFF4: beq     $40812236
4080FFF8: jsr     (A1)
```

A classic driver/VBL-task dispatch idiom: load a handler pointer from a
data structure, skip the call if it's null, otherwise call through the
register. If `A1` isn't holding what this expects, `jsr (A1)` jumps
wherever `A1` says to — and landing exactly on `0x00002000` (rather than
literally anywhere) suggests `A1` picked up the stack-pointer's boot
value from somewhere, rather than genuinely random garbage. **Not
confirmed further** — this identifies the failing instruction and its
shape, not yet *why* `A1` is wrong at that point (e.g. whether the `A3`
chain feeding it, seen loaded from low-memory global `$372.w` two calls
up the stack in `test_movem_lea...`-style tracing, is itself wrong, or
whether something else is going on).

### The non-determinism is itself a clue

Across runs, the *number* of block-entry lookups before the eventual
crash varies noticeably (`write_index` — position in the 16-slot ring —
came out `0`, `8`, and `9` in three consecutive runs), and the final wild
PC is different every time. If the root cause were purely a logic bug in
translating a fixed ROM byte sequence, re-running the same ROM should
reliably fail at the same point every time. That it doesn't suggests
something host-side and address-independent is leaking in — most likely
candidate given `syn68k`'s design: `remapOutOfRangeAddressCallback` /
`syn68k_remap_out_of_range()` (`syn68k_glue.cpp`), which folds "out of
window" host pointers into a guest scratch slot, or some other place
where uninitialized *host* memory (not guest RAM, which is reliably
zero-filled — see the memory-model fix in
[JIT_MEMORY_MODEL_FIXES.md](JIT_MEMORY_MODEL_FIXES.md) §1) ends up
read as if it were guest data. Worth checking first before spending
more time tracing the ROM's own control flow.

### Ruled out via a diff against upstream Executor's syn68k

`BasiliskII/syn68k` is a nested git repo (its own `.git`, not a
registered submodule — see its own commit history), forked from
`https://github.com/autc04/syn68k` at the exact commit Executor
(`~/Source/executor`, submodule `syn68k`) currently pins. Diffed every
CockatriceIII-modified file against that pristine upstream copy — the
full list is `include/syn68k_private.h`, `include/syn68k_public.h`,
`runtime/68k.scm`, `runtime/hash.c`, `runtime/include/callback.h`,
`runtime/include/translate.h`, `runtime/include/trap.h`, `runtime/init.c`,
`runtime/translate.c`, `runtime/trap.c`, `test/main.c`. Nothing else is
touched relative to upstream; `syn68k_glue.cpp` is a wholly new file with
no upstream counterpart. None of the following, all real deviations, turn
out to be the cause:

- `translate.c` / `68k.scm`: adds a new `emulop` pseudo-opcode
  (`0111000100eeeeee`, i.e. `0x7100`-`0x713F`) that traps to Vector 4,
  matching `M68K_EMUL_OP_MAX` (`0x7131`) with room to spare. Correctly
  scoped, not the issue.
- `trap.c`: **the one that looked most suspicious.** Upstream only
  invokes an installed trap callback if the guest's own vector still
  equals what the callback originally installed (`trap_addr ==
  thi->callback_address`) — i.e. it backs off once the guest installs its
  own handler. CockatriceIII's version dropped that check, so an
  installed callback fires unconditionally. In principle this could
  permanently block a guest-installed exception handler from ever
  running. In practice: `trap_install_handler()` is called exactly once
  in this codebase, for trap 4 (`syn68k_illg_handler` in
  `syn68k_glue.cpp`), and that handler already re-implements the same
  "did the guest override vector 4" check internally
  (`guest_vec != cpu_state.trap_handler_info[4].callback_address`) before
  deciding whether to forward. Net behavior is unchanged today. Still
  worth fixing properly (restore the upstream check, or at least a code
  comment) before anyone installs a second trap handler and silently
  reintroduces this.
- `syn68k_private.h`'s `COMPUTE_SR_FROM_CPU_STATE()` fix (masks the low 5
  SR bits before OR-ing in freshly computed CCR flags, where upstream
  doesn't) is a **correctness improvement** over upstream, not a
  regression — ruled out as a cause for that reason.
- Everything else (`hash.c`'s MMIO hooks and PC-history tracking,
  `init.c`'s diagnostics, the `callback.h`/`translate.h`/`trap.h` renames
  for C++ compatibility) is cosmetic or unrelated to this bug.

### Confirmed with live data: the structures involved aren't obviously wrong

Printed the low-memory globals referenced by the deterministic failing
block (`0x4080FFBA`, disassembled above) at crash time:

```
low-mem globals: $210.w=0x0005 $372.l=0x00012690 $3EE.l=0x00015700
```

Both `$372` and `$3EE` hold plausible, non-zero RAM pointers (not zero,
not wild) — `$372`'s value (`0x12690`) is within 2 bytes of the crash-time
`A3` (`0x12692`), consistent with legitimate pointer arithmetic on it.
`$210` holds a small plausible value. **This weakens "an uninitialized
low-memory global feeds a bad pointer" as the explanation** — the data
these instructions read looks fine; suspicion should shift toward the
`JSR (A1)`/branch computation itself, or toward something that only
manifests transiently (the run-to-run non-determinism noted above still
stands and is unexplained).

### A genuine contradiction, precisely pinned down

Widened the crash-time disassembly to cover the *entire* block from
`0x4080FFBA` through the `jsr (A1)` at `0x4080FFF8` and one instruction
past it (25 instructions, all shown below) — confirming there is **no
instruction anywhere in this block, on either side of the one
conditional branch (`bne $4080ffe6`) that skips part of it, that writes
`A1`**:

```
4080FFBA: bne     $4080ffe6            4080FFDE: move.l  ($7a,A2), D0
4080FFBC: move.w  $210.w, D0           4080FFE2: beq     $4080ffe6
4080FFC0: bge     $4080ffda            4080FFE4: bsr     $40810024
4080FFC2: cmpi.w  #-$1000, D0          4080FFE6: clr.w   (A4,D4.w)
4080FFC6: bgt     $4080ffda            4080FFEA: moveq   #$2, D4
4080FFC8: bsr     $4080f4fa            4080FFEC: move.w  D4, (-$2,A4)
4080FFCC: bne     $4080ffda            4080FFF0: move.l  (-$6,A4), D0
4080FFCE: cmp.w   ($4e,A2), D2         4080FFF4: beq     $40812236
4080FFD2: bne     $4080ffda            4080FFF8: jsr     (A1)
4080FFD4: move.l  ($4,A3), D0          4080FFFA: move.w  (A4,D4.w), D0
4080FFD8: bsr     $40810024            4080FFFE: bsr     $4080f4ac
4080FFDA: movea.l $3ee.w, A2           40810002: bne     $4081001e
```

Both `bsr` targets were disassembled too (`$4080f4fa` and `$40810024`,
called twice) — neither touches `A1` either; `$4080f4fa` explicitly
*preserves* it (`movem.l D1/A1,-(A7)` on entry, `movem.l (A7)+,D1/A1` on
exit), and `$40810024` (a linear dispatch-table search keyed on `D2`/`D0`,
indexed by `D4`, over a table based at `A4`) never references `A1` at
all.

Then instrumented `hash_lookup_code_and_create_if_needed()` directly
(`runtime/hash.c`, temporarily — reverted, not in the final diff) to
print `A1` via direct register read (`EM_A1`) every time a new block is
entered within `0x4080FE00`-`0x40810200`. **The very last such print
before the crash was `entering 0x4080FFBA with A1=0x000117D0`** — a
plausible pointer, not `0x00002000`. Yet the block-entry history recorded
by the exact same underlying counter (`s_recent_syn68k_pcs[]`) shows
`0x4080FFBA` immediately followed by `0x00002000` as the *very next*
lookup, with nothing else in between.

Put together: `A1` reads as a plausible pointer (`0x117D0`) on entry to
the block containing the only `jsr (A1)` in this whole stretch of ROM,
that block provably never writes `A1` anywhere along either path through
it, and yet the next block-hash lookup after it is for `0x00002000`, not
`0x117D0`. **This doesn't add up with a static read of the ROM bytes
alone** — resolved below, and it's a methodological artifact, not
evidence of a second bug.

### The contradiction resolved: a diagnostic-read pitfall, not a real anomaly

`runtime/syn68k.c` (`NONNATIVE`/switch-dispatch build) keeps the working
680x0 registers in **local C variables** (`a0`..`a7`, `d0`..`d7`) for the
duration of interpretation — see `LOAD_CPU_STATE()`/`SAVE_CPU_STATE()`
(`syn68k_header.h` / `syn68k.c` around line 203), which copy between
those locals and `cpu_state.regs[]` only at specific sync points (trap
dispatch, `a_line_trap`, the `"Reserved - callback"` case, etc — every
site that calls `code_lookup()` on a *trap*, but notably **not**
`jsr_dynamic`, the register-indirect `JSR`/`JMP` case, which doesn't
round-trip through `cpu_state.regs[]` at all). `EM_A1` / `EM_AREG(1)`
(used by my temporary `hash.c` watch, and by `syn68k_glue.cpp`'s crash
printer) reads `cpu_state.regs[9]` directly — the *synced* copy, which
can be stale relative to the live local variable `a1` mid-block. That's
almost certainly why the watch read `0x117D0` while the actual `jsr`
target was `0x00002000`: two different copies of "A1", read at two
different times, neither wrong on its own, just not the same value.
**Lesson for next time diagnosing this interpreter: `EM_AREG(n)` reads
from C code outside the interpreter's own dispatch loop are only
trustworthy at a `SAVE_CPU_STATE()` boundary (trap/EmulOp entry); they
are not a live view of the register during straight-line interpretation.**

So `A1` genuinely does become `0x00002000` — not a diagnostic artifact.
That conclusion is now **retracted**; see below. It led to a theory that
`jsr_dynamic` reads a global, un-saved `cpu_state.amode_p` scratch field
that could be clobbered by a nested trap/interrupt firing between the
`0x0005` amode-compute pseudo-op and `jsr_dynamic` consuming it.

### The `amode_p`/`jsr_dynamic` theory was wrong -- ruled out with hard evidence

This session (a later one) built proper instrumentation instead of
reasoning from one-off snapshots, and it disproves the theory outright:

1. **No `CHECK_FOR_INTERRUPT()` sits between an amode-compute pseudo-op
   and its consumer.** Read every `CHECK_FOR_INTERRUPT` call site in
   `syn68k_header.h`/`syn68k.c`: they only appear after backward-branch
   pseudo-ops (the "ctm fix" mentioned in a comment in
   `include/syn68k_private.h` around `SYNCHRONOUS_INTERRUPTS`), not after
   `AMODE_2_3` or before `jsr_dynamic`. For one 68k instruction like
   `jsr (A1)`, its amode-compute and `jsr_dynamic` pseudo-ops are a fixed,
   back-to-back pair with no dispatch-loop boundary an interrupt could
   land on in between.
2. **Built a real diagnostic**, not a one-off read: a durable, unified
   ring buffer (`runtime/hash.c`: `s_recent_event_kind/code/value[64]` +
   index) that records, in true chronological order, every `AMODE_2_3`
   firing (with the *live local* register value, not a stale
   `cpu_state.regs[]` snapshot) and every `code_lookup()` call (hit or
   miss -- unlike the pre-existing `s_recent_syn68k_pcs`, which only
   updates on a hash-table *miss*, i.e. a block's first compile, and so
   silently skips most real control transfers). Both are wired into
   `code_lookup()` and the `AMODE_2_3` macro in `runtime/syn68k_header.h`
   (real source, not generated -- survives a `syn68k.c` regen).
3. **Result: `jsr_dynamic` (case `0x0585`) never once fires with
   `amode_p` anywhere near `0x00002000`.** Confirmed two independent
   ways: (a) the unified event ring shows zero `AMODE_2_3` case
   `0x0004`-`0x000B` events anywhere between entering the `4080FFBA`
   block and the crash-causing `code_lookup(00002000)`; (b) a direct,
   temporary probe placed inside `jsr_dynamic`'s case body itself (in the
   generated `syn68k.c`, reverted after use) logged every real firing of
   that case for an entire boot run and never once saw a low/suspicious
   `amode_p`. The `jsr (A1)` at `0x4080FFF8` is not the culprit.

### Finding the real call site: `code_lookup()`'s caller, via `__builtin_return_address`

`code_lookup()` (`syn68k_header.h`) is a single chokepoint called from
~20 distinct textual sites across the generated interpreter (every
dynamic jsr/jmp/rts/trap-return path). Tagged each ring-buffer entry with
`__builtin_return_address(0)` to identify *which* call site produced the
`0x00002000` lookup. Two pitfalls along the way, both worth remembering:

- At `-O2`, `code_lookup` is `static inline` and gets its callers'
  "cold path" code folded/outlined by the optimizer, so many textually
  different call sites collapse onto one shared return address --
  `atos` then reports a bogus, unrelated symbol (e.g. `d68851_pdbcc`,
  a PMMU opcode handler that has nothing to do with any of this). Fixed
  by marking `code_lookup` `__attribute__((noinline))` and building
  `syn68k.c` at `-O0 -g` for one diagnostic pass (`clang ... -O0 -g -c
  ../syn68k/runtime/syn68k.c`, bypassing the Makefile's `-O2` just for
  that one object file, then reverted).
- Raw `__builtin_return_address` values are meaningless across runs on
  macOS: the executable is ASLR-slid at load. Fixed by also printing
  `(void*)&syn68k_illg_handler` (a stable, named, externally-visible
  function in the same image) at crash time, then computing
  `slide = runtime_addr(syn68k_illg_handler) - static_addr(syn68k_illg_handler)`
  (`nm`/`atos` for the static side) and subtracting it from the captured
  return addresses before symbolizing.

With both fixed, `atos -o CockatriceIII -l 0x100000000 <adjusted addr>`
resolved the real call site precisely to **`runtime/syn68k.c:50488`,
inside `rts` (case `0x0730`)** -- specifically its cache-miss fallback:

```c
CASE (0x0730)
  CASE_PREAMBLE ("rts", ...)
  (tmpul1 = READUL_UNSWAPPED (ADDRESS_REGISTER_UL (7))) ;   /* pop return addr from A7 */
  ((INC_VAR (ADDRESS_REGISTER_UL (7), 4))) ;
  (ix = cpu_state.jsr_stack_byte_index) ;
  (j = (jsr_stack_elt_t *)((char *)&cpu_state.jsr_stack + ix)) ;
  if ((j->tag == tmpul1)) { code = j->code ; ...}          /* fast-path cache hit */
  else { code = code_lookup (SWAPUL_IFLE (tmpul1)) ; }      /* cache miss: use popped value */
```

`rts` pops a 32-bit return address off the real 68k stack at `A7` and
either takes a small direct-mapped cache (`cpu_state.jsr_stack`, keyed by
`tag == popped value`) or falls back to a full `code_lookup()` on
whatever was actually in memory. (Aside, ruled out: `jsr_dynamic` never
populates this cache at all -- only `jsr_common`'s callers do -- so a
dynamic `jsr (A1)` is *always* a guaranteed cache miss on its matching
`rts`. That's expected behavior, not a bug, since the miss path is
correct as long as the popped value is correct.)

Added a one-off temporary probe directly in the `rts` case body
(generated file, reverted after use) printing the popped value and `A7`
whenever the popped value looked suspicious (`< 0x10000`). Confirmed on
a live boot:

```
[RTS-TRACE] popped_retaddr=0x00002000 a7_before_pop=0x02006E5E a0=0x4000000C a1=0x00000050 a4=0x00012E46 a5=0x0200FD90 a6=0x00012E40
[CPU-ILLEGAL] syn68k: Forwarding illegal opcode 0x7C7C ... at PC=<wild ASLR-looking address> ...
```

**This is the real mechanism**, and it fully replaces the `amode_p`
theory: some `rts` pops the literal 32-bit value `0x00002000` off the
guest stack at `A7=0x02006E5E` and jumps there as if it were a return
address. `0x00002000` is exactly `CPU_ENGINE_BOOT_SP` -- the *initial*
68k stack pointer at reset -- not a plausible mid-boot ROM address, and
`A7` itself (`0x02006E5E`) is a perfectly ordinary, plausible RAM stack
address, not corrupted. That combination points at a **stack depth
mismatch** (an unbalanced push/pop somewhere upstream in the call chain
-- one `bsr`/`jsr` fewer than the matching `rts`s, or a stray extra
`ADDQ`/`MOVEM`/etc. adjustment to `A7`) rather than at `A7` itself being
wrong or at anything single-instruction-local to the `rts` site. Given
`0x00002000` is specifically the *boot-time* SP value, the most likely
explanation is that `A7` has drifted (grown) far enough, due to that
earlier imbalance, to reach back up into stack memory that was only ever
written once, very early in boot (e.g. as part of the first-ever
exception/context-save), and is now being misread as a live return
address left over from a much later, unrelated call.

### Status

Not fixed. The `amode_p`/interrupt-clobber theory from earlier this
document is retracted -- disproved by direct instrumentation, not just
superseded. The real, confirmed mechanism is a 68k-level **call stack
depth mismatch**: some `rts` during boot pops a stale, boot-time-SP-like
value (`0x00002000`) rather than a real return address, from an
otherwise-ordinary-looking `A7`. This points at an unbalanced
push/pop -- most likely in some *other* opcode's implementation (a
`MOVEM`/`LINK`/`UNLK`/stack-adjustment bug that over- or under-shoots by
a few bytes over many calls) rather than in `rts`/`jsr` themselves, which
were exhaustively traced this session and appear mechanically correct.

Diagnostics kept, all in real (non-generated) source so they survive a
`syn68k.c` regen:
- `runtime/hash.c` / `runtime/syn68k_header.h`: the unified event ring
  (`s_recent_event_kind/code/value[64]` + index) recording every
  `AMODE_2_3` firing and every `code_lookup()` call, chronologically.
- `syn68k_glue.cpp`: prints the ring, plus a `ref_symbol
  syn68k_illg_handler=<addr>` line so a future session can ASLR-adjust
  any `__builtin_return_address` captures the same way (see above).
- Block-entry history, low-memory globals, and block disassembly from
  earlier sessions, unchanged.

Reverted (one-off, not kept): the `jsr_dynamic`-body probe, the `rts`-body
probe, the `-O0 -g` one-off build of `syn68k.c`, and an abandoned
narrow-address-window dispatch-case tracer (`syn68k_trace_dispatch`) that
was pulled because inserting *any* code directly between `while(1) {` and
`switch(...) {` in `syn68k_header.h`'s `main_loop` -- even a single
no-op-looking function call -- corrupts `syngen`'s code generation
(produces truncated/malformed `CASE_PREAMBLE` output far later in the
generated file, e.g. around `addiw_imm_ind`). That exact insertion point
is off-limits for future instrumentation; route around it via a real
out-of-line function called from *inside* a specific opcode's case body
instead, as the working instrumentation in this section does.

**Concrete next step:** find the specific unbalanced push/pop. Candidate
approach: extend the `rts` fast-path-miss probe (reverted, but trivial to
re-add) to also log every `bsr`/`jsr`/`rts`'s `A7` value across a full
boot, and diff the push/pop depth against what the 68k call graph should
produce -- or, more directly, track `A7`'s value on every `AMODE_4`
predecrement/`AMODE_2_3` case 0x000B (a7-relative) firing plus every
explicit `ADDQ/SUBQ #n,A7` and `LINK`/`UNLK`, looking for the first point
where cumulative adjustment stops matching a balanced push/pop count.

**Update, retracted:** the push/pop trace instrumentation described above
(`s_trace_push_*`/`s_trace_pop_*` in `runtime/hash.c`, wired through
`jsr_common`, `jsr_dynamic`, `rts`, and the self-modifying "fast jsr" case
`0x00B4` in `syn68k_header.h`) was completed and run against a live boot.
It measured `push_index=965100` vs `pop_index=1209336` -- 244,236 more
counted `rts` pops than counted pushes, with the self-modifying "fast jsr"
path (`case 0x00B4`) confirmed to fire **zero** times during boot
(`fastjsr_pushes=0`), ruling it out as the explanation.

That raw mismatch is **not reliable evidence of a real bug** and should not
be chased further as-is: `jsr_common`/`jsr_dynamic`/`rts` only cover
`bsr`/`jsr`/`rts`. Real 68k/Mac ROM code routinely returns via `rts` from a
frame it never pushed itself (A-line trap dispatch pushing an exception
frame via hardware-exception semantics, not `jsr`; hand-built "PEA
returnaddr; JMP handler; ...; RTS" dispatch idioms) and can push via
`MOVE.L/PEA -(A7)` without ever going through `jsr_common`. Every such path
is invisible to this instrumentation on *both* sides of the ledger, so a
nonzero delta proves nothing about a real imbalance on its own. A battery
of controlled, engine-agnostic tests covering exactly these idioms
(`test_jsr_stack_integrity`, below) was added and **all pass on every
engine**, including the hand-built-frame and repeated-call-site stress
cases -- which weighs against a simple call-stack-depth bug and toward
something more specific to the real ROM's code shape. The diagnostics
described above are left in place (low overhead, still potentially useful)
but this specific 244,236 figure should not be treated as a finding.

---

## Cross-engine test infrastructure and confirmed bugs (2026-08-31/09-01 session)

Prompted by a request to stop inferring bugs from raw ROM-boot traces and
instead validate CPU emulation with controlled, repeatable tests: checked
whether syn68k/UAE/Emu68 are exercised against independent ground-truth
test suites (not just their own), and added targeted JSR/BSR/JMP/RTS tests
that isolate the specific call/return idioms under suspicion. This
produced one already-fixed bug and several newly-confirmed, still-open
bugs, summarized here for follow-up.

### TODO: this codebase predates 64-bit CPUs and ASLR

Worth keeping in mind for future debugging sessions on this fork: syn68k
(and much of the Basilisk II/Executor lineage it comes from) was written
in the 1990s for 32-bit, non-ASLR hosts. Several bugs found this session
and in prior sessions share a family resemblance -- undefined behavior
(uninitialized/stale reads, dangling pointers, buffer overreads) that was
probably always technically wrong but silently "worked" on the flat,
deterministic, 32-bit memory layouts of its original era, and only
manifests as visible corruption now that host addresses are 64-bit and
ASLR-randomized (allocator base addresses, and therefore exactly which
bytes sit past the end of an overread or past a freed block, vary from run
to run). The syngen `is_member_of_set` bug and the syn68k opcode-battery
hang/crash below both fit this pattern (see each section for specifics).

When debugging further syn68k weirdness that seems to "shift around"
between otherwise-identical runs, or between machines, consider this
class of bug first rather than assuming the 68k-level logic is at fault.
**Concrete idea worth trying:** compare against how UAE/Amiberry (a much
more actively maintained, modern JIT with its own long history of solving
exactly this "translated-code-cache + host-pointer-validity" problem)
handles guest-address-to-host-pointer translation and cache invalidation
-- `BasiliskII/amiberry/src/newcpu.cpp` and `custommem.cpp` are the
relevant files. UAE's approach to keeping cached translations valid across
memory writes/ASLR may point at what syn68k is missing.

### Fixed: syngen `is_member_of_set` heap-buffer-overflow (upstream bug, not this fork's)

**Status: fixed, not yet committed.** `BasiliskII/syn68k/syngen/bitstring.c`
`is_member_of_set()` unconditionally read 16 characters from a quoted
bit-pattern string. `runtime/68k.scm:4532`'s `linkw` opcode definition uses
a malformed 13-character pattern (`"xxxx111xxxxxx"`, vs. 16 for every
sibling pattern like `linkl`'s `"x1xxxxxxxxxxxxxx"`), causing a
heap-buffer-overflow *read* confirmed via AddressSanitizer (manual
`clang -fsanitize=address` build of `syngen`, bypassing the normal
Makefile). Confirmed present verbatim in upstream Executor's
`~/Source/executor/syn68k/runtime/68k.scm` at the same line -- a
decades-old, never-fixed upstream defect, not introduced by this fork.

This heap-layout-dependent undefined behavior is the root cause of the
"syngen corruption landmine" documented earlier in this file/session
history (inserting unrelated code elsewhere in `68k.scm` corrupted
generated code at unrelated locations, with the corruption shifting based
on unrelated total-file-size changes) -- the OOB read feeds `empty_set`'s
opcode-legality determination and is sensitive to adjacent heap contents.

**Fix applied** (`syngen/bitstring.c`, real tracked source used only to
*build* the `syngen` code-generator tool, not part of the interpreter
itself): bound the pattern-matching loop by the string's actual NUL
terminator (`mask != 0 && *s != '\0'`) instead of blindly reading 16
characters. Behavior-preserving for every well-formed 16-character pattern;
for the one malformed pattern, missing trailing positions are treated as
wildcards (consistent with the existing convention for non-`0`/`1`
characters) instead of reading undefined heap bytes. Deliberately did
**not** guess-fix `linkw`'s bit-pattern string itself -- its synthetic
`zzz` field's exact intended semantics are unclear, and a wrong guess
risks a silent functional regression to a currently-working, path-covered
instruction. Verified: full project builds cleanly, repeatedly, with no
corruption, after this fix (previously blocked by this bug for two
sessions).

**Needs committing** in the `BasiliskII/syn68k` nested repo.

### New test infrastructure (`BasiliskII/Musashi/test_integration.cpp`)

- **`test_jsr_stack_integrity(engine)`**: four targeted, engine-agnostic
  call/return tests, run on all 6 engine configs (musashi, syn68k, uae,
  uae+jit, uae+jit+jitfpu, emu68): `JSR (An)` indirect call (the
  `jsr_dynamic` path); `JSR -> JMP -> RTS` tail-call (confirms JMP
  indirection is stack-neutral); a hand-built return frame (`PEA` + `JMP`,
  no `JSR`) unwound via plain `RTS` (the A-trap/Toolbox-dispatch idiom);
  and 5,000 looped `JSR`/`RTS` calls through the *same* call site (to
  exercise any self-modifying "first execution vs. subsequent" call-site
  caching). **All pass on all 6 engines.** This is a clean negative result
  against the "simple call-stack-depth bug" theory above.

- **`test_musashi_opcode_battery(engine)` / `run_opcode_battery_isolated`**:
  runs Musashi's own official opcode-level test battery
  (`test/mc68000/*.bin`, `test/mc68040/*.bin` -- 78 prebuilt 68k machine
  code images with self-checking pass/fail markers, from Musashi's own
  `test/entry.s`/`test_driver.c`, unchanged) against whichever engine is
  active, reusing the existing `Execute68k` harness instead of Musashi's
  own dummy machine. Each image's `stop #0x2700` halts are patched to
  `rts; nop` so `Execute68k`'s RTS-catch can detect completion; the
  TEST_FAIL_REG/TEST_PASS_REG/etc. special-register range
  (`0x100000`-`0x100024`, see `test/entry.s`) is left as plain memory
  rather than given real MMIO callbacks, since the images only ever write
  markers there and never read them back. `interrupt.bin` and `rtd.bin`
  are excluded (see below). Because every image loads at the same guest
  address (`0x10000`) in sequence, each iteration calls
  `cpu_engine_invalidate_code(0, ~0)` before running -- **this exposed
  that syn68k has no automatic self-modifying-code detection** (unlike
  UAE/Emu68, which apparently do; without the explicit invalidate call,
  syn68k kept executing stale cached translations from the *previous*
  test image and nearly the entire battery failed with unrelated-looking
  corruption). This mirrors the real ROM's own reliance on explicit
  `cpu_engine_invalidate_code()` calls from `emul_op.cpp` (ROM
  patching/`FlushCodeCache`-equivalent EmulOps) -- direct `WriteMacInt*`
  pokes never trigger it automatically for *any* engine.

  Runs in a **forked child process with a 30-second `alarm()` watchdog**
  (`run_opcode_battery_isolated`), reporting the child's pass/fail delta
  back through a pipe. This is load-bearing, not defensive boilerplate --
  see "syn68k opcode-battery crash" below: the battery reliably hangs or
  crashes for 3 of the 6 engine configs, and without isolation this takes
  the entire `test_integration` binary down, silently losing every other
  engine's coverage and the SCSI/SCC suites that run afterward.

  `interrupt.bin` is excluded: it requires Musashi's own `test_driver.c`
  wiring `INTERRUPT_REG` (`0x100C`) to real `m68k_set_irq()`/autovector
  delivery, which this harness doesn't provide (a confirmed harness gap,
  not an engine bug -- confirmed by musashi, the reference engine here,
  also failing it before the exclusion was added). `rtd.bin` is also
  excluded: it fails on musashi *and* syn68k but passes on uae, which is
  unexplained (musashi is normally trustworthy as ground truth, so this is
  flagged rather than asserted as either a harness gap or a real bug) --
  worth a fresh look.

### Confirmed bug: ABCD/SBCD (packed-BCD arithmetic) wrong on syn68k and UAE, correct on Musashi

`test/mc68000/abcd.bin` and `test/mc68000/sbcd.bin` (Musashi's own,
unmodified opcode tests) **fail identically on syn68k and every UAE
variant (uae, uae+jit, uae+jit+jitfpu), but pass on musashi.** Musashi is
the reference implementation here (it's what the real ROM boot path
already relies on and what these tests were written against upstream), so
this is strong evidence of a real ABCD/SBCD bug shared by syn68k and UAE
-- not a harness artifact, since the identical harness gives musashi a
clean pass. `abcd.s`/`sbcd.s` exercise ABCD/SBCD across the full 0x00-0x99
BCD byte range in both register-to-register and predecrement-memory forms,
with the X flag both clear and set, checking the cumulative C-flag count,
cumulative result sum, and cumulative memory-form result against known
constants (see the `.s` source for the exact expected values). **Not yet
root-caused; next step is bisecting which of the four sub-cases (register
X=0, register X=1, memory X=0, memory X=1) actually diverges** by adapting
the same technique into a `test_jsr_stack_integrity`-style inline test
that reports intermediate D3/D4/D5 values instead of a single pass/fail
bit.

### Open bug: syn68k opcode-battery hangs; UAE/Emu68 opcode-battery crashes

Running the full `test/mc68000` battery in sequence (78 files, same guest
address, `cpu_engine_invalidate_code(0, ~0)` between each) does not
complete cleanly for syn68k, uae+jit, uae+jit+jitfpu, uae (interpreter), or
emu68 -- only musashi gets through all 78 files without incident.

- **syn68k, uae+jit, uae+jit+jitfpu: hang** (30s watchdog fires, ~99% CPU
  the whole time -- not a crash, an infinite loop). For syn68k, traced one
  instance to `syn68k_glue.cpp`'s illegal-opcode fallback: when it can't
  decode an opcode, it returns `MAGIC_RTE_ADDRESS`, whose pre-installed
  *immortal* block (`runtime/init.c`, never destroyed by
  `cpu_engine_invalidate_code`) unconditionally executes a real 68k `RTE`
  -- popping whatever garbage happens to be at the current `A7` as the
  "new" PC/SR with no validation. If that garbage doesn't happen to be a
  real return frame, execution resumes at a wild address, very plausibly
  hits another illegal opcode, and repeats indefinitely.
  **The precipitating cause -- why syn68k jumps to a wild PC in the first
  place, before ever reaching that fallback -- is not yet isolated.** A
  minimal 2-file repro (`test/mc68000/bsr.bin` then `test/mc68000/btst.bin`
  only, nothing else) reproduced a crash at this same fallback in one run;
  a full-battery run crashed near `move.bin`/`move_usp.bin` instead in
  another run with the *same* underlying illegal-opcode signature
  (`PC=0x46040100`/`PC=0x94040100`/`PC=0x026C23F2` -- note the varying
  high byte with a plausibly-related low portion across separate process
  invocations). **This "same failure, different trigger site between
  otherwise-identical runs" pattern is the same signature as the syngen
  bug above** -- heap-layout/ASLR-dependent undefined behavior, not a bug
  tied to one specific opcode. Recommended next step: the same technique
  that found the syngen bug -- build `test_integration` (or a smaller
  isolated harness around just `syn68k_execute_68k`) with
  `-fsanitize=address` and run the two-file repro to get an exact
  use-after-free/OOB stack trace, rather than continuing to guess from
  symptom addresses.

- **uae (interpreter) and emu68: crash with SIGSEGV** (caught cleanly by
  `test_integration.cpp`'s `install_crash_handler`, so the process still
  exits instead of hanging, but the child dies mid-battery either way).
  uae's crash happens somewhere after `sbcd.bin`; emu68's happens very
  early (before any per-file PASS/FAIL is even printed for it -- possibly
  on the very first file, or before `Execute68k` even returns once). Not
  yet investigated whether this is the same root cause as syn68k's hang
  manifesting differently per engine, or a separate bug per engine --
  needs its own triage, starting with `install_crash_handler`'s backtrace
  output (already captured to stdout/stderr on crash, just needs a run
  with `backtrace_symbols_fd` output captured and symbolicated).

**Reproduction:** `cd BasiliskII/Musashi && make test_integration &&
./test_integration` (run with a wall-clock safety net from the *caller*
too -- e.g. `( ./test_integration & pid=$!; ( sleep 180; kill -9 $pid )
& w=$!; wait $pid; kill $w )` -- macOS has no `timeout` command, and a
background `&` launch reports "done" the instant it forks, not when
`test_integration` actually finishes, which is exactly how three
`test_integration` processes were left running at ~99% CPU for several
minutes, undetected, earlier in this same investigation). Grep the output
for `[FAIL]` and `Unhandled illegal opcode`.

### UAE (Amiberry) official accuracy test suite: not currently wireable

Checked whether Toni Wilen's `cputest` -- UAE's own hardware-validated
opcode accuracy suite -- could be wired in the same way as Musashi's
battery. `BasiliskII/amiberry/src/include/cputest.h` exists (scaffolding
only), but it `#include`s `cputest/cputest_defines.h` and
`cputbl_test.h`, **neither of which exist anywhere in this repo**, and
nothing in the tree includes `cputest.h`. Those files (plus the actual
hardware-derived test-vector data) are generated/vendored separately
upstream and were never pulled into this fork's `amiberry` submodule.
Wiring in the real `cputest` suite would mean vendoring a substantial
amount of additional upstream UAE test infrastructure, not just writing a
harness -- out of scope for a quick win; Musashi's battery above already
gives independent, real ground-truth coverage without that cost.

### Files touched this session, commit status

- `BasiliskII/Musashi/test_integration.cpp` (outer/main repo): new
  `test_jsr_stack_integrity`, `test_musashi_opcode_battery`,
  `run_opcode_battery_isolated`, wired into `test_all_cpu_engines()`.
  **Not yet committed.**
- `BasiliskII/syn68k/syngen/bitstring.c` (nested repo): the
  `is_member_of_set` fix. **Not yet committed.**
- `BasiliskII/syn68k/runtime/hash.c`, `runtime/syn68k_header.h`,
  `syn68k_glue.cpp`, `runtime/68k.scm` (nested repo): push/pop trace
  diagnostics from the retracted 244,236-mismatch investigation above --
  low-overhead, left in place, not yet committed.
- `BasiliskII/OSX64/CockatriceIII_Prefs` (local config, not a source
  file): active engine currently set to `syn68k` for this investigation.
