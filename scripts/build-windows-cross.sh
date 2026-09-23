#!/usr/bin/env bash
# Local Windows cross build (x64 or ARM64) from macOS or Linux.
#
#   scripts/build-windows-cross.sh                 # x64: build + gate tests under Wine
#   scripts/build-windows-cross.sh --arch arm64    # ARM64: build + package (no tests)
#   scripts/build-windows-cross.sh --package      # also stage a runnable folder
#   scripts/build-windows-cross.sh --no-tests      # just build
#   scripts/build-windows-cross.sh --cpu-tests     # also run the engine accuracy suite
#   scripts/build-windows-cross.sh --clean         # reconfigure from scratch
#
# x64 uses MinGW-w64 GCC (brew install mingw-w64) and runs the tests under
# Wine. ARM64 uses llvm-mingw, downloaded into .cross-win/ on first use when
# aarch64-w64-mingw32-clang is not on PATH; its binaries cannot run here, so
# ARM64 skips the tests and always packages. Both need curl, zstd
# (brew install zstd) and cargo with the target's Rust std
# (rustup target add x86_64-pc-windows-gnu / aarch64-pc-windows-gnullvm).
#
# SDL comes from the same MSYS2 packages CI installs (mingw64 or clangarm64),
# cached in .cross-win/<arch>. The build goes to build-win-<arch>; --package
# stages the exe, the DLLs it needs and dist/'s prefs, XPRAM and ROM in
# build-win-<arch>/package, ready to copy to a Windows machine. A rebuild
# replaces only the exe and DLLs there, so edited prefs survive.
#
# Cross builds cannot run the code generators (build68k, gencpu, m68kmake):
# they are built for Windows. Their output is plain C that does not depend on
# the host, so it is copied from a native build (build/, configured and
# generated here if needed) with newer timestamps, and the generators are
# never run.
#
# Under Wine on Apple Silicon (Rosetta) the x86 JIT also fails
# mc68000/rox.bin; real x86_64 hardware does not, so treat that as an
# emulator artefact.

set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
if command -v sysctl >/dev/null 2>&1 && sysctl -n hw.ncpu >/dev/null 2>&1; then
	JOBS="$(sysctl -n hw.ncpu)"
else
	JOBS="$(nproc)"
fi

ARCH=x64
HOST_BUILD="$ROOT/build"
MSYS_MIRROR="${MSYS_MIRROR:-https://repo.msys2.org/mingw}"
LLVM_MINGW_VERSION="${LLVM_MINGW_VERSION:-20260922}"
RUN_TESTS=""
RUN_CPU_TESTS=0
DO_PACKAGE=0
DO_CLEAN=0

while [ $# -gt 0 ]; do
	case "$1" in
	--arch)      ARCH="${2:?--arch needs x64 or arm64}"; shift ;;
	--no-tests)  RUN_TESTS=0 ;;
	--cpu-tests) RUN_CPU_TESTS=1 ;;
	--package)   DO_PACKAGE=1 ;;
	--clean)     DO_CLEAN=1 ;;
	-j)          JOBS="${2:?-j needs a job count}"; shift ;;
	-h | --help)
		sed -n '2,33p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*)
		echo "build-windows-cross: unknown option '$1' (try --help)" >&2
		exit 1
		;;
	esac
	shift
done

# Prints an error and exits.
#
# Arguments:
#   $1: Message.
die() {
	echo "build-windows-cross: $1" >&2
	exit 1
}

CROSS="$ROOT/.cross-win"
case "$ARCH" in
x64)
	TRIPLE=x86_64-w64-mingw32
	PROCESSOR=AMD64
	MSYS_REPO=mingw64
	MSYS_PKG_PREFIX=mingw-w64-x86_64
	RUST_TARGET=x86_64-pc-windows-gnu
	CC="$TRIPLE-gcc"
	CXX="$TRIPLE-g++"
	RUN_TESTS="${RUN_TESTS:-1}"
	;;
arm64)
	TRIPLE=aarch64-w64-mingw32
	PROCESSOR=ARM64
	MSYS_REPO=clangarm64
	MSYS_PKG_PREFIX=mingw-w64-clang-aarch64
	RUST_TARGET=aarch64-pc-windows-gnullvm
	CC="$TRIPLE-clang"
	CXX="$TRIPLE-clang++"
	# ARM64 Windows binaries cannot run on this host
	RUN_TESTS=0
	RUN_CPU_TESTS=0
	DO_PACKAGE=1
	# Use llvm-mingw from PATH, or fetch the pinned release once
	if ! command -v "$CC" >/dev/null 2>&1; then
		LLVM_MINGW="$CROSS/llvm-mingw-$LLVM_MINGW_VERSION"
		if [ ! -x "$LLVM_MINGW/bin/$CC" ]; then
			case "$(uname -s)" in
			Darwin) asset="llvm-mingw-$LLVM_MINGW_VERSION-ucrt-macos-universal.tar.xz" ;;
			Linux)  asset="llvm-mingw-$LLVM_MINGW_VERSION-ucrt-ubuntu-22.04-$(uname -m).tar.xz" ;;
			*)      die "no llvm-mingw download for $(uname -s); put $CC on PATH" ;;
			esac
			echo "==> fetching $asset"
			mkdir -p "$CROSS"
			curl -fSL --progress-bar -o "$CROSS/$asset" \
				"https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_VERSION/$asset"
			tar -xf "$CROSS/$asset" -C "$CROSS"
			mv "$CROSS/${asset%.tar.xz}" "$LLVM_MINGW"
			rm -f "$CROSS/$asset"
		fi
		export PATH="$LLVM_MINGW/bin:$PATH"
	fi
	;;
*)
	die "--arch must be x64 or arm64"
	;;
esac

BUILD_DIR="$ROOT/build-win-$ARCH"
CACHE="$CROSS/$ARCH"
SDL_PREFIX="$CACHE/$MSYS_REPO"

# Tools
for tool in "$CC" "$CXX" curl cargo cmake; do
	command -v "$tool" >/dev/null 2>&1 || die "$tool not found (see --help)"
done
if command -v rustup >/dev/null 2>&1 &&
	! rustup target list --installed | grep -qx "$RUST_TARGET"; then
	die "Rust target missing: rustup target add $RUST_TARGET"
fi
if [ "$RUN_TESTS" -eq 1 ] || [ "$RUN_CPU_TESTS" -eq 1 ]; then
	command -v wine >/dev/null 2>&1 || die "wine not found (install it or pass --no-tests)"
fi

# Extracts an MSYS2 .pkg.tar.zst into the current directory.
#
# Arguments:
#   $1: Package file.
extract_pkg() {
	if tar --zstd -tf "$1" >/dev/null 2>&1; then
		tar --zstd -xf "$1"
	elif command -v unzstd >/dev/null 2>&1; then
		unzstd -c "$1" | tar -xf -
	else
		die "need tar --zstd or unzstd (brew install zstd)"
	fi
}

# Downloads and extracts the newest MSYS2 build of a package for this arch.
#
# Arguments:
#   $1: Package name without the MSYS2 architecture prefix.
fetch_pkg() {
	local name
	name="$(curl -fsSL "$MSYS_MIRROR/$MSYS_REPO/" |
		grep -oE "$MSYS_PKG_PREFIX-$1-[0-9][^\"<> ]*-any\.pkg\.tar\.zst" |
		grep -v '\.sig$' | sort -uV | tail -1)"
	[ -n "$name" ] || die "could not find $1 in $MSYS_MIRROR/$MSYS_REPO"
	echo "    $name"
	curl -fsSL -o "$name" "$MSYS_MIRROR/$MSYS_REPO/$name"
	extract_pkg "$name"
	rm -f "$name"
}

# SDL 1.2 (sdl12-compat over SDL2), as CI installs it
if [ ! -f "$SDL_PREFIX/lib/libSDL.dll.a" ]; then
	echo "==> fetching SDL from MSYS2 ($MSYS_REPO) into $CACHE"
	mkdir -p "$CACHE"
	(cd "$CACHE" && fetch_pkg sdl12-compat && fetch_pkg SDL2)
fi

# The generated sources come from a native build of the same tree
echo "==> host code generation in $HOST_BUILD"
if [ ! -f "$HOST_BUILD/CMakeCache.txt" ]; then
	cmake -S "$ROOT" -B "$HOST_BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
fi
cmake --build "$HOST_BUILD" -j"$JOBS" --target uaecpu musashi >/dev/null

if [ "$DO_CLEAN" -eq 1 ]; then
	echo "==> removing $BUILD_DIR"
	rm -rf "$BUILD_DIR"
fi

# Toolchain file: the cross compilers, their binutils, and find_* confined to
# the SDL prefix. llvm-mingw's llvm-nm/llvm-objcopy drive the core's symbol
# isolation, which renames symbols on Windows clang.
TOOLCHAIN="$CROSS/mingw-$ARCH.cmake"
mkdir -p "$CROSS"
{
	echo "set(CMAKE_SYSTEM_NAME Windows)"
	echo "set(CMAKE_SYSTEM_PROCESSOR $PROCESSOR)"
	echo "set(CMAKE_C_COMPILER $CC)"
	echo "set(CMAKE_CXX_COMPILER $CXX)"
	echo "set(CMAKE_RC_COMPILER $TRIPLE-windres)"
	if [ "$ARCH" = arm64 ]; then
		echo "set(CMAKE_NM $(command -v llvm-nm))"
		echo "set(CMAKE_OBJCOPY $(command -v llvm-objcopy))"
		echo "set(CMAKE_AR $(command -v llvm-ar))"
		echo "set(CMAKE_RANLIB $(command -v llvm-ranlib))"
	fi
	echo "set(CMAKE_FIND_ROOT_PATH \"$SDL_PREFIX\")"
	echo "set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)"
	echo "set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)"
	echo "set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)"
} >"$TOOLCHAIN"

echo "==> configure ($ARCH) -> $BUILD_DIR"
cmake -S "$ROOT" -B "$BUILD_DIR" \
	-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
	-DCMAKE_BUILD_TYPE=Release \
	-DSDL_INCLUDE_DIR="$SDL_PREFIX/include/SDL" \
	-DSDL_LIBRARY="$SDL_PREFIX/lib/libSDLmain.a;$SDL_PREFIX/lib/libSDL.dll.a" >/dev/null

# Build the generators so the dependency graph is satisfied, then put the
# host's generated sources in place, newer than every generator.
CORE_SRC="$HOST_BUILD/BasiliskII/vendor/uae-portable-cpu"
CORE_DST="$BUILD_DIR/BasiliskII/vendor/uae-portable-cpu"
MUSASHI_SRC="$HOST_BUILD/BasiliskII/vendor/musashi"
MUSASHI_DST="$BUILD_DIR/BasiliskII/vendor/musashi"
cmake --build "$BUILD_DIR" -j"$JOBS" --target build68k m68kmake >/dev/null
cp "$CORE_SRC/cpudefs.c" "$CORE_DST/"
sleep 1
touch "$CORE_DST/cpudefs.c"
cmake --build "$BUILD_DIR" -j"$JOBS" --target gencpu >/dev/null
cp "$CORE_SRC"/cpuemu_*.c "$CORE_SRC/cpustbl.c" "$CORE_SRC/cputbl.h" "$CORE_DST/"
cp "$MUSASHI_SRC"/m68kops.c "$MUSASHI_SRC"/m68kops.h "$MUSASHI_DST/"
sleep 1
# Not cpudefs.c: it is gencpu's input, and a newer one relinks gencpu
touch "$CORE_DST"/cpuemu_*.c "$CORE_DST"/cpustbl.c "$CORE_DST"/cputbl.h \
	"$MUSASHI_DST"/m68kops.c "$MUSASHI_DST"/m68kops.h

echo "==> build -j$JOBS"
cmake --build "$BUILD_DIR" -j"$JOBS"
echo "==> built $BUILD_DIR/BasiliskII/CockatriceIII.exe"

# The toolchain's own runtime DLLs (libwinpthread, llvm-mingw's libc++ and
# libunwind) sit in its target bin directory
if [ "$ARCH" = arm64 ]; then
	TOOLCHAIN_BIN="$(dirname "$(command -v "$CC")")/../$TRIPLE/bin"
else
	TOOLCHAIN_BIN="$(dirname "$("$CC" -print-file-name=libwinpthread.a)")/../bin"
fi

if [ "$DO_PACKAGE" -eq 1 ]; then
	PKG="$BUILD_DIR/package"
	echo "==> package -> $PKG"
	# Replace the program, but keep the prefs, XPRAM and ROM a previous run
	# left there: they are the user's settings and the guest's PRAM
	mkdir -p "$PKG"
	rm -f "$PKG"/*.exe "$PKG"/*.dll
	cp "$BUILD_DIR/BasiliskII/CockatriceIII.exe" "$PKG/"
	for f in CockatriceIII_Prefs CockatriceIII_XPRAM Quadra800.rom; do
		if [ -f "$ROOT/dist/$f" ] && [ ! -f "$PKG/$f" ]; then cp "$ROOT/dist/$f" "$PKG/"; fi
	done
	# sdl12-compat's SDL.dll loads SDL2.dll at run time, so it is not an import
	cp "$SDL_PREFIX/bin/SDL.dll" "$SDL_PREFIX/bin/SDL2.dll" "$PKG/"
	# Copy every DLL the package imports that the toolchain or SDL prefix has,
	# until nothing new turns up
	OBJDUMP="$(command -v llvm-objdump || command -v "$TRIPLE-objdump")"
	added=1
	while [ "$added" -eq 1 ]; do
		added=0
		for f in "$PKG"/*.exe "$PKG"/*.dll; do
			for dll in $("$OBJDUMP" -p "$f" | awk '/DLL Name:/ {print $3}'); do
				[ -f "$PKG/$dll" ] && continue
				for dir in "$TOOLCHAIN_BIN" "$SDL_PREFIX/bin"; do
					if [ -f "$dir/$dll" ]; then
						cp "$dir/$dll" "$PKG/"
						added=1
						break
					fi
				done
			done
		done
	done
	ls "$PKG" | sed 's/^/    /'
fi

# Wine finds the MinGW runtime DLLs through WINEPATH
export WINEDEBUG=-all
export WINEPATH="$TOOLCHAIN_BIN;$SDL_PREFIX/bin"

# Runs one test binary under Wine with a time limit, so a hang cannot wedge
# the script.
#
# Arguments:
#   $1: Test executable.
#   $2: Time limit in seconds.
run_wine() {
	perl -e 'alarm shift; exec @ARGV' "$2" wine "$1"
}

if [ "$RUN_TESTS" -eq 1 ]; then
	echo "==> gate tests (Wine)"
	failed=0
	cd "$BUILD_DIR/BasiliskII/tests"
	for t in basilisk_*_test.exe; do
		log="${t%.exe}.wine.log"
		run_wine "$t" 180 >"$log" 2>&1 || true
		result="$(tr -d '\r' <"$log" | grep -E '^Results' | tail -1)"
		echo "    ${t%.exe}: ${result:-no result, see $log}"
		case "$result" in
		*", 0 failed") ;;
		*) failed=1 ;;
		esac
	done
	[ "$failed" -eq 0 ] || die "gate tests failed"
fi

if [ "$RUN_CPU_TESTS" -eq 1 ]; then
	# Reported, never gated: 5 known failures (see BasiliskII/tests/README.md)
	echo "==> cpu tests (Wine; informational, 5 known failures)"
	cd "$BUILD_DIR/BasiliskII/tests"
	run_wine cpu_tests.exe 900 2>&1 | tr -d '\r' | grep -E '\[FAIL\]|^Results' || true
fi
