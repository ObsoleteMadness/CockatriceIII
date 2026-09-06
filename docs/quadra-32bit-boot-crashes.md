# Quadra 32-bit boot crashes (Type 10 and Type 4)

Reference for agents and humans debugging **Mac OS System Error** bombs on
Cockatrice III's Quadra 800 / 32-bit ROM path (`rom Quadra800.rom`,
`modelid 29`, `cpu 4`, `ramsize 67108864`).

Musashi is the golden path. If Musashi boots and another engine does not, the
bug is almost always engine glue (Time Manager, EmulOp ABI, interrupts), not
ROM patches or SCSI resource rewriting. Those patches already run on Musashi.

Related: [basilisk-ii-boot-and-patch.md](basilisk-ii-boot-and-patch.md) for the
host call chain; [cpu-engine-opcode-fixes.md](cpu-engine-opcode-fixes.md) for
opcode-battery UAE/Emu68 work.

---

## How to tell the two bombs apart

Mac OS "Type N" is 68k exception vector `N+1`. The host prints this from
`cockatrice_report_cpu_exception()` in `BasiliskII/cpu_engine.cpp`.

| Console | Vector | Mac bomb | Typical PC / opcode |
|---------|--------|----------|---------------------|
| `[CPU-LINEF]` or Type 10 | 11 | Line-F | `PC=0x00065AAx`, opcode `0xFF00` / fill |
| `[SYSTEM-ERROR] … vector=5` Type 4 | 5 | Zero Divide | `DIVU.W` (`0x82C5` at ~`0x001A81C8`) |

Success: `[BOOT] HasMacStarted: warm-start flag WLSC is set` from
`HasMacStarted()` in `BasiliskII/macos_util.cpp` (`0xcfc == 'WLSC'`).

Do **not** run two emulator processes against the same SCSI image at once.

---

## 1. Type 10 at `0x65AAx` — Microseconds EmulOp ABI

### Symptom

Early 32-bit boot, Happy Mac never appears. Line-F at `PC≈0x00065AAC`,
`A0≈0x00065AA4`, opcode in the `0xFFxx` / `0x00xx` fill band. Same signature
on Musashi, UAE, and m68k-rs (it is not a JIT or 32-bit memory-map bug).

### Root cause

Commit `23e7721` changed `M68K_EMUL_OP_MICROSECONDS` to write a 64-bit count
through `A0` as `UnsignedWide*`.

The ROM stub in `rom_patches.cpp` is `EMUL_OP + RTS`. Trap `$A093`
(`Microseconds`) callers expect **Basilisk's original register ABI**:
`A0 = hi`, `D0 = lo`. Leaving `A0` as a heap/data pointer made the next
`JSR (A0)` / jump land in `0x65AAx` (uninitialized / fill) → Line-F → Type 10.

Bisect (Musashi, Quadra ROM, System 8.1 HDA): phases 0–4 all booted;
`23e7721` was the first bad commit. Reverting only this EmulOp on that commit
booted.

### Fix

`BasiliskII/emul_op.cpp`:

```cpp
case M68K_EMUL_OP_MICROSECONDS:
    Microseconds(r->a[0], r->d[0]);
    break;
```

`BasiliskII/tests/basilisk/basilisk_emulop_test.cpp` expects A0/D0 to change
and **must not** write through the pointer in A0.

### What this is not

- Not a 32-bit addressing / `Host_Mem_Base` hole bug (phase 2 `2dbbab9` booted).
- Not W^X / icache (`3d1c488` booted).
- Not `mem_strategy` (`301aba2` booted).
- Not SCSI Complete DMA overlapping the RAM thunk at `0x00162790` (visible in
  traces, but the Type 10 bisect did not land there).
- Not XPRAM `$8A` 32-bit-clean bits (CLKNOMEM already forces them on that
  path; direct `READ_XPRAM` was a red herring for this bomb).

---

## 2. Type 4 on UAE interpreter — TimeDBRA calibration `DIVU.W D5,D1`

### Symptom

Musashi, UAE+JIT, and both m68k-rs paths reach WLSC and continue. **UAE with
`jit false`** often dies *after* WLSC:

```
[SYSTEM-ERROR] uae: 68k exception vector=5 (Mac bomb Type 4: Zero Divide)
opcode=0x82C5 PC=0x001A81C8
D5=00000000 D1=00061A80
TimeDBRA=0x0064
```

Flaky: 4/7 boots in ~28s in one series; when it fires, PC/opcode/D5 are
identical. UAE JIT (`jit true`, `jitfpu true`) did not hit it in the same
window.

### Guest code (RAM, ~`0x001A8148`)

A loaded driver (seen next to `ltlk` / SCSI resource patches) calibrates Time
Manager against `TimeDBRA` (`0x0d00`, faked to 100 by SetupTimeK / VIA-less
ROM patches):

1. `D5 = 0x0000FFFF` (unsigned min seed), `D7 = 0x03938700` (60e6 µs).
2. `InsTime` / `PrimeTime(-D7)` / immediate `RmvTime`; `D7 += tmCount`
   → trap **overhead** in Mac microseconds.
3. Loop: `PrimeTime(-D6)` / **four** `MOVE.W TimeDBRA,D0; DBF D0,*` /
   `RmvTime`; `D6 += tmCount; D6 -= D7` (elapsed − overhead);
   `D5 = min(D5, D6)`.
4. `MOVE.W TimeDBRA,D1` / `MULU.W #4000,D1` / **`DIVU.W D5,D1`**.

`D1=0x00061A80` is `100 * 4000`. `DIVU.W` uses the **low 16 bits of D5**.
If elapsed − overhead is 0, `D5` becomes 0 → vector 5.

UAE reports PC *after* `DIVU` unless the exception hook uses
`regs.instruction_pc` (the faulting opcode is two bytes earlier).

### Root cause

`PrimeTime` / `RmvTime` use host `clock_gettime` / `gettimeofday`. UAE's
interpreter finishes ~400 empty `DBF`s in the **same host microsecond** as
the overhead-only Prime/Rmv pair. Remaining equals the full delay, so
elapsed − overhead is 0.

Musashi, UAE JIT, and m68k-rs spend ≥1 host µs on those DBFs, so they
usually skip this.

Interrupts are masked (`ORI #$0700,SR`) around the measurement; 60 Hz does
not advance the TM task. Remaining is purely host clock vs wakeup.

### Fix (do this; do not inflate TimeDBRA)

`RmvTime` adds the engine's **emulated nanoseconds** between `PrimeTime` and
`RmvTime` to the wall-clock sample before computing remaining.

- `CPUEngine.emulated_ns` — optional. Musashi / m68k-rs leave it `NULL`
  (wall clock only).
- UAE: `amiberry_cpu_emulated_ns()` maps `currcycle` as a 40 MHz 68040
  (`(currcycle * 25) / CYCLE_UNIT`). ~400 interpreter DBFs become tens of
  Mac µs, so elapsed − overhead is never 0.
- `TimerInterrupt()` still expires tasks on **wall clock** only, so 60 Hz
  tasks do not fire early.

Files: `BasiliskII/timer.cpp`, `BasiliskII/cpu_engine.cpp`,
`BasiliskII/include/cpu_engine.h`, `BasiliskII/amiberry/hosted/amiberry_host.cpp`,
`BasiliskII/amiberry/amiberry_glue.cpp`.

### Fixes that failed (do not retry)

| Attempt | Why it failed |
|---------|----------------|
| Floor `RmvTime` remaining by 1 µs whenever host elapsed was 0 | Overhead pair and spin pair both got the same 1 µs → difference still 0; Type 4 became **7/7**. |
| Raise `TimeDBRA` (and friends) from 100 to 8192 | First `DIVU.W D5,D1` survived; a later `DIVU.W D1,D2` at `0x001A68FC` died **7/7** with `D1=0` (calibration quotient collapsed). |
| Blame SCSI DMA vs thunk `0x00162790` | Overlap logs are real; bisect showed they did not introduce Type 10, and Type 4 is register `D5` after TM math. |
| Reject writes of `0x00065AAx` into `0x0016279A` | Symptom suppression; the Type 10 producer was `A0` after Microseconds. |

---

## 3. Diagnostics worth keeping

On vector 2–8 and 11, `cockatrice_report_cpu_exception()` logs PC, optional
D/A snapshot, and bytes around the fault. Vector 5 also dumps `TimeDBRA` /
`TimeSCCDBRA` / `TimeSCSIDBRA` / `TimeRAMDBRA` and a wider code window.

UAE fills the snapshot in `ExceptionX` via `cockatrice_set_cpu_exception_context()`
(`BasiliskII/amiberry/src/newcpu.cpp`), using `regs.instruction_pc` because
`DIVU` increments PC before `Exception_cpu(5)`.

`dump_memory true` / `dump_file /tmp/memory.bin` writes a `CKDUMP1` header plus
guest RAM on the first reportable exception.

`cpu_engine_note_pc_trace()` is a 4096-entry PC ring for the first fault. No
engine hook calls it yet; dumps stay empty until one is wired.

`[BOOT] HasMacStarted: warm-start flag WLSC is set` is a one-shot boot milestone.

---

## 4. How to verify

Working prefs (launch from `BasiliskII/OSX64/`):

```
cpu_emulator uae   # also musashi, m68k_rs
jit false          # and jit true / jitfpu true for UAE
rom Quadra800.rom
scsi0 <System 8.1 HDA>
cpu 4
fpu true
modelid 29
ramsize 67108864
```

Expect WLSC and **no** `[SYSTEM-ERROR] vector=5` and **no** `[CPU-LINEF]` at
`0x65AAx`. UAE interpreter: 7/7 clean through the old Type 4 site after the
cycle-credit fix (previously 4/7 Type 4).

Engine matrix after the Microseconds ABI restore: Musashi, UAE±JIT, m68k-rs±JIT
all reached WLSC. Only UAE interpreter then hit Type 4 until the Time Manager
change.
