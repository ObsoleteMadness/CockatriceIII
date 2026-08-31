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

### Status

Not fixed. Diagnostics are permanent (low-cost, only active on the
illegal-opcode-fault path) and should make the next pass faster —
run live boot with `cpu_emulator syn68k`, watch for
`[CPU-ILLEGAL] syn68k:`, and the block-entry history plus disassembly
will already be in the log.
