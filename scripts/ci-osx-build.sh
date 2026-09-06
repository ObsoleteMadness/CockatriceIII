#!/usr/bin/env bash
# Local/CI macOS build used by .github/workflows/build-and-release.yml.
#
# Usage:
#   scripts/ci-osx-build.sh arm64|amd64|universal [version]
#
# This is the same setup + compile + lipo checks as the osx-* CI jobs.
# Run it on Apple Silicon to reproduce a GitHub Actions macOS build
# before pushing. Packaging the .dmg stays in the workflow.
#
# Arguments:
#   $1  Slice to build: arm64, amd64 (x86_64), or universal (lipo).
#   $2  Optional APP_VERSION (default: dev-local).

set -euo pipefail

ARCH="${1:?usage: $0 arm64|amd64|universal [version]}"
VERSION="${2:-dev-local}"
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/BasiliskII/OSX64"
JOBS="$(sysctl -n hw.ncpu)"

case "$ARCH" in
arm64|amd64|universal) ;;
*)
	echo "ci-osx-build: unknown arch '$ARCH' (want arm64, amd64, or universal)" >&2
	exit 1
	;;
esac

cd "$ROOT"

echo "ci-osx-build: ${ARCH} version=${VERSION}"

# Native ARM brew for the arm64 slice only. Intel / universal link
# the committed prefix in dist/dependencies/osx/intel (rebuild with
# dist/dependencies/osx/rebuild-intel-sdl.sh, do not compile SDL here).
if [ "$ARCH" != "amd64" ]; then
	brew install sdl12-compat
fi
if [ "$ARCH" = "amd64" ] || [ "$ARCH" = "universal" ]; then
	rustup target add x86_64-apple-darwin
	test -x dist/dependencies/osx/intel/bin/sdl-config
	test -f dist/dependencies/osx/intel/include/SDL/SDL.h
	lipo -info dist/dependencies/osx/intel/lib/libSDL*.dylib | grep -q x86_64
fi

# Local trees keep obj/ and slirp from the last ARCH. CI checkouts are
# empty, so clean first so a laptop run matches a runner.
make -C "$BUILD_DIR" clean-objects

if [ "$ARCH" = "universal" ]; then
	make -C "$BUILD_DIR" app-universal APP_VERSION="${VERSION#v}" -j"$JOBS"
	file "$BUILD_DIR/CockatriceIII"
	lipo -info "$BUILD_DIR/CockatriceIII"
	lipo -info "$BUILD_DIR/CockatriceIII" | grep -q x86_64
	lipo -info "$BUILD_DIR/CockatriceIII" | grep -q arm64
else
	make -C "$BUILD_DIR" app ARCH="$ARCH" APP_VERSION="${VERSION#v}" -j"$JOBS"
	file "$BUILD_DIR/CockatriceIII"
	lipo -info "$BUILD_DIR/CockatriceIII"
	if [ "$ARCH" = "amd64" ]; then
		lipo -info "$BUILD_DIR/CockatriceIII" | grep -q x86_64
	else
		lipo -info "$BUILD_DIR/CockatriceIII" | grep -q arm64
	fi
fi
file "$BUILD_DIR/CockatriceIII.app/Contents/MacOS/CockatriceIII"

echo "ci-osx-build: ${ARCH} ok"
