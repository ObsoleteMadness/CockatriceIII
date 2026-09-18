# Cockatrice III tests

Configure once from the repository root, then drive everything through CTest:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8

ctest --test-dir build -L gate --output-on-failure   # must pass
ctest --test-dir build -L cpu  --output-on-failure   # engine accuracy, reported
ctest --test-dir build --output-on-failure           # everything

./build/BasiliskII/tests/cpu_tests --engine musashi  # fast iteration path
```

## What gates and what does not

`-L gate` is the eleven `basilisk_*` suites. They are required to pass.

`-L cpu` is `cpu_tests`, which is **reported rather than gated**. It currently
has 12 known failures: four opcode fixtures (`abcd`, `sbcd`, `chk2`, `cmp2`)
across each of the three UAE engine configurations. Those are genuine accuracy
gaps in the UAE core, not harness problems — Musashi passes all 136 of its
checks and m68k-rs all 132 of its own against the same fixtures. See
[EMU68_BOOT_PROGRESS.md](../../EMU68_BOOT_PROGRESS.md).

## Layout

- `cpu/` — Musashi opcode battery plus instruction, FPU, exception and
  ROM-snippet tests, run across musashi, uae (interpreter, JIT, JIT+FPU) and
  m68k-rs.
- `basilisk/` — memory, engine registry, EmulOp, ROM patches, resource patches,
  SCSI, SCC, disk images.

Opcode images live in [`../vendor/musashi/test/`](../vendor/musashi/test). Hang-prone
work is isolated by `run_isolated()` at 30 seconds, and CTest caps each suite at
120 seconds so a wedged engine fails rather than hanging CI.

ROM snippets load `dist/Quadra800.rom` (or `QUADRA_ROM`). A missing ROM skips
those tests.

## ROM and resource patches

`basilisk_patches_test` asserts the full `PatchROM()` patch log against
`basilisk/fixtures/quadra800_patches.txt`. A patch whose byte signature stops
matching the ROM, or lands somewhere new, fails as a one-line diff instead of a
boot bomb. After an intentional change, regenerate and **read the diff**:

```
REGEN_PATCH_MANIFEST=1 ./build/BasiliskII/tests/basilisk_patches_test
```

`basilisk_rsrcpatch_test` drives `CheckLoad()` with synthetic resources built
from the Apple ROM source sequences (see
[docs/rom-patches-vs-supermario.md](../../docs/rom-patches-vs-supermario.md)),
plus boundary cases: resources shorter than a signature, signatures with too
little run-up, empty and all-`0xFF` buffers. Every fixture is bracketed with
guard bytes, so an out-of-bounds write is caught without ASan too.

`basilisk_patchguard_test` proves the patch pass fails *loudly*. Each case
corrupts a copy of the ROM so one locator misses, then asserts `PatchROM()`
returns false and ROM offset 0 is untouched — offset 0 being where every failed
locator used to write its patch.

`basilisk_stubabi_test` executes the stubs `PatchROM()` plants and checks their
register contracts against the Apple ROM sources: `Microseconds` returning
A0 = high / D0 = low, the Time Manager wrapper restoring the caller's interrupt
mask, `BlockMove`'s copy and noErr, and the hand-emulated `rtd` in
`SCSIDispatch` removing exactly the selector and arguments. The manifest says a
patch landed at the right offset; this says the bytes there behave. Stub offsets
are read from `GetPatchLog()`, so the two cannot drift apart.

`basilisk_toolbox_test` covers the Toolbox/OS trap trampolines in
[toolbox_traps.cpp](../toolbox_traps.cpp) — the RAM-trampoline mechanism
(`_SetToolTrap` / `_SetOSTrapAddress`) that replaces ROM byte patches for
post-boot traps. It runs every case on all engine configurations, because the
one thing the dispatcher must get right — *which* hooked trap it was entered
for — is derived from the guest PC, and the engines do not agree on what the PC
is at EmulOp time. `--engine <id>` narrows it.

## Sanitizers

```
cmake -S . -B build-asan -DCOCKATRICE_TEST_ASAN=ON
cmake --build build-asan -j8
ctest --test-dir build-asan -L gate --output-on-failure
```

Run the patch tests under ASan after touching either patch file: the failure
mode there is out-of-bounds writes and unsigned-underflow scans, which a plain
build can miss entirely.
