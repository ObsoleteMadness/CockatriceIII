# Cockatrice III — 64-bit Port

This branch ports Cockatrice III (a BasiliskII derivative) from 32-bit-only
builds to native 64-bit builds on macOS (Apple Silicon and Intel) and Windows
(x64 and ARM64), while keeping the existing 32-bit Windows build working.

The historical release notes live in [README](README) / [README.old](README.old).
This file only covers what changed to make 64-bit builds possible.

## Why this was needed

BasiliskII's 68k CPU emulator (UAE) and memory subsystem were written with the
assumption that a host pointer fits in 32 bits. Several places in the codebase
either truncated a pointer into a 32-bit UAE register or typedef'd `uintptr`
to a fixed 32-bit integer. On a 64-bit host that truncation silently drops the
top 32 bits of a real pointer, which reliably crashes or corrupts memory as
soon as the emulator touches Mac RAM/ROM above the low 4GB of address space
(or, in practice, whenever the host allocator hands back a 64-bit address, which
it always does).

## Key changes

- **`uintptr` is now a real pointer-sized integer.**
  [BasiliskII/mingw/sysdeps.h](BasiliskII/mingw/sysdeps.h) previously hardcoded
  `typedef unsigned int uintptr;` (32-bit, always). It now uses
  `typedef uintptr_t uintptr;` from `<stdint.h>`. The new
  [BasiliskII/OSX64/sysdeps.h](BasiliskII/OSX64/sysdeps.h) does the same. This
  is what `uae_cpu/memory.cpp` relies on for `RAMBaseDiff`/`ROMBaseDiff`/
  `FrameBaseDiff` to correctly round-trip a host pointer.

- **Ethernet packet delivery no longer truncates a host pointer into a
  32-bit UAE register.** [BasiliskII/SDL/sdl_pcap.cpp](BasiliskII/SDL/sdl_pcap.cpp)
  used to pass the host packet buffer address as `r.a[0] = (uint32)p + 14`,
  which loses the high bits of the pointer on 64-bit hosts. It now stashes the
  pointer out-of-band via `EtherSetPacketData()` and clears `r.a[0]`; the new
  `ether_packet_data` side channel in [BasiliskII/ether.cpp](BasiliskII/ether.cpp)
  (declared in [BasiliskII/include/ether.h](BasiliskII/include/ether.h)) is
  what `EtherReadPacket()` reads from instead of trusting the 32-bit register.
  [BasiliskII/emul_op.cpp](BasiliskII/emul_op.cpp) was updated to match the new
  `EtherReadPacket` calling convention.

- **Unaligned memory access helpers use compiler builtins instead of
  hand-rolled bit tricks.** `do_get_mem_long`/`do_put_mem_long`/etc. in
  `sysdeps.h` (mingw and OSX64) now use `__builtin_bswap32`/`__builtin_bswap16`
  and are enabled for `__x86_64__`, `__aarch64__`, `_M_X64`, and `_M_ARM64`, in
  addition to the original `__i386__`/`__powerpc__`/`__m68k__` set.

- **Fixed a real logic bug in 64-bit disk positioning.**
  [BasiliskII/disk.cpp](BasiliskII/disk.cpp) combined the high and low 32 bits
  of a 64-bit disk offset with `||` (logical OR) instead of `|` (bitwise OR),
  which is only correct by accident when the low bits happen to be non-zero.
  This is unrelated to the host architecture but was caught while auditing the
  64-bit code paths.

- **Removed other latent 32-bit assumptions:**
  - [BasiliskII/SDL/video_sdl.cpp](BasiliskII/SDL/video_sdl.cpp): `SDLscreen->pixels`
    is a `void*`; pointer arithmetic on it now casts to `uint8*` first (this
    silently "worked" via GCC's non-standard `void*` arithmetic extension, but
    fails to compile with a strict/newer Clang toolchain).
  - [BasiliskII/dummy/audio_dummy.cpp](BasiliskII/dummy/audio_dummy.cpp):
    `audio_sample_rates` changed from `uint32` to `int32` to match the
    signature expected elsewhere.
  - [BasiliskII/dummy/user_strings_dummy.cpp](BasiliskII/dummy/user_strings_dummy.cpp)
    and [BasiliskII/dummy/cdrom_dummy.cpp](BasiliskII/dummy/cdrom_dummy.cpp):
    `const`-correctness and `bool`/`FALSE` cleanups needed by modern
    C++ compilers (Clang/GCC 8+) that reject the old code as ill-formed or
    warn it into a build failure under `-Werror`-adjacent settings.
  - [BasiliskII/SDL/main_sdl.cpp](BasiliskII/SDL/main_sdl.cpp): Windows header
    includes changed from backslash (`SDL\SDL.h`) to forward-slash (`SDL/SDL.h`)
    paths, since MSYS2/MinGW toolchains don't treat `\` as a path separator in
    `#include`. DrMinGW crash-handler integration (`exchndl.h`, `ExcHndlInit()`)
    is now gated to 32-bit x86 (or an explicit `USE_EXCHNDL` define) since
    DrMinGW doesn't support x64/ARM64.
  - [BasiliskII/mingw/config.h](BasiliskII/mingw/config.h): `SIZEOF_VOID_P` /
    `SIZEOF_CHAR_P` are now computed correctly (8 on `_WIN64`/`__x86_64__`/
    `__aarch64__`, 4 otherwise) instead of being hardcoded for 32-bit.

## Directory / build layout changes

- `BasiliskII/OSXarm` (the initial Apple Silicon-only port) was renamed to
  `BasiliskII/OSX64` and its Makefile now builds either Apple Silicon or Intel
  Macs from one tree via an `ARCH` variable (`arm64` by default, or `x86_64`/
  `amd64`), selecting the right Homebrew SDL prefix
  (`/opt/homebrew` vs `/usr/local`) and `-arch` flag automatically.
- `BasiliskII/mingw/Makefile` was rewritten to build Windows x86, x64, or
  ARM64 from the same tree using whatever MSYS2 toolchain/MSYSTEM
  (`MINGW32`/`MINGW64`/`CLANGARM64`) invokes it, using `sdl-config` when
  available instead of hardcoded library paths.

## CI: GitHub Actions

[.github/workflows/build-and-release.yml](.github/workflows/build-and-release.yml)
builds all five targets on every push, on pull requests into `main`, and on
`v*` tags (which also cuts a GitHub Release):

| Target       | Runner          | Build dir             |
|--------------|-----------------|------------------------|
| osx-arm      | macos-latest (native arm64) | `BasiliskII/OSX64` |
| osx-amd64    | macos-latest (cross x86_64) | `BasiliskII/OSX64` |
| win32-x64    | windows-latest (MINGW64)    | `BasiliskII/mingw` |
| win32-x86    | windows-latest (MINGW32)    | `BasiliskII/mingw` |
| win32-arm    | windows-latest (CLANGARM64) | `BasiliskII/mingw` |

Each job packages its build (`.dmg` on macOS, `.zip` on Windows) alongside the
files in [dist/](dist/) and uploads it as a build artifact; on a version tag
the artifacts from all targets are attached to a single GitHub Release.

The workflow triggers on push to **any** branch (not just `main`), so pushing
a feature/port branch like `osx-arm` runs the full build matrix without
needing to merge first.
