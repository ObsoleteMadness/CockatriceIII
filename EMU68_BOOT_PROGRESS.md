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

Not started this session. Prior finding stands from
[JIT_MEMORY_MODEL_FIXES.md](JIT_MEMORY_MODEL_FIXES.md) §3: live boot
forwards an illegal opcode from a garbage PC (`0x5A436CBF`), unrelated to
the memory model — a bug in syn68k's own recompiler/dispatch, not yet
investigated further.
