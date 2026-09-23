# CI builds and packaging

## GitHub Actions

[.github/workflows/build-and-release.yml](../.github/workflows/build-and-release.yml)
builds all targets on every push, on pull requests into `main`, and on
`v*` tags (which also cuts a GitHub Release):

| Target        | Runner                                     |
|---------------|--------------------------------------------|
| osx-arm       | macos-latest (Apple Silicon, native arm64) |
| osx-amd64     | macos-latest (Apple Silicon, cross x86_64) |
| osx-universal | macos-latest (Apple Silicon, lipo fat)     |
| win-x64       | windows-latest (MINGW64)                   |
| win-arm64     | windows-11-arm (CLANGARM64)                |
| linux-x64     | ubuntu-latest                              |
| linux-arm64   | ubuntu-24.04-arm                           |

Every target builds with the same two CMake commands; there are no per-target
build directories any more. `win32-x86` was dropped with 32-bit support. Each
job runs the `gate` test label (must pass) and the `cpu` label (reported only).

The workflow triggers on push to **any** branch (not just `main`), so pushing
a feature/port branch runs the full build matrix without needing to merge
first.

## Artifacts and releases

Each job packages its build (`.dmg` on macOS, `.zip` on Windows) alongside the
files in [dist/](../dist/) and uploads it as a build artifact; on a version tag
the artifacts from all targets are attached to a single
[GitHub Release](https://github.com/ObsoleteMadness/CockatriceIII/releases).
Each macOS `.dmg` carries both the flat `CockatriceIII` binary and a
double-clickable `CockatriceIII.app` bundle, each with its own copy of the
prefs, XPRAM and archive from `dist/`.

## Reproducing a macOS job locally

[scripts/ci-osx-build.sh](../scripts/ci-osx-build.sh) (`arm64`, `amd64`, or
`universal`) is exactly what the workflow runs, so it reproduces a macOS CI
job on an Apple Silicon machine. For day-to-day work,
[scripts/build-macos.sh](../scripts/build-macos.sh) wraps the CMake build
(`--debug`, `--asan`, `--bundle`, `--run`, `--clean`, `--arch x86_64`);
`--help` lists them all.

## Reproducing the Linux and Windows jobs locally

Two scripts build the other platforms from a Mac (or any Docker/MinGW host)
and run the same gate tests as CI:

- [scripts/build-linux-docker.sh](../scripts/build-linux-docker.sh) builds in an
  Ubuntu 24.04 container set up like the CI runner
  ([scripts/docker/linux.Dockerfile](../scripts/docker/linux.Dockerfile)).
  `--arch arm64|amd64` picks the target; amd64 runs under emulation on Apple
  Silicon. The working tree is streamed into the container, so uncommitted
  edits are included and nothing is written back unless `--out DIR` asks for
  the binary.
- [scripts/build-windows-cross.sh](../scripts/build-windows-cross.sh)
  cross-builds Windows into `build-win-<arch>/`, using SDL from the same MSYS2
  packages CI installs. `--arch x64` (the default) uses MinGW-w64 GCC and runs
  the gate suites under Wine. `--arch arm64` uses llvm-mingw, fetched into
  `.cross-win/` on first use; its binaries cannot run on the build host, so it
  skips the tests and stages `build-win-arm64/package/` (the exe, every DLL it
  imports, and `dist/`'s prefs, XPRAM and ROM) to copy to an ARM64 Windows
  machine or VM. `--package` does the same for x64. The core and Musashi code
  generators cannot run in a cross build, so their output (plain C, host
  independent) is copied from the native `build/`.

Under emulation (the amd64 container, or Wine under Rosetta) the x86 JIT also
fails `mc68000/rox.bin`. Real x86-64 hardware in CI does not, so treat that
as an emulator artefact rather than a regression.

## macOS bundles and the universal binary

`cmake --build build --target bundle` wraps the binary into
`CockatriceIII.app` with a generated `Info.plist`
(`-DCOCKATRICE_BUNDLE_VERSION=x.y.z` sets `CFBundleVersion` and
`CFBundleShortVersionString`), copies in the app icon, and ad-hoc codesigns it.
The JIT entitlement is not optional: without `com.apple.security.cs.allow-jit`
the translated-code mapping cannot be made executable under Hardened Runtime.
The ROM and prefs file are looked up in `Contents/Resources` as well as beside
the executable (see [CockatriceIII_Prefs.md](CockatriceIII_Prefs.md#where-the-file-is-searched-for)),
so a bundled ROM is found however the app was launched.

Universal binaries are built as two separate configurations joined with `lipo`,
not via `CMAKE_OSX_ARCHITECTURES="arm64;x86_64"` — the vendored CPU core links
a symbol-isolated object, which admits only one architecture per build, so the
top-level `CMakeLists.txt` refuses a multi-arch configuration outright.
[scripts/ci-osx-build.sh](../scripts/ci-osx-build.sh) does this. The Intel
slice links the committed prefix in
[`dist/dependencies/osx/intel`](../dist/dependencies/osx) (rebuild with
`dist/dependencies/osx/rebuild-intel-sdl.sh`); CI never compiles SDL from
source.
