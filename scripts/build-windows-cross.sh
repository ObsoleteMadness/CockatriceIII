#!/usr/bin/env bash
# Local Windows x64 cross build from macOS or Linux with MinGW-w64.
#
#   scripts/build-windows-cross.sh               # build + gate tests under Wine
#   scripts/build-windows-cross.sh --no-tests    # just build
#   scripts/build-windows-cross.sh --cpu-tests   # also run the engine accuracy suite
#   scripts/build-windows-cross.sh --clean       # reconfigure from scratch
#
# Needs: x86_64-w64-mingw32-gcc/g++ (brew install mingw-w64), curl and zstd
# (brew install zstd), cargo with the x86_64-pc-windows-gnu target
# (rustup target add x86_64-pc-windows-gnu), and Wine for the tests.
# Windows ARM64 needs llvm-mingw and is left to CI.
#
# SDL comes from the same MSYS2 packages CI installs, cached in
# .cross-win/x64. The build goes to build-win-x64.
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

BUILD_DIR="$ROOT/build-win-x64"
HOST_BUILD="$ROOT/build"
CACHE="$ROOT/.cross-win/x64"
MSYS_MIRROR="${MSYS_MIRROR:-https://repo.msys2.org/mingw}"
RUN_TESTS=1
RUN_CPU_TESTS=0
DO_CLEAN=0

while [ $# -gt 0 ]; do
	case "$1" in
	--no-tests)  RUN_TESTS=0 ;;
	--cpu-tests) RUN_CPU_TESTS=1 ;;
	--clean)     DO_CLEAN=1 ;;
	-j)          JOBS="${2:?-j needs a job count}"; shift ;;
	-h | --help)
		sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'
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

# Tools
for tool in x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ curl cargo cmake; do
	command -v "$tool" >/dev/null 2>&1 || die "$tool not found (see --help)"
done
if command -v rustup >/dev/null 2>&1 &&
	! rustup target list --installed | grep -qx x86_64-pc-windows-gnu; then
	die "Rust target missing: rustup target add x86_64-pc-windows-gnu"
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

# Downloads and extracts the newest MSYS2 mingw64 build of a package.
#
# Arguments:
#   $1: Package name without the mingw-w64-x86_64- prefix.
fetch_pkg() {
	local name
	name="$(curl -fsSL "$MSYS_MIRROR/mingw64/" |
		grep -oE "mingw-w64-x86_64-$1-[0-9][^\"<> ]*-any\.pkg\.tar\.zst" |
		grep -v '\.sig$' | sort -V | tail -1)"
	[ -n "$name" ] || die "could not find $1 on $MSYS_MIRROR"
	echo "    $name"
	curl -fsSL -o "$name" "$MSYS_MIRROR/mingw64/$name"
	extract_pkg "$name"
	rm -f "$name"
}

# SDL 1.2 (sdl12-compat over SDL2), as CI installs it
if [ ! -f "$CACHE/mingw64/lib/libSDL.dll.a" ]; then
	echo "==> fetching SDL from MSYS2 into $CACHE"
	mkdir -p "$CACHE"
	(cd "$CACHE" && fetch_pkg sdl12-compat && fetch_pkg SDL2)
fi
SDL_PREFIX="$CACHE/mingw64"

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

# Toolchain file: MinGW compilers, and find_* confined to the SDL prefix
TOOLCHAIN="$ROOT/.cross-win/mingw-x64.cmake"
cat >"$TOOLCHAIN" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH "$SDL_PREFIX")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

echo "==> configure -> $BUILD_DIR"
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
touch "$CORE_DST"/cpudefs.c "$CORE_DST"/cpuemu_*.c "$CORE_DST"/cpustbl.c "$CORE_DST"/cputbl.h \
	"$MUSASHI_DST"/m68kops.c "$MUSASHI_DST"/m68kops.h

echo "==> build -j$JOBS"
cmake --build "$BUILD_DIR" -j"$JOBS"
echo "==> built $BUILD_DIR/BasiliskII/CockatriceIII.exe"

# Wine finds the MinGW runtime DLLs (libwinpthread sits in the toolchain's
# target bin directory, beside its lib directory) through WINEPATH
MINGW_TARGET_LIB="$(dirname "$(x86_64-w64-mingw32-gcc -print-file-name=libwinpthread.a)")"
export WINEDEBUG=-all
export WINEPATH="$MINGW_TARGET_LIB/../bin;$SDL_PREFIX/bin"

# Runs one test binary under Wine with a time limit (no fork() on Windows, so
# a hang would otherwise wedge the script).
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
	# Reported, never gated: 20 known failures (see BasiliskII/tests/README.md)
	echo "==> cpu tests (Wine; informational, 20 known failures)"
	cd "$BUILD_DIR/BasiliskII/tests"
	run_wine cpu_tests.exe 900 2>&1 | tr -d '\r' | grep -E '\[FAIL\]|^Results' || true
fi
