# Cockatrice III — 64-bit

Cockatrice III 64-bit is an experimental port of Cockatrice III to 64-bit Mac, Windows and Linux
hosts. Cockatrice III is a Basilisk II–derived 68k Macintosh emulator.

It runs on AMD64 and ARM64 on each host, and needs a Mac ROM image and a copy of Mac OS (it is
developed against the Quadra 800 ROM running System 7.5–8.1).

## Features

Compared with Cockatrice III:

- **Multiple CPU Options**: choose from Mushashi, UAE and m68k-rs CPU emulators.
- **High-performance JIT** on x86-64 and AArch64 hosts, built on the
  [uae-portable-cpu](https://github.com/ObsoleteMadness/uae-portable-cpu) core, with an optional
  direct-memory mode (`jitdirect`).
- **Dynamic disk assignment**: mount and swap disk images from the Disk menu while the Mac is
  running.
- **LToUDP support**: LocalTalk over UDP on the printer port, compatible with Mini vMac's
  LToUDP.
- **Dynamic video resolution**: change the emulated screen size at run time from the Video
  menu.
- **ROM patch cleanup**: patches are verified before they are applied, fail loudly instead of
  corrupting memory, and use Apple's own install mechanisms where possible.

## Download

Prebuilt binaries for macOS (Apple Silicon, Intel and universal), Windows (x64 and ARM64) and
Linux (x64 and ARM64) are on the
[GitHub Releases](https://github.com/ObsoleteMadness/CockatriceIII/releases) page.

## Building

Cockatrice III is one CMake project on every host. Clone with submodules:

```
git clone --recursive https://github.com/ObsoleteMadness/CockatriceIII.git
cd CockatriceIII
```

Every platform needs CMake, a C/C++17 compiler and SDL 1.2 (`sdl12-compat` is fine). The
optional m68k-rs CPU engine needs Rust 1.93+ (`cargo`); pass `-DCOCKATRICE_ENABLE_M68K_RS=OFF`
to build without it.

### macOS

```
xcode-select --install
brew install cmake sdl12-compat rust

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
cmake --build build --target bundle     # optional: CockatriceIII.app
```

[scripts/build-macos.sh](scripts/build-macos.sh) wraps this for day-to-day work (`--debug`,
`--bundle`, `--run`, …; `--help` lists them). Intel and universal release builds use
[scripts/ci-osx-build.sh](scripts/ci-osx-build.sh); see [docs/ci-builds.md](docs/ci-builds.md).

### Windows

Install [MSYS2](https://www.msys2.org/) and open the **MINGW64** shell (x64) or the
**CLANGARM64** shell (ARM64).

x64:

```
pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake \
    mingw-w64-x86_64-ninja mingw-w64-x86_64-sdl12-compat mingw-w64-x86_64-rust
```

ARM64:

```
pacman -S --needed mingw-w64-clang-aarch64-toolchain mingw-w64-clang-aarch64-cmake \
    mingw-w64-clang-aarch64-ninja mingw-w64-clang-aarch64-sdl12-compat mingw-w64-clang-aarch64-rust
```

Then, in the same shell:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Linux

Debian/Ubuntu:

```
sudo apt-get install cmake ninja-build build-essential libsdl1.2-compat-dev libpcap-dev
# Rust for the m68k-rs engine: https://rustup.rs, or add -DCOCKATRICE_ENABLE_M68K_RS=OFF

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

libpcap is needed for its headers only; the library is loaded at run time for native networking.

### Tests

```
ctest --test-dir build -L gate --output-on-failure   # must pass
ctest --test-dir build -L cpu  --output-on-failure   # CPU engine accuracy, reported
```

See [BasiliskII/tests/README.md](BasiliskII/tests/README.md).

## Configuration

Settings live in a `CockatriceIII_Prefs` text file, found beside the executable, in the macOS
app bundle's `Contents/Resources`, or in the per-user location (defaults are written there if
none exists). The file and ROM in use are printed at startup. A typical setup:

```text
rom Quadra800.rom
modelid 29
cpu 4
fpu true
ramsize 67108864
scsi0 /path/to/System753.hda
scsi6 /path/to/cdimage.iso

cpu_emulator uae      # musashi (default), uae or m68k_rs
jit true              # use the JIT
jitfpu true           # use the FPU JIT
jitdirect true        # uae only: inline RAM/ROM/framebuffer access (fastest)
ltoudp true           # LocalTalk over UDP on the printer port
```

Every option, the file syntax and where the file is searched for are described in
[docs/CockatriceIII_Prefs.md](docs/CockatriceIII_Prefs.md).

## Documentation

- [docs/CockatriceIII_Prefs.md](docs/CockatriceIII_Prefs.md): every configuration option and
  where the prefs file is searched for.
- [docs/64bit-changes.md](docs/64bit-changes.md): what changed in the 64-bit port and the CPU
  engines.
- [docs/ci-builds.md](docs/ci-builds.md): CI matrix, release packaging, macOS bundles and
  universal binaries.
- [docs/rom-patches-vs-supermario.md](docs/rom-patches-vs-supermario.md): how the ROM and
  resource patches work, mapped onto Apple's SuperMario ROM sources.
- [docs/basilisk-ii-boot-and-patch.md](docs/basilisk-ii-boot-and-patch.md): the boot sequence
  and EmulOps.
- [docs/uae-portable-cpu-host-hooks.md](docs/uae-portable-cpu-host-hooks.md): how the uae
  engine and its JIT are wired in.
- [docs/cpu-engine-m68k-rs.md](docs/cpu-engine-m68k-rs.md) and
  [docs/cpu-engine-opcode-fixes.md](docs/cpu-engine-opcode-fixes.md): the other CPU engines and
  their accuracy fixes.
- [docs/quadra-32bit-boot-crashes.md](docs/quadra-32bit-boot-crashes.md): diagnosing boot
  crashes.
- [docs/guest-window-mirroring.md](docs/guest-window-mirroring.md) and
  [docs/classic-menu-bar-compositor.md](docs/classic-menu-bar-compositor.md): experimental
  native window and menu bar integration.

The original Basilisk II and Cockatrice release notes are in [README.old](README.old) and
[README](README).

## Credits

- **Basilisk II** by Christian Bauer et al.
- **Cockatrice** and **Cockatrice III** by neozeed, with contributions including
  rakslice's SDL audio fixes.
- **Musashi** 680x0 emulator by Karl Stenerud.
- **uae-portable-cpu**, derived from the UAE/WinUAE CPU core and JIT (Bernd Schmidt, Toni Wilen
  and the UAE contributors).
- **m68k-rs** by Ben Letchford.
- **SDL** and **slirp**.
- LToUDP follows the protocol used by **Mini vMac**.

## License

GNU General Public License, version 2 — see [COPYING](COPYING).
