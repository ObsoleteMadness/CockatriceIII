# WinUAE hardware-accuracy cputest (vendored)

Toni Wilen's [WinUAE cputest](https://github.com/tonioni/WinUAE/tree/master/cputest)
validates 680x0 opcode behavior. Cockatrice uses two layers:

| Component | Upstream | Role in CI |
|-----------|----------|------------|
| **cputestgen** (`cputest.cpp`) | WinUAE repo root | Hosted generator self-check via `CPU_TESTER` core |
| **Native replay** (`cputest/main.c`) | WinUAE `cputest/` | Runs `.dat` vectors on real m68k (Amiga); vendored for reference |

## Vendoring

Large generated `cpuemu_*_test.cpp` files are not in WinUAE git (build with `gencpu`
and `CPU_TEST=1`). The vendor script fetches WinUAE sources plus Amiberry's
pre-generated gencpu outputs:

```bash
BasiliskII/scripts/vendor-uae-cputest.sh
```

Files land in `vendor/` (gitignored). See `vendor/UPSTREAM.txt` after a fetch.

## Cockatrice integration

`BasiliskII/tests/Makefile` builds `obj/uae_cputest` from the vendored tree,
`hosted/cputest_support_overrides.cpp`, and Amiberry `readcpu` / softfloat.
`cpu_tests --engine uae` runs the smoke preset in `cockatrice_cputest.ini`
(copied to `tests/obj/cputestgen.ini` at build time).

This is **orthogonal** to the Musashi `.bin` opcode battery: cputest exercises
many CCR/EA combinations per mnemonic; the battery runs fixed Musashi binaries
through each engine's `Execute68k` path.
