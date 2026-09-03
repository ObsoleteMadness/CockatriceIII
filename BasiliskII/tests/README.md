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
- `basilisk/` — memory, engine registry, EmulOp, ROM patches, SCSI, SCC, disk images.

Hang-prone work is isolated with a **30 second** timeout (`run_isolated()` and `run_with_timeout.sh`). Override with `TEST_TIMEOUT`.

ROM snippets load `dist/Quadra800.rom` (or `QUADRA_ROM`). Missing ROM skips those tests.

A full `make test` CPU pass can take a while: each hung engine is isolated at 30s per test/image rather than wedging the suite. `./cpu_tests --engine musashi` is the fast iteration path.

Opcode images stay in `BasiliskII/Musashi/test/`. Native Musashi `make test` in that tree still runs `test_driver` / `test_fpu`.
