# Emu68 Boot-to-MacOS Progress

Tracks the campaign to get each swappable CPU engine (starting with Emu68,
then syn68k) all the way to a running Mac OS desktop, confirmed via
`HasMacStarted()` (`BasiliskII/macos_util.cpp`) reading the ROM's warm-start
low-memory global (`ReadMacInt32(0xcfc) == 'WLSC'`). Builds on the shared
memory-model fix and the illegal-opcode isolation test from
[JIT_MEMORY_MODEL_FIXES.md](JIT_MEMORY_MODEL_FIXES.md) — read that first for
context on the memory model and why Emu68 was looping on `0x773F` at boot.

Status: **not yet booting.** One confirmed, precisely-isolated, not-yet-fixed
bug is blocking Emu68 boot. Details and a fast reproduction below.

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

### What it is: address range (ROM vs RAM) is the deciding factor

An isolated `Execute68k()` test byte-for-byte reproducing this exact block
(all four instructions, including the real `JMP`) **passes** when placed
at a low-RAM scratch address (`0x7250`) with its jump-table stub at
`0x9000`. The identical bytes, at the identical *relative* offsets, placed
instead at `ROMBaseMac + 0x7000` / `ROMBaseMac + 0x7100` (poked directly
through `Host_Mem_Base`, since `WriteMacInt16` silently drops ROM writes)
**hang** — same failure shape as live boot (`opcode 7100 at 0000fffc`
looping forever). Nothing else differs between the two tests. This is the
one variable left unexplained: something in Emu68's translator behaves
differently when the executing PC is in ROM range (`0x40800000`+) versus
RAM range (`0x00000000`-ish), for this specific `(d8,PC,Xn.L)` JMP. Not
yet root-caused further — the next step is finding what in the translator
branches on absolute address range (ICache hashing? a ROM/RAM fast-path?)
rather than treating all addresses uniformly for PC-relative arithmetic.

### Fast reproduction

`BasiliskII/Musashi/test_integration.cpp`, run manually (not wired into
the automatic suite — see below for why):

```sh
cd BasiliskII/Musashi
make test_integration
JMP_TABLE_ROM=1 ./test_integration    # hangs: reproduces the bug (Ctrl-C or timeout)
JMP_TABLE_MANUAL=1 ./test_integration # passes: same bytes at a RAM address
```

Runs in well under a second (no SDL, no real ROM, no full boot) versus
minutes to reproduce live. `JMP_TABLE_ROM=1` **hangs** while the bug is
open — always run it with a wall-clock timeout, e.g.
`timeout 10 env JMP_TABLE_ROM=1 ./test_integration` (or the
background-process-plus-`kill` pattern if your `timeout` isn't GNU
coreutils' version).

### Test suite additions

- `test_movem_lea_pc_tracking()` — wired into `test_all_cpu_engines()` for
  every engine. Passes today (it doesn't include the real `JMP`, by
  design — see the comment above `test_movem_lea_jmp_table_dispatch()` in
  the source). Kept as permanent regression coverage for the `MOVEM.L`
  and `(d16,PC)` `LEA` codegen this bug sits next to.
- `test_movem_lea_jmp_table_dispatch()` — the full sequence including the
  real `JMP`, at a RAM address. Passes today. Reachable via
  `JMP_TABLE_MANUAL=1`.
- The ROM-range variant of the same test lives inline in `main()` behind
  `JMP_TABLE_ROM=1` (not a named/registered test function — it's a
  hang-on-failure repro, not something `test_all_cpu_engines()` can run
  safely). **Once the underlying bug is fixed, promote this into a proper
  `test_*()` function wired into the automatic suite** (or add a
  cycle-budget escape hatch to `Execute68k()`/`emu68_run_jit_slice()`
  first, if you want it in CI before the bug is fixed).

---

## syn68k

Not started this session. Prior finding stands from
[JIT_MEMORY_MODEL_FIXES.md](JIT_MEMORY_MODEL_FIXES.md) §3: live boot
forwards an illegal opcode from a garbage PC (`0x5A436CBF`), unrelated to
the memory model — a bug in syn68k's own recompiler/dispatch, not yet
investigated further.
