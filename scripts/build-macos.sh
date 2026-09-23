#!/usr/bin/env bash
# Local macOS build. Configure, build, and run the gate tests in one step.
#
#   scripts/build-macos.sh                 # Release, build + gate tests
#   scripts/build-macos.sh --debug         # Debug build
#   scripts/build-macos.sh --asan          # ASan + UBSan (tests)
#   scripts/build-macos.sh --no-tests      # just build
#   scripts/build-macos.sh --cpu-tests     # also run the engine accuracy suite
#   scripts/build-macos.sh --bundle        # also assemble CockatriceIII.app
#   scripts/build-macos.sh --run           # launch the emulator afterwards
#   scripts/build-macos.sh --clean         # reconfigure from scratch
#   scripts/build-macos.sh --arch x86_64   # cross-build the Intel slice
#
# This is the iteration script. Release and CI artifacts are built by
# scripts/ci-osx-build.sh (arm64 | amd64 | universal), which also does the lipo
# and packaging; use that when reproducing a CI job, not this.

set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
JOBS="$(sysctl -n hw.ncpu)"
INTEL_SDL="$ROOT/dist/dependencies/osx/intel"

BUILD_TYPE=Release
BUILD_DIR="$ROOT/build"
ARCH=""
ASAN=OFF
RUN_TESTS=1
RUN_CPU_TESTS=0
DO_BUNDLE=0
DO_RUN=0
DO_CLEAN=0

while [ $# -gt 0 ]; do
	case "$1" in
	--debug)     BUILD_TYPE=Debug ;;
	--release)   BUILD_TYPE=Release ;;
	--asan)      ASAN=ON; BUILD_DIR="$ROOT/build-asan" ;;
	--no-tests)  RUN_TESTS=0 ;;
	--cpu-tests) RUN_CPU_TESTS=1 ;;
	--bundle)    DO_BUNDLE=1 ;;
	--run)       DO_RUN=1 ;;
	--clean)     DO_CLEAN=1 ;;
	--arch)      ARCH="${2:?--arch needs arm64 or x86_64}"; shift ;;
	-j)          JOBS="${2:?-j needs a job count}"; shift ;;
	-h | --help)
		sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*)
		echo "build-macos: unknown option '$1' (try --help)" >&2
		exit 1
		;;
	esac
	shift
done

cd "$ROOT"

if [ "$DO_CLEAN" -eq 1 ]; then
	echo "==> removing $BUILD_DIR"
	rm -rf "$BUILD_DIR"
fi

CONFIGURE_ARGS=(
	-S "$ROOT" -B "$BUILD_DIR"
	-DCMAKE_BUILD_TYPE="$BUILD_TYPE"
	-DCOCKATRICE_TEST_ASAN="$ASAN"
)

if [ -n "$ARCH" ]; then
	CONFIGURE_ARGS+=(-DCMAKE_OSX_ARCHITECTURES="$ARCH")
	# FindSDL would otherwise pick the native ARM Homebrew SDL for an x86_64
	# build. The committed Intel prefix is the supported way to cross-build
	# that slice; CI never compiles SDL from source.
	if [ "$ARCH" = "x86_64" ]; then
		if [ ! -x "$INTEL_SDL/bin/sdl-config" ]; then
			echo "build-macos: missing Intel SDL prefix at $INTEL_SDL" >&2
			echo "  rebuild it with dist/dependencies/osx/rebuild-intel-sdl.sh" >&2
			exit 1
		fi
		CONFIGURE_ARGS+=(
			-DSDL_INCLUDE_DIR="$INTEL_SDL/include/SDL"
			-DSDL_LIBRARY="$INTEL_SDL/lib/libSDLmain.a;$INTEL_SDL/lib/libSDL.dylib;-framework Cocoa"
		)
	fi
fi

echo "==> configure ($BUILD_TYPE${ARCH:+, $ARCH}${ASAN:+, asan=$ASAN}) -> $BUILD_DIR"
cmake "${CONFIGURE_ARGS[@]}"

echo "==> build -j$JOBS"
cmake --build "$BUILD_DIR" -j"$JOBS"

if [ "$DO_BUNDLE" -eq 1 ]; then
	echo "==> bundle"
	cmake --build "$BUILD_DIR" --target bundle -j"$JOBS"
fi

if [ "$RUN_TESTS" -eq 1 ]; then
	echo "==> gate tests"
	ctest --test-dir "$BUILD_DIR" -L gate --output-on-failure
fi

if [ "$RUN_CPU_TESTS" -eq 1 ]; then
	# Engine accuracy is reported, never gated: there are 5 known failures
	# (cmp2 in each of the five UAE configurations), so a non-zero
	# exit here is expected and must not fail the script.
	echo "==> cpu tests (informational; 5 known failures)"
	ctest --test-dir "$BUILD_DIR" -L cpu --output-on-failure || true
fi

BIN="$BUILD_DIR/BasiliskII/CockatriceIII"
echo "==> built $BIN"
file "$BIN"

if [ "$DO_RUN" -eq 1 ]; then
	# The emulator resolves prefs, XPRAM and the ROM relative to its working
	# directory, so run it from dist/ where those actually live.
	echo "==> running from $ROOT/dist"
	cd "$ROOT/dist"
	exec "$BIN"
fi
