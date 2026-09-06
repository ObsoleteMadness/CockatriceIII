# CPU engine opcode battery fixes (UAE and Emu68)

Reference for agents and humans working on Cockatrice III CPU engines. Documents
the fixes that made the Musashi `.bin` opcode battery pass on **UAE (Amiberry)**
and **Emu68**, as tracked in `BasiliskII/docs/TODO`.

**Test harness**

| Command | What it runs |
|---------|--------------|
| `cd BasiliskII/tests && ./cpu_tests --engine uae` | UAE interpreter + `uae+jit` + `uae+jit+jitfpu` (358 checks) |
| `cd BasiliskII/tests && ./cpu_tests --engine emu68` | Emu68 JIT engine (109 checks) |
| `cd BasiliskII/tests && ./cpu_tests --engine musashi` | Reference baseline (122 checks) |

Opcode images live in `BasiliskII/Musashi/test/`. Each image is loaded at guest
`0x10000`, executed via `Execute68k()`, and checked against `pass_reg` /
`fail_reg` at `0x100004` / `0x100000`. Hang-prone tests run in forked children
with a 30s timeout (`run_isolated()` in `tests/include/test_harness.h`).

---

## Shared test-harness fix (all engines)

Musashi test images end with `STOP #0x2700` on both success and failure paths
(`Musashi/test/entry.s`). The battery used to patch that to `RTS; NOP`.

That breaks the **failure path**: `TEST_FAIL` is reached by branch, not `jsr`.
The patched `RTS` pops the `jsr run_test` return address, falls through to
`mov.l #1, TEST_PASS_REG`, and reports `(pass_reg=1 fail_reg=0)` even when the
test failed.

**Fix:** patch `STOP #0x2700` → `M68K_EXEC_RETURN (0x7100); NOP` in
`tests/cpu/cpu_opcode_battery.cpp`. Every engine already treats `0x7100` as the
`Execute68k` exit hook.

**Regression:** `test_execute68k_test_fail_no_pass_reg()` in
`tests/cpu/cpu_regressions.cpp` — branches to `TEST_FAIL` must not write
`pass_reg`.

---

## UAE (Amiberry) fixes

**Baseline:** 12 opcode-battery failures (abcd, sbcd, chk2, cmp2 × interpreter,
JIT, JIT+FPU). **After fixes:** 358 passed, 0 failed.

### 1. Banked Mac memory: extension-word fetch (`get_di*`)

Cockatrice maps Mac RAM/ROM through a sparse 4 GB host window with `PROT_NONE`
holes. Amiberry is initialized with `canbang = false` in
`amiberry/hosted/amiberry_host.cpp` (no flat NATMEM direct map).

Mode-0 opcode handlers read PC-relative extension words via `get_diword()` /
`get_dibyte()`, which historically dereferenced `regs.pc_p` as a host pointer.
On unmapped guest addresses that raises a host **SIGSEGV** instead of a 680x0
bus error — notably in CHK2/CMP2 PC-relative handlers and other multi-word
instructions.

**Fix:** in `amiberry/src/include/newcpu.h`, when `!canbang`, route
`get_dibyte` / `get_diword` / `get_dilong` / `next_diword` / `next_dilong`
through `get_bytei` / `get_wordi` / `get_longi(m68k_getpc() + offset)` so reads
go through the banked Mac memory layer (`cockatrice_mac_get_*` in
`amiberry/hosted/amiberry_memory.cpp`).

Setting `cpu_compatible = true` was tried and rejected: it breaks `Execute68k`
and EmulOp glue regressions while not fixing the underlying fetch path.

### 2. ABCD / SBCD BCD carry

**Symptom:** cumulative BCD tests (`abcd.bin`, `sbcd.bin`) wrote wrong values
to result registers (e.g. D4/D5).

**ABCD root cause:** carry detection used `(newv & 0x3F0) > 0x90` instead of
Musashi-correct `newv > 0x99` (918 input combinations differ).

**SBCD root cause:** UAE used a different algorithm (nibble borrow tracking via
`bcd` temp and `(… & 0x300) > 0xFF`) that diverged from Musashi on many inputs.
The SBCD handlers were rewritten to match Musashi's sequence: adjust low nibble,
add high nibble, test `newv > 0x99`, then add `0xA0`.

**Fix:** mechanical replace across all `amiberry/src/cpuemu_*.cpp` generated
handlers (interpreter and JIT variants share the same source).

Example (ABCD):

```c
cflg = newv > 0x99;   /* was: (newv & 0x3F0) > 0x90 */
```

Example (SBCD — Musashi-compatible structure):

```c
newv = newv_lo;
if (newv_lo > 9)
    newv -= 6;
newv += newv_hi;
cflg = newv > 0x99;
if (cflg)
    newv += 0xA0;
```

### 3. CHK2 / CMP2 wrapped-range carry

**Symptom:** chk2/cmp2 logical failures after SIGSEGV was fixed; wrapped ranges
(lower > upper) set carry incorrectly.

**Root cause:** out-of-range test for wrapped ranges used **AND** where **OR**
is required:

```c
/* wrong */
if (lower > upper && reg > upper && reg < lower) SET_ALWAYS_CFLG(1);
/* fixed */
if (lower > upper && (reg > upper || reg < lower)) SET_ALWAYS_CFLG(1);
```

**Fix:** replaced in all CHK2/CMP2 handlers in `cpuemu_*.cpp`.

### 4. CHK2 / CMP2 byte bounds and Z/C interaction

**Symptom:** `cmp2.bin` still failed after the wrapped-range fix; byte-size
tests were wrong while word/long paths passed.

**Root causes:**

1. **Byte bound zero-extension** — byte CHK2/CMP2 handlers sign-extended bound
   bytes (`(uae_s8)get_byte(...)`). Musashi compares bounds as unsigned 0–255
   in 32-bit arithmetic. Example: bounds `0xF0`/`0x70` with `reg=0x60` — UAE
   treated lower bound as −16 (in range); Musashi treats it as 240 (out of range,
   C=1).

2. **Carry skipped on Z match** — older handlers used `if/else`: when
   `reg == lower || reg == upper` only Z was set and C was not computed. Musashi
   always computes C. In wrapped ranges, `reg == upper` can still require C=1.

**Fix:** across byte CHK2/CMP2 handlers:

```c
lower = (uae_s32)(uae_u8)get_byte(dsta);
upper = (uae_s32)(uae_u8)get_byte(dsta + 1);
/* ... set Z when reg matches bound, but always run both C tests below ... */
if (lower <= upper && (reg < lower || reg > upper)) SET_ALWAYS_CFLG(1);
if (lower > upper && (reg > upper || reg < lower)) SET_ALWAYS_CFLG(1);
```

### 5. EmulOp / EXEC_RETURN (pre-existing glue, required for harness)

`amiberry/amiberry_glue.cpp` implements `cockatrice_uae_illg()` for Basilisk
`0x71xx` traps. `amiberry/src/newcpu.cpp` calls it from `op_illg()` before
taking a real illegal-instruction exception. `M68K_EXEC_RETURN (0x7100)` triggers
`TriggerExecutionReturn()` so `Execute68k` slices exit cleanly.

### 6. Nested Execute68k must not re-enter JIT

**Symptom:** Segfault at host PC=0 inside `m68k_run_jit` during boot (often
while handling the 60 Hz IRQ EmulOp). Backtrace shows
`m68k_run_jit` → `winuae_execute_68k` → `ADBInterrupt` → `EmulOp` → `op_illg` →
`m68k_run_jit` (re-entrant).

**Root cause:** `winuae_execute_68k` / `winuae_execute_68k_trap` called
`amiberry_cpu_execute_slice()` → `m68k_run()` → `m68k_run_jit` while the outer
CPU thread was already inside the JIT pushall trampoline (dispatching an EmulOp
via `execute_normal` / `op_illg`). Re-entering JIT corrupts the trampoline state
and can tail-call through a NULL handler (PC=0).

**Fix:** route nested `Execute68k` / `Execute68kTrap` through
`amiberry_cpu_execute_interpreter_slice()` → `m68k_run_interpreter_slice()`
(interpreter-only, same as Emu68's Musashi slice). Clear `SPCFLAG_MODE_CHANGE`
after each nested call so `M68K_EXEC_RETURN` does not force the outer JIT slice
to return early.

### 7. F-line MMU ops without MMU (Mac System Error type 10)

**Symptom:** After earlier boot fixes, Mac OS shows **System Error type 10**
(Line 1111 trap) while loading extensions (INIT/PACK/cd* resources in the log).

**Root cause:** `amiberry_cpu_init()` sets `currprefs.mmu_model = 0` for **both**
JIT and interpreter — there is no JIT vs non-JIT MMU split. `FULLMMU` is
compiled (`amiberry/sysconfig.h`) but inactive at runtime. The live handler is
`mmu_op()` in `newcpu.cpp` (the `cpummu.cpp` `#else` stub is dead code when
`FULLMMU` is defined). That handler only implemented **PLPA on 68060**; on
**68040** (Quadra 800) PLPA and other stray cache/MMU F-line ops fell through
to `op_illg()` → vector 11 → Mac type 10.

**Fix:** in `newcpu.cpp` when `mmu_model == 0`:
- `mmu_op()` NOPs PLPA on 68040 and stray `0xF5xx` cache/MMU F-line ops.
- `mmu_op30()` no longer escalates failed fake-PMOVE to `op_illg()`.
- `op_illg()` treats **`0xF0xx` MMU coprocessor** opcodes (e.g. `0xF0FF` at PC
  `0x00065A66` in extension init) as NOPs with correct PC advance, instead of
  vector 11.

`cockatrice_uae_fline_trap()` logs `[F-LINE]` with opcode, PC, SP, JIT/MMU
state, and four guest words at PC (up to 100 lines). Init log prints
`mmu_model` and `compfpu` explicitly.

**JIT note:** `jit true` with default `jitfpu false` leaves `compfpu=false`; FPU
F-line opcodes (`0xF2xx`) can still trap — the log prints a hint to try
`jitfpu true`.

**Stale JIT at extension load (primary type-10 cause with `jit true`):** `[F-LINE]`
at PC `0x00065A66` showed guest words `F0FF F0F0 FF00 F0F0` — fill-pattern
memory, not real 68040 code. The JIT had translated that region before
CheckLoad/BlockMove wrote INIT segments; `cpu_engine_invalidate_code()` and
`FlushCodeCache()` were no-ops on the UAE path (`invalidate_code` was `nullptr`,
`main_sdl.cpp` / `main_unix.cpp` had empty stubs).

**Fix:** wire `amiberry_cpu_engine.invalidate_code` → `amiberry_cpu_invalidate_code()`
→ `flush_icache(3)` when JIT is active; forward `FlushCodeCache()` through
`cpu_engine_invalidate_code()` (same as test stubs).

**EmulOp / prefetch (primary JIT PC desync):** `cockatrice_uae_illg()` handled
Basilisk `0x71xx` traps from `op_illg()` but returned before `fill_prefetch()`.
After many `7104` (CLKNoMem / XPRAM) calls in a JIT block, `regs.pc_p` could
drift from the logical PC; execution then landed in heap fill at `0x00065A66`
(`F0FF F0F0 FF00 …`), raising type 10 on `0xFF00`.

**Fix:** call `fill_prefetch()` when `cockatrice_uae_illg()` consumes an EmulOp;
mark `0x7100..0x713F` as `fl_end_block` in ARM/x86 `build_comp()` so JIT never
merges blocks across host traps; when `mmu_model==0`, skip other stray `0xFxxx`
words with a 2-byte advance instead of vector `0xB`.

**macOS `pagezero_size` (do not use):** [uyjulian/macemu `core_cleanup`](https://github.com/uyjulian/macemu/tree/core_cleanup)
documents `-Wl,-pagezero_size,0x2000` for legacy x86 macOS UAE/ARAnyM JIT (low host
addresses for old load layouts). Cockatrice maps guest RAM at `Host_Mem_Base + offset`
(`memory_init()`), not identity-mapped host page zero. Applying `pagezero_size` on
Darwin arm64 broke launch (SIGKILL / malformed Mach-O before `main`); keep the default
4GB `__PAGEZERO` — see comment in `BasiliskII/OSX64/Makefile`.

### 8. Nested Execute68k must restore outer register file (all engines)

**Symptom:** Mac **System Error type 10** during extension load (`vCheckLoad libi
ID 10`) with Musashi **and** UAE. Musashi stderr: `unknown PMOVE mode 7 (modes
f300) (PC 65a91)`; stdout log: `[CPU-LINEF] Unhandled Line-F Opcode 0xFF03 at
PC=0x00065A95` mid XPRAM (`7104`) loop. Guest PC lands in heap fill (`~0x65A00`),
not real code — **not JIT-specific** (Musashi reproduces it).

**Trigger in log:** `EmulOp 7129` → `EtherIRQ` → `Execute68k(prot->handler)` in
`sdl_pcap.cpp` immediately before `vCheckLoad libi ID 10` and the XPRAM burst.
Same pattern for `ADBInterrupt()` → `Execute68k()` on 60 Hz IRQ.

**Root cause:** `musashi_execute_68k`, `winuae_execute_68k`, `emu68_execute_68k`,
and syn68k equivalents saved only **PC** (or nothing) around nested host JSR
emulation. D0–D7, A0–A7, and SR from the nested handler (Ethernet, ADB, timer,
etc.) remained in the live CPU when the interrupted extension/boot path resumed,
corrupting the next guest jump/call into uninitialized heap.

**Fix:** snapshot full D/A/SR/PC before nested `Execute68k` / `Execute68kTrap`,
copy nested results into the caller's `M68kRegisters *r`, then restore the outer
snapshot into the CPU. Implemented in:
- `Musashi/musashi_glue.cpp`
- `amiberry/amiberry_glue.cpp`
- `Emu68/emu68_glue.cpp`
- `syn68k/syn68k_glue.cpp`

EmulOp writeback (`musashi_illg_callback` / `cockatrice_uae_illg`) still restores
registers from the **EmulOp entry** snapshot; that does not help if guest code
runs between nested `Execute68k` and the next EmulOp, or if A7 commit is gated.

---

## Emu68 fixes

**Baseline:** 72 passed, 33 failed (~7 minute suite time, many 30s timeouts).
**After fixes:** 109 passed, 0 failed (~3 seconds).

### 1. Missing LINE0 `InsnTable` entries

**Symptom:** JIT logged `[JIT] opcode 020a at … not implemented`, illegal
instruction vectored to address 0, infinite loop / timeout.

**Root cause:** Emu68's `InsnTable` in `Emu68/upstream/src/M68k_LINE0.c` had
large gaps for addressing modes common in Musashi tests — especially **An-direct**
and **PC-relative** forms of `ANDI`, `ORI`, `SUBI`, bit ops, etc.

**Fix:** generated and inserted **98 gap ranges** covering **772 opcodes** (octal
array indices, e.g. `[002010 … 002017]` for `ANDI.B #imm, (A0)`). Inference used
neighboring covered entries for `EMIT_*` function and flag metadata.

Regenerate hosted translator output after editing upstream LINE0:

```bash
# from BasiliskII build — prep_translator.py copies upstream → Musashi/emu68_gen
make -C BasiliskII/OSX64   # or your platform makefile
```

### 2. `M68K_EXEC_RETURN (0x7100)` inside JIT blocks

**Symptom:** `jsr`/`stack` stress tests hung or crashed; `test-fail-exec-return`
failed. Execute68k plants `0x7100` on the stack; a large translated block (e.g.
5000× JSR loop) could `RTS` into `0x7100` **inside** the JIT unit without
reaching the run-loop EmulOp dispatcher.

**Fixes (layered):**

| Layer | File | Change |
|-------|------|--------|
| Translation | `Emu68/hosted/prep_translator.py` | In `EmitINSN()`, if `(opcode & 0xff00) == 0x7100`, emit call to `emu68_hosted_emulop()` + `MARKER_STOP` instead of translating as 680x0 |
| Run loop | `Emu68/emu68_glue.cpp` | After each JIT block, re-read PC; if `0x71xx`, dispatch EmulOp (handles stale cached blocks) |
| Execute68k | `Emu68/emu68_glue.cpp` | `emu68_run_execute_slice()` uses Musashi `m68k_execute()` for `Execute68k` / `Execute68kTrap` slices — reliable `illg_callback` for `0x7100` |
| Cache | `Emu68/emu68_glue.cpp` | `emu68_invalidate_code()` → `cache_invalidate_all(ICACHE)` |
| Tests | `tests/include/test_harness.h` | `run_isolated()` child calls `cpu_engine_invalidate_code(0, ~0u)` so forked tests do not inherit stale JIT |

### 3. Hosted translator / debug stubs

| Issue | Fix |
|-------|-----|
| `debug_not_implemented` linked as a function; translators read its address as non-zero flag → every unimplemented opcode executed bare-metal `svc #0x100` → **SIGSYS** on Darwin | Define `int debug_not_implemented = 1;` in `emu68_glue.cpp`; hosted `svc()` in `A64.h` is a no-op |
| Missing includes after prep pass | `prep_translator.py` injects `emu68_hosted.h` / `emu68_darwin_jit.h`; declare `emu68_hosted_emulop()` in `hosted/emu68_hosted.h` |
| `kprintf` swallowed when `#if DEBUG` off | Always-on `kprintf()` in `emu68_glue.cpp` for translator diagnostics |

### 4. What the opcode battery actually validated

Once InsnTable gaps and `0x7100` handling were fixed, the remaining TODO items
(abcd, sbcd, subx, divs/divu, chk2, cmp2, 68040 bitfield/cas, lea/tas, jsr/stack)
**passed without separate per-opcode Emu68 patches** — they were downstream of
missing translations and EXEC_RETURN hangs, not independent Musashi mismatches in
the JIT core.

---

## Files touched (summary)

### UAE

| File | Role |
|------|------|
| `amiberry/src/include/newcpu.h` | Banked `get_di*` / `get_ii*_jit` fetch |
| `amiberry/src/cpuemu_*.cpp` | ABCD/SBCD carry, CHK2/CMP2 range + byte fixes |
| `amiberry/hosted/amiberry_host.cpp` | `canbang = false`, hosted init |
| `amiberry/hosted/amiberry_memory.cpp` | Mac memory → `cockatrice_mac_*` |
| `amiberry/amiberry_glue.cpp` | EmulOp / EXEC_RETURN dispatch; nested Execute68k on interpreter |
| `amiberry/src/newcpu.cpp` | `cockatrice_uae_illg()` in `op_illg()`; `m68k_run_interpreter_slice()` |

### Emu68

| File | Role |
|------|------|
| `Emu68/upstream/src/M68k_LINE0.c` | InsnTable gap fill |
| `Emu68/hosted/prep_translator.py` | EmulOp hook in `EmitINSN()` |
| `Emu68/emu68_glue.cpp` | Execute68k interpreter slice, post-block 0x7100, invalidate, hosted stubs |
| `Emu68/hosted/emu68_hosted.h` | `emu68_hosted_emulop()` declaration |

### Test harness (shared)

| File | Role |
|------|------|
| `tests/cpu/cpu_opcode_battery.cpp` | STOP → `0x7100` patch |
| `tests/cpu/cpu_regressions.cpp` | TEST_FAIL must not set pass_reg |
| `tests/include/test_harness.h` | JIT invalidate in isolated child |
| `scripts/vendor-uae-cputest.sh` | Vendors [WinUAE cputest](https://github.com/tonioni/WinUAE/tree/master/cputest) + gencpu outputs |
| `amiberry/cputest/` | Cockatrice smoke ini, hosted overrides, vendored tree (gitignored) |
| `tests/cpu/cpu_uae_cputest.cpp` | Runs `obj/uae_cputest` (CPU_TESTER generator self-check) |
| `tests/cpu/cpu_syn68k_battery.cpp` | Runs `obj/syn68k_battery` vs golden CRC output |

---

## WinUAE cputest (vendored)

Hardware-accuracy **cputest** is vendored from Toni Wilen’s upstream:

- `WinUAE/cputest/` — native m68k replay harness, `cputestgen.ini`, disassembler tables
- `WinUAE/cputest.cpp` — hosted generator (CPU_TESTER self-check)

Generated `cpuemu_*_test.cpp` files are **not** in WinUAE git; `vendor-uae-cputest.sh`
fetches Amiberry’s pre-built gencpu `CPU_TEST=1` outputs and applies hosted patches
(`nzcv` flags, TCHAR shims via `amiberry/hosted/tchar.h`).

```bash
BasiliskII/scripts/vendor-uae-cputest.sh
make -C BasiliskII/tests uae_cputest syn68k_battery cpu_tests
```

`cpu_tests --engine uae` runs the `CockatriceSmoke` preset; `cpu_tests --engine syn68k`
runs the syn68k native CRC battery (`10000 -notnative`, 15-minute timeout).

**Open:** `uae_cputest` currently SIGSEGV during `CockatriceSmoke` generation (null deref
in generator startup); binary builds and loads ini correctly — runtime debug still needed.

---

## Remaining work (syn68k)

`BasiliskII/docs/TODO` still lists syn68k opcode battery failures and hangs
(abcd, divs, divu, move_usp, move_xxx_flags, sbcd, divs_long; btst, chk2, cmp2,
illegal `0x773F` handler loop). UAE and Emu68 items above are complete.

Known **out of scope** for this opcode pass (still on TODO): UAE FPU accuracy
(Calculator / scroll bars under Mac OS 8), `regs.spcflags` atomicity.
