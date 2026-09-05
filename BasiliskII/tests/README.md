# Cockatrice III tests

Build and run from this directory:

```
make test          # Basilisk must pass; CPU is reported (may fail)
make test-strict   # Fail on CPU failures too
make test-basilisk
make test-cpu
./cpu_tests --engine musashi
```

## Layout

- `cpu/` — Musashi opcode battery plus instruction/FPU/exception/ROM-snippet tests, run on musashi, UAE, and m68k-rs. UAE also runs vendored [WinUAE cputest](../amiberry/cputest/README.md) smoke.
- `basilisk/` — memory, engine registry, EmulOp, ROM patches, resource patches, SCSI, SCC, disk images.

Hang-prone work is isolated with a **30 second** timeout (`run_isolated()` and `run_with_timeout.sh`). Override with `TEST_TIMEOUT`.

ROM snippets load `dist/Quadra800.rom` (or `QUADRA_ROM`). Missing ROM skips those tests.

## ROM and resource patches

`basilisk_patches_test` asserts the full `PatchROM()` patch log against
`basilisk/fixtures/quadra800_patches.txt`. A patch whose byte signature stops
matching the ROM, or lands somewhere new, fails as a one-line diff instead of a
boot bomb. After an intentional change, regenerate and **read the diff**:

```
REGEN_PATCH_MANIFEST=1 ./basilisk_patches_test
```

`basilisk_rsrcpatch_test` drives `CheckLoad()` with synthetic resources built
from the Apple ROM source sequences (see
[docs/rom-patches-vs-supermario.md](../../docs/rom-patches-vs-supermario.md)),
plus boundary cases: resources shorter than a signature, signatures with too
little run-up, empty and all-`0xFF` buffers. Every fixture is bracketed with
guard bytes, so an out-of-bounds write is caught without ASAN too.

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
post-boot traps. It runs every case on all five engine configurations, because
the one thing the dispatcher must get right — *which* hooked trap it was
entered for — is derived from the guest PC, and the engines do not agree on
what the PC is at EmulOp time. `--engine <id>` narrows it.

```
ASAN=1 make test-basilisk      # AddressSanitizer + UBSan
```

Run the patch tests under ASAN after touching either patch file: the failure
mode there is out-of-bounds writes and unsigned-underflow scans, which a plain
build can miss entirely.

A full `make test` CPU pass can take a while: each hung engine is isolated at 30s per test/image rather than wedging the suite. `./cpu_tests --engine musashi` is the fast iteration path.

Opcode images stay in `BasiliskII/Musashi/test/`. Native Musashi `make test` in that tree still runs `test_driver` / `test_fpu`.
