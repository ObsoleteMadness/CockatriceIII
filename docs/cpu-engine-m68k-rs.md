# m68k-rs CPU Engine

CockatriceIII can use [m68k-rs](https://github.com/benletchford/m68k-rs) as a fifth
680x0 interpreter via the `m68k_rs` engine id.

## Build requirements

- Rust **1.93+** (see `BasiliskII/vendor/m68k-rs/rust-toolchain.toml`)
- `cargo` on `PATH`

The macOS app Makefile builds `libcockatrice_m68k_rs.a` automatically:

```bash
cd BasiliskII/OSX64
make
```

## Configuration

In your prefs file:

```
cpu_emulator m68k_rs
```

JIT prefs are ignored for this engine (interpreter only).

## Architecture

```
m68k_rs_glue.cpp  →  cockatrice_m68k_rs (Rust staticlib)  →  vendor/m68k-rs
       ↓
ReadMacInt* / EmulOp / cpu_engine_intlev()
```

Trap mapping:

| Basilisk | m68k-rs path |
|----------|----------------|
| EmulOp `0x7100`–`0x713F` | `CycleBatchExit::IllegalInstruction` → C `handle_illegal` |
| Execute68kTrap `0xAxxx` | Hardware Line-A (guest ROM toolbox) |
| 60 Hz / SCC IRQ | `run_for_cycles_with_boundary_hook` + `set_irq` |

## Tests

```bash
cd BasiliskII/tests
make cpu_tests
./cpu_tests --engine m68k_rs
./cpu_tests --engine musashi   # baseline (122 checks)
```

As of integration, `m68k_rs` passes **118** checks with **0** failures. Four Musashi
opcode-battery images are skipped for this engine (`abcd.bin`, `sbcd.bin`,
`chk2.bin`, `cmp2.bin`): upstream m68k-rs intentionally models BCD flag semantics
and CHK2/CMP2 bounds compare differently than the legacy Musashi fixtures (see
`vendor/m68k-rs/tests/musashi_tests.rs` `#[ignore]` notes). FPU snippets run via
the same `cpu_fpu.cpp` path as Musashi.

`M68K_EXEC_RETURN` (`0x7100`) ends the current cycle slice immediately after the
host callback, matching Musashi's `m68k_end_timeslice()` so patched `STOP #0x2700`
in opcode-battery images cannot fall through into `TEST_FAIL`.

## Submodule

m68k-rs is vendored at `BasiliskII/vendor/m68k-rs`. Update with:

```bash
git submodule update --init --recursive
```
