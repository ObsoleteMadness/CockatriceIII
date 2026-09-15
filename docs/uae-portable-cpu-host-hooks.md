# uae-portable-cpu: host hooks needed to replace the Amiberry core

Cockatrice III is replacing its hand-edited Amiberry tree
([BasiliskII/amiberry/](../BasiliskII/amiberry/)) with
[uae-portable-cpu](https://github.com/ObsoleteMadness/uae-portable-cpu), which is
vendored as a submodule at [BasiliskII/vendor/uae-portable-cpu](../BasiliskII/vendor/uae-portable-cpu).

uae-portable-cpu has no Amiga or Atari hardware in it. The price is that every
place where Cockatrice used to edit the UAE core directly now needs a
**supported hook**. This document lists those hooks. We found them in three
places:

1. Edits Cockatrice made to the Amiberry sources (search for `cockatrice_`,
   `Basilisk`, `macemu` and `EMULOP` under `BasiliskII/amiberry/src`).
2. The host layer in `BasiliskII/amiberry/hosted/` and `amiberry_glue.cpp`,
   which re-implements Amiga symbols so the core will link.
3. The Musashi changes: `m68kconf.h` callbacks, the `m68kcpu.h` and `m68kmmu.h`
   diffs since `bebd8f4`, and [MUSASHI_FIXES.md](../MUSASHI_FIXES.md).

Each hook below is written as a host-neutral contract. Any emulator that embeds
a 680x0 and runs host code from inside the guest will need the same things:
Basilisk II, SheepShaver-style trap tables, Atari native features, or
test harnesses.

Status is measured against uae-portable-cpu `bb2718a`. The hooks are now implemented
upstream on the `feat-host-hooks` branch. The contracts that shipped are in
[HOST_HOOKS.md](../BasiliskII/vendor/uae-portable-cpu/HOST_HOOKS.md), and the
*Upstream API* column below maps each hook to them.

---

## Summary

| # | Hook | Why a host needs it | `bb2718a` | Upstream API (`feat-host-hooks`) |
|---|------|---------------------|-----------|----------------------------------|
| 1 | Host-trap opcodes | EmulOps `0x7100–0x713F` call into C++ | Missing | `illegal` + `aline` hooks, `uae_cpu_reserve_opcodes()` |
| 2 | End the timeslice from inside a hook | `M68K_EXEC_RETURN` must leave the execute call | Present | `uae_cpu_end_timeslice()` (innermost call only) |
| 3 | Re-entrant nested execution | `Execute68k` / `Execute68kTrap` run inside an EmulOp | Partial | Re-entrant `uae_cpu_execute()`, `uae_cpu_execute_depth()` |
| 4 | Interrupt level, thread-safe | 60 Hz tick thread raises IRQs | Partial | `get_irq` pull hook, `uae_cpu_signal_irq()`, thread-safe `uae_cpu_set_irq()` |
| 5 | 64-bit emulated clock | Time Manager calibration | Partial | `uae_cpu_get_cycles()` (uint64). JIT countdown fold waits for the JIT |
| 6 | Tight delay-loop (`DBF Dn,*-2`) hook | TimeDBRA Type 4 bomb | Missing | `dbf_spin` hook, emitted by `gencpu` into the fast tables |
| 7 | Line-F / MMU-less coprocessor policy | 68040 Mac code runs `PFLUSH`/`PLPA`/`F0xx` | Partial | `fline` hook with `FPU_ABSENT` / `MMU_ABSENT` / `UNKNOWN` |
| 8 | Exception observer | "Did Mac OS just bomb" report | Partial | `exception` observer; TRAP hook is now `TRAP #0–15` and consumable |
| 9 | Guest bus fault from host memory access | Unmapped holes → vector 2 | Missing | `uae_cpu_raise_bus_error()`, `unmapped_bus_error` config |
| 10 | Region-typed memory map | Direct JIT memory, ROM, SCC MMIO | Partial | `UAE_MEM_JIT_DIRECT`, `UAE_MEM_JIT_UNSAFE_BURST`, `uae_cpu_get_mem_flags()` |
| 11 | Code-cache invalidation | `CheckLoad` / `BlockMove` / ROM patches | Missing | `uae_cpu_invalidate_code()` (no-op until a JIT is built) |
| 12 | Per-instruction hook | PC heartbeat / crash trace | Present | Unchanged; interpreter only |
| 13 | Symbol isolation | Link next to Musashi and Basilisk globals | Missing | `UAE_CPU_MUSASHI_API=OFF`, `UAE_CPU_ISOLATE_SYMBOLS=ON` → `uaecpu_isolated` |

Hooks 1, 3, 5, 6 and 11 each have a JIT half. The current `CMakeLists.txt`
does not compile anything under `src/cpu/jit/`. JIT parity with the Amiberry
engine (`jit true`) means landing those JIT halves at the same time the JIT
sources join the build.

---

## 1. Host-trap opcodes

**What Cockatrice needs.** Basilisk II patches the Mac ROM with EmulOps. These
are `0x71xx` words: MOVEQ with bit 8 set, which is not a valid 680x0 encoding.
The CPU must hand them to the host *before* it applies any other illegal-opcode
policy. `0x7100` (`M68K_EXEC_RETURN`) ends a nested `Execute68k`. `0x7101–0x713F`
dispatch to `EmulOp()` with the full register set.

**Evidence.**
- Musashi: `M68K_ILLG_HAS_CALLBACK = M68K_OPT_SPECIFY_HANDLER` →
  `musashi_illg_callback()` in [musashi_glue.cpp](../BasiliskII/Musashi/musashi_glue.cpp).
- Amiberry:
  - `op_illg` calls `cockatrice_uae_illg()` first
    ([newcpu.cpp:4001](../BasiliskII/amiberry/src/newcpu.cpp#L4001)).
  - Separately, `build_cpufunctbl` installs dedicated `op_emulop_1` and
    `op_emulop_return_1` handlers for the whole range, before the JIT swaps
    tables ([newcpu.cpp:2081](../BasiliskII/amiberry/src/newcpu.cpp#L2081)).
  - Both JIT backends mark the range `fl_end_block`
    ([compemu_support_arm.cpp:3408](../BasiliskII/amiberry/src/jit/arm/compemu_support_arm.cpp#L3408),
    [compemu_support_x86.cpp:5339](../BasiliskII/amiberry/src/jit/x86/compemu_support_x86.cpp#L5339)).
  - `op_emulop_1` hard-flushes the JIT cache after the host call, because the
    handler may have run nested guest code and changed registers behind the
    compiled block's back
    ([newcpu.cpp:1870](../BasiliskII/amiberry/src/newcpu.cpp#L1870)).
- m68k-rs: `M68kRsHostCallbacks::handle_illegal`.

**uae-portable-cpu today.**
- [musashi_api.c](../BasiliskII/vendor/uae-portable-cpu/src/api/musashi_api.c)
  stores `s_illg_cb` and never calls it.
- [newcpu.c `op_illg`](../BasiliskII/vendor/uae-portable-cpu/src/cpu/newcpu.c#L4041)
  offers nothing to the host, so every EmulOp takes vector 4. (Amiberry's Amiga
  checks, including the `cloanto_rom` MOVEQ shortcut, are compiled out here
  because the build defines `WINUAE_FOR_HATARI`.)

**Proposed contract.**
- `int illegal_hook(void *ud, uint16_t opcode, uint32_t pc)` is called first in
  `op_illg` and in any dedicated handler.
- Before the call, flags are materialised (`MakeSR`) and `pc` is the address of
  the opcode word.
- Return 1 = handled. The core resumes at `pc + 2` unless the hook set the PC
  itself, then refills prefetch.
- Return 0 = continue with normal exception processing.
- `uae_cpu_reserve_opcodes(cpu, first, last)` routes a range to the hook with
  no decode. Under JIT the range always ends a block and is never compiled.
- After a handled reserved opcode, the JIT must re-sync its register cache.
  Amiberry does a full flush; a cheaper "reload regs" exit is fine.
- The Musashi-compat API should wire `m68k_set_illg_instr_callback` to this hook.

## 2. End the timeslice from inside a hook

**What Cockatrice needs.** `M68K_EXEC_RETURN` sets the return flag
(`TriggerExecutionReturn`). The CPU must then leave the *innermost* execute call
after the current instruction and nothing more.

**Evidence.**
- Musashi: `m68k_end_timeslice()` in `musashi_illg_callback`.
- Amiberry needed three separate edits:
  - `SPCFLAG_BRK` returns only when nested
    ([newcpu.cpp:5110](../BasiliskII/amiberry/src/newcpu.cpp#L5110)).
  - `SPCFLAG_MODE_CHANGE` is used at top level
    ([amiberry_glue.cpp:56](../BasiliskII/amiberry/amiberry_glue.cpp#L56)).
  - The host deliberately avoids MODE_CHANGE while nested, so the outer JIT loop
    is not torn down
    ([amiberry_host.cpp:988](../BasiliskII/amiberry/hosted/amiberry_host.cpp#L988)).

**uae-portable-cpu today.** `m68k_end_timeslice()` sets `SPCFLAG_BRK`, and
`do_specialties` returns 1 on it. That is correct for the interpreter loop in
`m68k_execute`.

**Proposed contract.**
- `uae_cpu_end_timeslice()` may be called from any hook, including memory
  callbacks.
- It affects only the innermost active execute call.
- Compiled JIT code checks it at the end of every block.
- Document that the debugger's use of `SPCFLAG_BRK` must not share the flag.

## 3. Re-entrant nested execution

**What Cockatrice needs.** `Execute68k(addr, regs)` and `Execute68kTrap(trap, regs)`
run guest code to completion from *inside* an EmulOp handler. That handler is
itself running inside `m68k_execute`. The shared helpers
`cpu_engine_write_trap_stub` and `cpu_engine_write_exec_return_frame` build the
stack frame; the engine just has to run it.

**Evidence.**
- Musashi: calls `m68k_execute(5000)` in a loop from inside the illegal callback
  ([musashi_glue.cpp](../BasiliskII/Musashi/musashi_glue.cpp)).
- Amiberry:
  - Added `m68k_run_interpreter_slice()` so nested calls never re-enter the JIT
    push-all trampoline
    ([newcpu.cpp:7146](../BasiliskII/amiberry/src/newcpu.cpp#L7146)).
  - Added a depth counter and a nested-quit flag
    ([amiberry_host.cpp:967](../BasiliskII/amiberry/hosted/amiberry_host.cpp#L967)).
  - Added `m68k_prepare()`, because Amiberry only builds its function tables
    inside `m68k_go()`, which a host that owns the loop never calls
    ([newcpu.cpp:2402](../BasiliskII/amiberry/src/newcpu.cpp#L2402)).

**uae-portable-cpu today.**
- `m68k_execute` measures its budget from `currcycle` with locals, so it can be
  nested in the interpreter.
- It does not track depth.
- Once the JIT is built, nothing stops a nested call from entering compiled code.

**Proposed contract.**
- `m68k_execute` / `uae_cpu_execute` are re-entrant from any hook.
- A nested call always runs in the interpreter, whatever the JIT setting.
- `uae_cpu_execute_depth()` is exposed.
- `end_timeslice` (hook 2) is scoped to the current depth.
- Initialisation (`m68k_init` + `m68k_set_cpu_type` + `m68k_pulse_reset`) fully
  prepares the core, with no hidden `m68k_go` step.

## 4. Interrupt level

**What Cockatrice needs.** Level 1 (VIA / 60 Hz), 2 or 4 (SCC) and 7 (NMI).
`TriggerInterrupt()` is called from the SDL tick thread while the CPU thread is
running.

**Evidence.**
- Musashi: `m68k_set_irq(cpu_engine_intlev())`.
- Amiberry: sets `SPCFLAG_INT`, and the core then *pulls* the level through
  `intlev()`, which Basilisk exports
  ([cpu_engine.cpp:932](../BasiliskII/cpu_engine.cpp#L932)).
- m68k-rs: `get_irq` pull callback.

**uae-portable-cpu today.** `m68k_set_irq` writes `pending_irq_level` and ORs
`regs.spcflags`. Neither write is atomic. `intlev()` is a global symbol that
clashes with Basilisk's (see hook 13).

**Proposed contract.**
- `uae_cpu_set_irq()` is safe to call from another thread: an atomic level store
  plus an atomic spcflags OR.
- Optionally, `int get_irq(void *ud)` is polled whenever `SPCFLAG_INT` is set.
- The level is level-triggered: 0 clears it.

## 5. Emulated clock

**What Cockatrice needs.** `CPUEngine::emulated_ns` is a monotonic count of
emulated work. Time Manager adds the `PrimeTime`→`RmvTime` delta, so a
calibration spin that finishes within one host microsecond still counts as
elapsed Mac time. See [quadra-32bit-boot-crashes.md](quadra-32bit-boot-crashes.md).

**Evidence.** In [amiberry_host.cpp](../BasiliskII/amiberry/hosted/amiberry_host.cpp):
- `do_cycles_slow` folds the JIT `pissoff` countdown into `currcycle` and
  raises MODE_CHANGE once per 40000-cycle host slice (line 178).
- `jit_consumed_cycles` (line 122) and `amiberry_cpu_emulated_ns` (line 1095)
  keep the value monotonic across countdown reloads.
- Without the fold, `RmvTime` saw a *smaller* value than `PrimeTime`.

**uae-portable-cpu today.**
- `m68k_cycles_run()` returns `int` (`currcycle / CYCLE_UNIT`), which overflows
  in long sessions.
- There is no JIT countdown to fold, because the JIT is not built.

**Proposed contract.**
- `uint64_t uae_cpu_get_cycles(cpu)` is monotonic for the life of the instance.
- It includes cycles consumed by compiled blocks that have not yet returned to C.
- `m68k_execute(n)` honours `n` under JIT: the countdown equals the remaining
  budget.

## 6. Tight delay-loop hook

**What Cockatrice needs.** The ROM's TimeDBRA calibration runs `MOVE.W #n,D0;
DBF D0,*-2` between `PrimeTime` and `RmvTime`. A fast interpreter or JIT finishes
the loop so quickly that elapsed time minus overhead is 0. The following
`DIVU.W D5,D1` then raises vector 5 (Mac System Error type 4).

**Evidence.**
- Amiberry interpreter: hand-edited `op_51c8_0_ff` short-circuits `offs == -2`
  into `amiberry_dbf_delay_loop()`, which sets `Dn.W = 0xFFFF` and credits
  `(count + 1) × 10` clocks
  ([cpuemu_0.cpp:21135](../BasiliskII/amiberry/src/cpuemu_0.cpp#L21135),
  [amiberry_host.cpp:234](../BasiliskII/amiberry/hosted/amiberry_host.cpp#L234)).
- ARM JIT: `compile_dbf_tight_delay` emits a call to the same helper
  ([compemu_support_arm.cpp:2436](../BasiliskII/amiberry/src/jit/arm/compemu_support_arm.cpp#L2436)).
- Musashi and m68k-rs are slow enough that they never needed it.

**uae-portable-cpu today.**
- Missing.
- `cpuemu_*.c` files are generated by `gencpu` at build time, so a hand edit like
  Amiberry's would be overwritten. The hook has to live in `gencpu.c`.

**Proposed contract.**
- Opt-in, off by default, and never active in cycle-exact modes:
  `uint32_t dbf_spin_hook(void *ud, int dreg, uint16_t count)`.
- It is called for `DBF Dn,*-2` (displacement −2, condition false) in the
  interpreter and the JIT.
- The host returns the number of cycles to credit.
- The core then finishes the loop with the architecturally correct result:
  `Dn.W = 0xFFFF`, and the PC falls through.

## 7. Line-F / MMU-less coprocessor policy

**What Cockatrice needs.** Basilisk runs 68040 Mac OS with the PMMU patched out
(`InitMMU` NOPs). System 7/8 and extensions still execute
`PFLUSH`/`PTEST`/`PLPA`, `CINV`/`CPUSH` and stray `0xF0xx` words. On this
machine those must be no-ops, not Line-1111 traps (Mac System Error type 10).

**Evidence.**
- Musashi: `cinv`/`cpush` no-ops (MUSASHI_FIXES §2); FPU dispatch widened to
  EC020+ (§4); PMOVE diagnostic in `m68kmmu.h`.
- Amiberry:
  - `op_illg` treats `(opcode & 0xFF00) == 0xF000` as a 2-byte NOP when
    `mmu_model == 0`
    ([newcpu.cpp:4065](../BasiliskII/amiberry/src/newcpu.cpp#L4065)).
  - The no-MMU `mmu_op` NOPs PLPA on 68040 and any unknown F-line (with a cache
    flush) instead of calling `op_illg`
    ([newcpu.cpp:4432](../BasiliskII/amiberry/src/newcpu.cpp#L4432),
    [cpummu.cpp:1841](../BasiliskII/amiberry/src/cpummu.cpp#L1841)).
  - `cockatrice_uae_fline_trap` logs each case
    ([amiberry_host.cpp:878](../BasiliskII/amiberry/hosted/amiberry_host.cpp#L878)).

**uae-portable-cpu today.** The no-MMU
[`mmu_op`](../BasiliskII/vendor/uae-portable-cpu/src/cpu/newcpu.c#L4429)
handles PFLUSH and PTEST, but PLPA only on 68060. Anything else rewinds the PC
and goes through `op_illg` to vector 11. That is correct Motorola behaviour for
real hardware, but wrong for this host.

**Proposed contract.**
- Keep the Motorola-accurate behaviour as the default.
- Add a hook that the core calls before it vectors any F-line or unimplemented
  coprocessor op: `int fline_hook(void *ud, uint16_t opcode, uint32_t pc, int reason)`,
  where `reason` ∈ {`FPU_ABSENT`, `MMU_ABSENT`, `UNKNOWN`}.
- Return 1 = treat as a NOP. The core skips the full instruction length,
  including extension words and EA, not a blind +2.
- Mac policy then lives in Cockatrice: NOP `MMU_ABSENT`, trap the rest.
- Amiberry's blanket `0xF0xx` +2 skip was a Mac-specific shortcut. Do not copy
  it into the core.

## 8. Exception observer

**What Cockatrice needs.** For vectors 2–8 and 11, capture a register snapshot
at the *faulting* instruction and call `cockatrice_report_cpu_exception()`
before the stack frame is pushed. Skip vector 9 (trace) and vector 10 (A-line
Toolbox dispatch, which fires thousands of times per second). See
[cpu_engine.h](../BasiliskII/include/cpu_engine.h).

**Evidence.**
- Musashi: calls added to every `m68ki_exception_*` in `m68kcpu.h` (diff since
  `bebd8f4`).
- Amiberry: `cockatrice_uae_report_exception()` at the top of `ExceptionX`,
  using `regs.instruction_pc`, because DIVU/DIVS have already advanced the PC
  ([newcpu.cpp:3562](../BasiliskII/amiberry/src/newcpu.cpp#L3562)).
- m68k-rs: `take_last_exception_vector()` polled after each slice.

**uae-portable-cpu today.**
- [`exception_debug`](../BasiliskII/vendor/uae-portable-cpu/src/cpu/newcpu.c#L2676)
  calls `g_trap_hook(ud, nr)` from every exception path, with no PC or opcode,
  and ignores the return value.
- The Musashi-compat API binds `m68k_set_trap_instr_callback` to it. Musashi
  defines that callback as "TRAP #n: return 1 if handled", so a host relying on
  that would be told about every exception and could never consume a TRAP.

**Proposed contract.** Split it into two hooks:
- `void exception_hook(void *ud, const uae_cpu_exception_info_t *info)` fires
  for every vector, before the frame is built. It is observe-only.
  `info` carries `vector`, `fault_pc` (`instruction_pc`), `current_pc`, `opcode`,
  `sr`, and a flag saying whether it is an interrupt.
- `int trap_instr_hook(void *ud, int trap_nr)` fires for `TRAP #0–15` only.
  Return 1 = handled (Musashi semantics).

## 9. Guest bus fault from a host memory access

**What Cockatrice needs.** A read or write to an address that is not committed
in the 4 GB `Host_Mem_Base` window must become a 680x0 access fault (vector 2).
It must not become a host SIGSEGV or silent open-bus data.

**Evidence.**
- Amiberry:
  - `memory_put_*` calls `cockatrice_memory_raise_guest_fault()`, which longjmps
    to a `memory_guard_enter()` checkpoint
    ([amiberry_memory.cpp:41](../BasiliskII/amiberry/hosted/amiberry_memory.cpp#L41),
    [memory.cpp](../BasiliskII/memory.cpp)).
  - After a SIGSEGV longjmp, `amiberry_cpu_bus_error()` calls `Exception(2)`
    ([amiberry_host.cpp:955](../BasiliskII/amiberry/hosted/amiberry_host.cpp#L955)).
  - `get_diword` and friends were edited to fetch through `get_word()` when
    `canbang` is false, so extension words in holes fault instead of
    dereferencing `pc_p`
    ([newcpu.h:473](../BasiliskII/amiberry/src/include/newcpu.h#L473)).
- Musashi: banked callbacks with guards (MUSASHI_FIXES §3).

**uae-portable-cpu today.**
- `get_diword` already falls back to `get_word()` when `regs.pc_p == NULL`. Good.
- In Musashi mode, unmapped slots forward to `m68k_read_memory_*`, which has no
  way to signal a fault.
- `dummy_check` returns 0, but nothing turns that into an exception.
- `m68k_pulse_bus_error()` calls `BusError68000(0,0,0)` → `Exception(2)` with no
  address and no instruction abort. Calling it from inside a callback would
  continue the faulting instruction afterwards.

**Proposed contract.**
- `uae_cpu_raise_bus_error(cpu, addr, is_write, size)` may be called from inside
  any memory callback, and from any hook.
- It aborts the current instruction using the core's own `hardware_exception2` /
  `exception2` path, and builds the correct frame for the CPU model: 68000
  group 0, 68020/030 format `$B`, 68040 format `$7`.
- The Musashi-compat API should let `m68k_pulse_bus_error()` behave the same way
  when it is called during an access.
- Add a config option, `unmapped_access` = `OPEN_BUS` | `BUS_ERROR` | `CALLBACK`.

## 10. Region-typed memory map

**What Cockatrice needs.** Every region has a distinct policy:

| Region | Reads | Writes | JIT |
|--------|-------|--------|-----|
| RAM | direct | direct | direct |
| NuBus framebuffer | direct | direct | direct |
| ROM | direct | dropped | indirect |
| SCC windows (`0x90xxxx`, `0xB0xxxx`, `0x5000xxxx`) | callback | callback | indirect |
| Everything else | bus error (hook 9) | bus error (hook 9) | indirect |

The JIT also needs to know where ROM is (for `isinrom()` trap demotion), and
must never compile direct `LDR`/`STR` against IO or hole addresses.

**Evidence.** In Amiberry:
- `amiberry_init_mac_banks` builds four addrbanks with explicit `jit_read_flag`,
  `jit_write_flag` and `ABFLAG_*` values
  ([amiberry_host.cpp:556](../BasiliskII/amiberry/hosted/amiberry_host.cpp#L556)).
- It repoints `kickmem_bank` at Mac ROM so `isinrom()` works
  ([compemu_support_arm.cpp:2051](../BasiliskII/amiberry/src/jit/arm/compemu_support_arm.cpp#L2051)).
- It forces `jit_n_addr_bank_unsafe = 1` so MOVEM/MOVE16 bursts stay on helpers
  ([amiberry_host.cpp:752](../BasiliskII/amiberry/hosted/amiberry_host.cpp#L752)).
- `sysconfig.h` renames `dummy_bank` and `mem_banks` to avoid colliding with
  Basilisk.
- See also [jit-memory-architecture.md](../BasiliskII/docs/jit-memory-architecture.md).

**uae-portable-cpu today.**
- `memory_map_ptr(start, size, host_ptr, UAE_MEM_RAM|ROM|CACHEABLE)` exists, and
  ROM writes are dropped (`ram_lput` checks `ABFLAG_ROM`).
- `memory_map_custom` covers IO.
- There are no JIT direct/indirect flags per region.
- There is no ROM-range registration for the JIT.
- Mapping resolution is 64 KB banks, which matches Basilisk.

**Proposed contract.**
- Add flags `UAE_MEM_JIT_DIRECT` (safe for inlined host-pointer access) and
  `UAE_MEM_JIT_UNSAFE_BURST` (disable native MOVEM/MOVE16 into this region).
- The JIT derives "is ROM" from `UAE_MEM_ROM` regions instead of Amiga
  `kickmem_bank`.
- The ROM write policy is selectable: drop or callback.
- Guarantee in the docs that `memory_map_*` may be called again after
  `m68k_pulse_reset`; the framebuffer moves on resolution change.

## 11. Code-cache invalidation

**What Cockatrice needs.** Once Basilisk has written guest code (`CheckLoad`,
`BlockMove`, ROM/resource patches), `CPUEngine::invalidate_code(addr, size)`
must discard any translation that overlaps the range. If the current block is
one of them, it must leave compiled code immediately.

**Evidence.** In Amiberry:
- `flush_icache_hard` was changed to set `SPCFLAG_MODE_CHANGE`, because
  otherwise stale native code kept running fill-pattern heap
  ([compemu_support_arm.cpp:3483](../BasiliskII/amiberry/src/jit/arm/compemu_support_arm.cpp#L3483),
  [compemu_support_x86.cpp:5437](../BasiliskII/amiberry/src/jit/x86/compemu_support_x86.cpp#L5437)).
- `flush_icache_range` was added for ARM, and exported as a full flush on x86
  ([compemu_support_x86.cpp:5537](../BasiliskII/amiberry/src/jit/x86/compemu_support_x86.cpp#L5537)).
- `comp_hardflush = false`, so 68040 `CINVA`/`CPUSHx` issued for DMA coherency
  do not force constant recompiles
  ([amiberry_host.cpp:752](../BasiliskII/amiberry/hosted/amiberry_host.cpp#L752)).

**uae-portable-cpu today (`caa5e73`).**
- `uae_cpu_invalidate_code()` exists, but ignores the range: it flushes the
  whole cache and sets `SPCFLAG_END_COMPILE`
  ([host_hooks.c:606](../BasiliskII/vendor/uae-portable-cpu/src/cpu/host_hooks.c#L606)).
- The ARM backend has no `flush_icache_range` at all; the x86 one is `static`
  and unreachable
  ([compemu_support_x86.cpp:5499](../BasiliskII/vendor/uae-portable-cpu/src/cpu/jit/x86/compemu_support_x86.cpp#L5499)).
- See *JIT performance findings* below: a whole-cache flush per code write is
  what made the Amiberry engine spend most of its time recompiling.

**Proposed contract.**
- `uae_cpu_invalidate_code(cpu, addr, size)`: `size == ~0` flushes everything.
- It is safe to call from hooks and between slices.
- If an invalidated block is executing, the core exits to the dispatcher after
  the current instruction.
- It is a no-op when the JIT is off.
- Expose the guest-visible `CINV`/`CPUSH` flush mode (hard or lazy) as config.

## 12. Per-instruction hook

**What Cockatrice needs.** `cpu_engine_note_pc_trace()` feeds a ring buffer
that is dumped on the first fatal exception.

**Evidence.** Musashi `M68K_INSTRUCTION_HOOK`; m68k-rs `boundary_hook`; Amiberry
samples once per slice via `cpu_engine_note_pc`.

**uae-portable-cpu today.** `g_instr_hook` is called from the `m68k_execute`
and `uae_cpu_execute` loops, so only the interpreter reaches it.

**Proposed contract.** Keep it interpreter-only, and document that under JIT it
fires at block entry at most. Leaving it `NULL` must cost nothing on the hot
path.

## 13. Symbol isolation

**What Cockatrice needs.** The Musashi engine is always linked, because it is
the golden reference. Basilisk's own globals live in the same binary.

**Collisions found.**

| uae-portable-cpu symbol | Already defined by |
|-------------------------|--------------------|
| `intlev()` (`uae_glue.c`) | [cpu_engine.cpp:932](../BasiliskII/cpu_engine.cpp#L932) |
| `memory_init()` (`memory.c`) | [memory.cpp:675](../BasiliskII/memory.cpp#L675) |
| `m68k_init`, `m68k_execute`, `m68k_set_reg`, … (`musashi_api.c`) | Musashi |
| `m68k_read_memory_*` (weak/alternatename defaults) | [memory_musashi.cpp](../BasiliskII/Musashi/memory_musashi.cpp) |
| `regs`, `currprefs`, `mem_banks`, `dummy_bank`, `write_log`, `currcycle` | Amiberry, while both trees coexist during migration |

Amiberry dodged these with `#define` renames in
[sysconfig.h](../BasiliskII/amiberry/sysconfig.h), plus the
`amiberry_cpu_api.h` firewall so Basilisk and UAE `sysdeps.h` never meet.

**Proposed contract.**
- A CMake option `UAE_CPU_MUSASHI_API=OFF` leaves out `musashi_api.c`.
  Cockatrice uses `uae_cpu.h`.
- A `UAE_CPU_SYMBOL_PREFIX` (or `-fvisibility=hidden` with only `uae_cpu_*`
  exported from a shared or `-r` linked object) keeps internal symbols out of
  the host namespace.
- Hooks that are currently global (`g_*_hook`) move onto `uae_cpu_t`. The
  "multi-instance" context API is really single-instance until this is done.
  (Not done upstream yet.)

---

## What shipped upstream

The full API is in `include/uae_cpu.h` and
[HOST_HOOKS.md](../BasiliskII/vendor/uae-portable-cpu/HOST_HOOKS.md). It differs
from the proposal above in these ways:

- **Line-A has its own hook (`aline`).** Musashi's illegal callback and
  m68k-rs's `handle_illegal`/`handle_aline` split them the same way.
- **One resume rule for `illegal`, `aline` and `fline`.** When a hook handles
  the word and leaves the PC where it was, execution resumes after the
  opcode word; if the hook moved the PC, execution continues there.
  Multi-word FPU/PMMU skips must set the PC.
- **`dbf_spin` is offered on every iteration.** Returning 0 declines (runs that
  iteration); a non-zero return completes the loop and credits that many
  cycles.
- **The TRAP hook receives `trap_nr` 0–15.** Returning non-zero services the
  TRAP without an exception.
- **Hooks are still process-global.** The core is single-instance; moving them
  onto `uae_cpu_t` was not done.
- **Symbol isolation is a relocatable link, not a prefix header.** It works
  with Apple ld or GNU ld + objcopy, not MSVC. The core is compiled with
  `-fno-common` so data symbols can be hidden too.
- **JIT halves of hooks 1, 3, 5, 6, 10 and 11 wait for the JIT sources to join
  the build.** The public entry points already exist so Cockatrice can call
  them unconditionally.

Verified with `ctest` on macOS arm64 and Ubuntu 24.04: default build 7/7, and
isolated with the Musashi API off 3/3. Musashi and m68k-rs suite pass counts
are unchanged from `bb2718a`.

## JIT build upstream (`feat-jit-build`)

A second upstream branch, stacked on `feat-host-hooks`, builds the ARM64 and
x86-64 JIT in uae-portable-cpu. It also adds the JIT halves of hooks 1, 3, 5,
6 and 11. What it means for the Cockatrice engine:

- **Declare the flat window.** Call `uae_cpu_set_jit_memory_base(cpu, Host_Mem_Base)`.
  Translated code derives the 68k PC from host pointers through that base, the
  same assumption Amiberry's `natmem_offset` made. Code outside the window is
  interpreted inside the JIT dispatcher.
- **Translation does not wait for `CACR`.** WinUAE only compiles once the guest
  enables the CPU cache. `jit_follow_cacr = false`, the default, compiles
  immediately. Set it to true to match the old Amiberry engine exactly.
- **Turn on direct memory access.** Set `jit_direct_memory = true` and map RAM,
  ROM and the frame buffer with `UAE_MEM_JIT_DIRECT` at `Host_Mem_Base + address`.
  This is the upstream form of the Amiberry engine's `canbang = true` with
  `comptrust* = 0`. Without it, translated code calls a handler for every
  access. The window must cover the whole 4 GB guest space, which
  `Host_Mem_Base` already does.
  - A region is only accessed inline if its host pointer is
    `Host_Mem_Base + start`; anything else keeps its handler.
  - ROM writes still go through the handler, so ROM write suppression is kept.
  - Map the SCC windows with `uae_cpu_map_custom()`. Accesses profiling saw
    there stay on the handler.
  - An inlined access that later lands in an uncommitted hole: on x86-64 and
    Windows ARM64 the library completes it through the region's handler. On
    arm64 macOS and Linux it reaches Cockatrice's own SIGSEGV/SIGBUS handler,
    as with the Amiberry engine today. The library's handlers pass on anything
    they don't handle.
  - The Amiberry engine forced `jit_n_addr_bank_unsafe = 1` after `MOVEM`
    bursts corrupted memory. Upstream exposes that as
    `UAE_MEM_JIT_UNSAFE_BURST`. Use it if that corruption reappears; on x86-64
    it also turns inline access off.
- **JIT FPU.** `jit_fpu` translates FPU instructions, like the Amiberry
  engine's `jitfpu`. It needs `jit_direct_memory` and the host-double FPU
  backend (`fpu_softfloat = false`), so FPU results are double precision. Two
  bugs make `jitfpu true` unsafe in the current Amiberry engine; both are
  fixed upstream:
  - The engine initialises SoftFloat, but translated FPU code works on host
    doubles. Compiled and interpreted FPU instructions then see different
    register values.
  - Its arm64 JIT does not save the host's callee-saved `d8`–`d15`, which hold
    FP0–FP7. Any C++ code holding a double across a call into the JIT gets it
    corrupted.
- **Windows.** The JIT builds and passes the test suite with MSVC (x64 and
  ARM64) and MinGW-w64, in upstream CI. There is no 32-bit JIT, so the
  win32-x86 build would run the interpreter.
- **Hooks work under the JIT:**
  - EmulOps (reserved opcodes) end blocks and are never compiled natively.
  - Nested `Execute68k` runs on the interpreter.
  - `dbf_spin` routes `DBF Dn` to its C handler while installed, which replaces
    `compile_dbf_tight_delay`.
  - `uae_cpu_invalidate_code()` flushes translated blocks.
- **Bugs found while wiring it up**, each now covered by a test:
  - uae-portable-cpu's `addrbank` put `name` before the accessors, so the JIT's
    fixed-offset helper calls hit the wrong function.
  - `m68k_run_jit`'s `STOPTRY` popped the caller's exception frame in C.
  - `memory_map_ptr()` computed host offsets with `addr & mask`, which breaks
    regions that don't start on a multiple of their size.

Verified on macOS arm64 natively and on x86_64 under Rosetta:
- **Tests:** 16/16 CTest, including JIT, direct-memory and benchmark smoke
  variants.
  - `host_hooks_jit_direct` checks inline access against a C reference, that
    devices and regions outside the window keep their handlers, and the x86-64
    fault recovery path.
  - `host_hooks` checks FPU backend selection and an FPU loop against C
    doubles, with and without `jit_fpu`.
  - `test_uae_cpu` previously checked nothing in Release builds (`NDEBUG`
    removed its asserts). Its checks now stay enabled and pass.
- **Fixtures:** m68k-rs per-fixture results are identical for the interpreter,
  the JIT and the JIT with direct access on the same flat memory map (104/127
  and 25/25). Most fixture code runs once and stays below the JIT's translation
  threshold, so the fixtures mainly check correctness around translated code.
- **Benchmark:** `uae_cpu_bench` (upstream `bench/`) checks every run against a
  C reference. Best of 3, seconds:

  | Workload | arm64 interp | arm64 jit | arm64 jit-direct | arm64 jit-fpu | x86_64 interp | x86_64 jit | x86_64 jit-direct | x86_64 jit-fpu |
  |---|---|---|---|---|---|---|---|---|
  | arith | 0.448 | 0.061 | 0.063 | – | 0.902 | 0.141 | 0.143 | – |
  | bytemix | 0.437 | 0.041 | 0.054 | – | 0.908 | 0.094 | 0.057 | – |
  | memcopy | 0.494 | 0.103 | 0.094 | – | 0.986 | 0.275 | 0.241 | – |
  | device | 0.110 | 0.028 | 0.029 | – | 0.248 | 0.062 | 0.063 | – |
  | fpu | 0.193 | 0.169 | 0.164 | 0.007 | 0.379 | 0.449 | 0.343 | 0.295 |

  Runs vary by roughly 10–25% on this machine. x86_64 ran under Rosetta, so
  compare within one architecture only. Rosetta emulates the x87 instructions
  the x86 FPU JIT uses, so the x86 `jit-fpu` figure says little about native
  x86.

---

## What stays in Cockatrice glue (not core hooks)

These are Basilisk policy, not CPU behaviour. They belong in the new
`CPUEngine` adapter, as they do for Musashi and m68k-rs:

- `M68kRegisters` marshalling around `EmulOp()`, and A7 commit gating
  (`cpu_engine_should_commit_a7`).
- `Execute68k` / `Execute68kTrap` stack frames (`cpu_engine_write_trap_stub`,
  `cpu_engine_write_exec_return_frame`) and restoring the PC afterwards.
- Warm reset via `setjmp`/`longjmp`, and `cpu_engine_reset_peripherals()`.
- The boot SP/PC/SR (`CPU_ENGINE_BOOT_*`) set after `m68k_pulse_reset`.
- JIT settings, as config passed to `uae_cpu_create`: `jit` → `jit_enabled`,
  `jitcachesize` → `jit_cache_size`, `jitfpu` → `jit_fpu`, plus
  `jit_direct_memory`. `jit_fpu` only takes effect with `jit_direct_memory`
  and `fpu_softfloat = false`.
- Low-heap and ROM-header diagnostic dumps (`cockatrice_m68k_low_heap_fault`,
  `cockatrice_uae_fline_trap` body), built on hooks 7 and 8.

## MUSASHI_FIXES.md items that are *not* hook requirements

- Sections 1 and 4–12 are bugs inside Musashi's opcode table and FPU. The
  uae-portable-cpu README records UAE matching Motorola on these (see
  `WALKTHROUGH.md` §4). Re-check them with the Cockatrice opcode battery;
  they are not API work.
- Section 2 (`CINV`/`CPUSH`) is implemented natively in UAE. Only the flush
  mode matters (hook 11).
- Section 3 (banked memory, 24/32-bit mirroring) is covered by hooks 9 and 10.
  Mirroring stays in Basilisk's memory callbacks.
- Section 13 (diagnostics instead of `exit(1)`) is covered by hooks 7 and 8.
- Section 14 (SCSI) is unrelated to the CPU.

## Musashi-compat API gaps noticed along the way

These don't affect Cockatrice if it uses `uae_cpu.h`, but they matter to anyone
using uae-portable-cpu as a drop-in Musashi:

- `m68k_set_bkpt_ack_callback`, `_pc_changed_`, `_tas_instr_` and `_fc_` store
  their callbacks and never call them. (`m68k_set_illg_instr_callback` is now
  wired to the `illegal` hook upstream.)
- `m68k_set_trap_instr_callback` fired for every exception and ignored the
  return value. It is now `TRAP #0–15` only and honours the return value
  upstream.
- `m68k_get_reg(context, M68K_REG_PC/SR)` reads the live CPU, not `context`.
- `m68k_cycles_remaining()` always returns 0, and `m68k_modify_timeslice()`
  is a no-op.

## Migration touchpoints in this repo

These files reference the Amiberry engine and will change when the swap lands:

- **Build:** [OSX64/Makefile](../BasiliskII/OSX64/Makefile),
  [mingw/Makefile](../BasiliskII/mingw/Makefile),
  [tests/Makefile](../BasiliskII/tests/Makefile)
- **Engine registration:** [cpu_engine.cpp](../BasiliskII/cpu_engine.cpp)
  (`ENABLE_AMIBERRY_CPU`, engine id `"uae"`)
- **Tests:** [tests/cpu/cpu_uae_cputest.cpp](../BasiliskII/tests/cpu/cpu_uae_cputest.cpp),
  [scripts/vendor-uae-cputest.sh](../BasiliskII/scripts/vendor-uae-cputest.sh)
- **Memory layout assumption:** the `0x50000000` SCC hole in
  [SDL/main_sdl.cpp:225](../BasiliskII/SDL/main_sdl.cpp#L225)

---

## JIT performance findings (September 2026)

Profiling the running emulator (`sample` on the CPU thread) found it spending
**79% of its time in `compile_block` and almost none in compiled code**. Four
defects, fixed in the Amiberry engine; the measurements are the Speedometer-style
CPU benchmark against a PowerMac 6100/60, Quadra 800 config, macOS arm64:

| Fix | CPU score |
|---|---|
| Before | ~200% |
| Defer icache invalidation to the end of the JIT write window (`2fe1c2b`) | 371% |
| `jitcachesize` 2048 → 16384 (`c6338a2`) | 1497% |
| Precise invalidation instead of a flush after every EmulOp (`11c0444`) | 2053% |

FPU scores 3191% of a 6100/60, so the JIT FPU path is healthy.

**1. An instruction-cache flush per branch patch.** `write_jmp_target()` called
`sys_icache_invalidate` for every 4-byte patch, most of them inside
`compile_block()`'s own write window: 61% of emulation-thread samples. Apple
Silicon denies the thread execute on MAP_JIT pages until the window closes, so
the flushes can be queued and merged, then issued once at the close.

**2. A full cache flush after every host trap.** `op_emulop_1` dropped the whole
translation cache after every EmulOp. Basilisk makes those constantly, so blocks
were re-verified or recompiled continuously. Nothing compiled survives an EmulOp
anyway: the block writes every register back before the call, EmulOps end their
block, and nested `Execute68k` runs on the interpreter.

**3. Invalidation that only matched block starts.** `flush_icache_range()`
compared the written range against each block's *start*, so a write into the
middle of a compiled block was missed. It now matches every checksum range and
redirects just those blocks through `check_checksum`.

**4. 68040 cache instructions never reached the JIT.** Mac OS announces new code
with `CPUSHA`/`CINVA`; those only flushed the emulated hardware cache, and only
pre-68040 `CACR` writes reached the JIT. On a Quadra that signal was lost
entirely, and the blanket per-EmulOp flush was covering for it.

Covered by [basilisk_jit_emulop_test](../BasiliskII/tests/basilisk/basilisk_jit_emulop_test.cpp),
which runs a guest loop through `m68k_run_jit` until it is translated, with a
host call each iteration that nests `Execute68k` and patches an instruction in
the middle of a compiled block.

### What this means for uae-portable-cpu

Checked against `caa5e73`. **No upstream changes have been made; these are
proposals.** Each is host-neutral — no Basilisk, Mac or Cockatrice concept
appears in them — so any embedder that writes guest code (HLE traps,
paravirtual drivers, debuggers, loaders) gets the same benefit.

| # | Finding | Upstream status | Proposal |
|---|---------|-----------------|----------|
| 1 | Per-patch icache flush | **Gap.** Same pattern in `write_jmp_target` ([compemu_midfunc_arm64.cpp:728](../BasiliskII/vendor/uae-portable-cpu/src/cpu/jit/arm/compemu_midfunc_arm64.cpp#L728)); a `jit_write_window_depth` counter already exists | Queue and merge flush ranges while the window is open; issue them in `jit_end_write_window()`. Internal, no API change |
| 2 | Flush per host trap | **Not a gap.** `uae_host_dispatch_trap_opcode()` never flushes, and reserved opcodes end blocks via `uae_host_jit_must_interpret()` | None |
| 3 | Range-scoped invalidation | **Gap.** `uae_host_invalidate_code()` ignores addr/size and flushes everything | Match the written range against each block's checksum ranges and lazily redirect only those blocks. `size == ~0` keeps the full flush. Behaviour only; the API is already right |
| 4 | Guest cache instructions | **Gap.** `flush_cpu_caches_040()` has no JIT path | Treat an instruction-cache `CINVA`/`CPUSHA`/`CINVP` as invalidation of the same scope, behind a config flag (default on) so hosts that keep code and data separate can opt out |

Two further things the library already answers, noted so the port does not
re-invent them:

- **"Is translation actually running?"** `compile_block()` does nothing until
  the guest enables its instruction cache, which cost real debugging time here.
  Upstream defaults `jit_follow_cacr = false` (translate immediately), and
  `uae_cpu_get_jit_code_size()` reports whether anything has been translated.
- **Nested execution.** `uae_host_run()` runs translated code only at
  `g_execute_depth == 1`; nested calls from inside a hook stay on the
  interpreter, which is the model the Amiberry engine arrived at by hand.
