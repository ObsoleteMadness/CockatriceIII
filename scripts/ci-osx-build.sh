#!/usr/bin/env bash
# Local/CI macOS build, used by .github/workflows/build-and-release.yml.
#
# Usage:
#   scripts/ci-osx-build.sh arm64|amd64|universal [version]
#
# Run it on Apple Silicon to reproduce a GitHub Actions macOS build before
# pushing. Packaging the .dmg stays in the workflow.
#
# The universal slice is built as two separate configurations joined with lipo,
# not via CMAKE_OSX_ARCHITECTURES="arm64;x86_64": the vendored CPU core links a
# symbol-isolated object and that admits only one architecture per build (the
# top-level CMakeLists refuses a multi-arch configuration outright).

set -euo pipefail

ARCH="${1:?usage: $0 arm64|amd64|universal [version]}"
VERSION="${2:-dev-local}"
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
JOBS="$(sysctl -n hw.ncpu)"
INTEL_SDL="$ROOT/dist/dependencies/osx/intel"

case "$ARCH" in
arm64 | amd64 | universal) ;;
*)
	echo "ci-osx-build: unknown arch '$ARCH' (want arm64, amd64, or universal)" >&2
	exit 1
	;;
esac

cd "$ROOT"
echo "ci-osx-build: ${ARCH} version=${VERSION}"

# Native ARM brew for the arm64 slice only. Intel and universal link the
# committed prefix in dist/dependencies/osx/intel (rebuild it with
# dist/dependencies/osx/rebuild-intel-sdl.sh); CI must not compile SDL.
if [ "$ARCH" != "amd64" ]; then
	brew list sdl12-compat >/dev/null 2>&1 || brew install sdl12-compat
fi
if [ "$ARCH" = "amd64" ] || [ "$ARCH" = "universal" ]; then
	rustup target add x86_64-apple-darwin
	test -x "$INTEL_SDL/bin/sdl-config"
	test -f "$INTEL_SDL/include/SDL/SDL.h"
	lipo -info "$INTEL_SDL"/lib/libSDL*.dylib | grep -q x86_64
fi

# Configure one slice. FindSDL would otherwise pick up the native ARM Homebrew
# SDL for an x86_64 build, so the Intel slice is pointed at the committed
# prefix explicitly.
configure_slice() {
	local osx_arch="$1" build_dir="$2"
	local args=(
		-S "$ROOT" -B "$build_dir"
		-DCMAKE_BUILD_TYPE=Release
		-DCMAKE_OSX_ARCHITECTURES="$osx_arch"
		-DCOCKATRICE_BUNDLE_VERSION="${VERSION#v}"
	)
	if [ "$osx_arch" = "x86_64" ]; then
		args+=(
			-DSDL_INCLUDE_DIR="$INTEL_SDL/include/SDL"
			-DSDL_LIBRARY="$INTEL_SDL/lib/libSDLmain.a;$INTEL_SDL/lib/libSDL.dylib;-framework Cocoa"
		)
	fi
	cmake "${args[@]}"
}

build_slice() {
	local osx_arch="$1" build_dir="$2"
	configure_slice "$osx_arch" "$build_dir"
	cmake --build "$build_dir" --target CockatriceIII -j"$JOBS"
}

OUT="$ROOT/build/CockatriceIII"

if [ "$ARCH" = "universal" ]; then
	build_slice arm64 "$ROOT/build-arm64"
	build_slice x86_64 "$ROOT/build-x86_64"
	mkdir -p "$ROOT/build"
	lipo -create \
		"$ROOT/build-arm64/BasiliskII/CockatriceIII" \
		"$ROOT/build-x86_64/BasiliskII/CockatriceIII" \
		-output "$OUT"
	# lipo output is unsigned, and the JIT entitlement is not optional: without
	# com.apple.security.cs.allow-jit the translated-code mapping cannot be made
	# executable under Hardened Runtime.
	codesign -s - --force \
		--entitlements "$ROOT/BasiliskII/platform/darwin/entitlements.plist" "$OUT"
	# Assemble the bundle from the arm64 tree, then drop the fat binary in.
	cmake --build "$ROOT/build-arm64" --target bundle -j"$JOBS"
	rm -rf "$ROOT/build/CockatriceIII.app"
	cp -R "$ROOT/build-arm64/BasiliskII/CockatriceIII.app" "$ROOT/build/CockatriceIII.app"
	cp "$OUT" "$ROOT/build/CockatriceIII.app/Contents/MacOS/CockatriceIII"
	codesign -s - --force --deep \
		--entitlements "$ROOT/BasiliskII/platform/darwin/entitlements.plist" \
		"$ROOT/build/CockatriceIII.app"
	lipo -info "$OUT" | grep -q x86_64
	lipo -info "$OUT" | grep -q arm64
else
	case "$ARCH" in
	arm64) osx_arch=arm64 ;;
	amd64) osx_arch=x86_64 ;;
	esac
	build_slice "$osx_arch" "$ROOT/build"
	cmake --build "$ROOT/build" --target bundle -j"$JOBS"
	cp "$ROOT/build/BasiliskII/CockatriceIII" "$OUT" 2>/dev/null || true
	rm -rf "$ROOT/build/CockatriceIII.app"
	cp -R "$ROOT/build/BasiliskII/CockatriceIII.app" "$ROOT/build/CockatriceIII.app"
	lipo -info "$OUT" | grep -q "$osx_arch"
fi

file "$OUT"
lipo -info "$OUT"
file "$ROOT/build/CockatriceIII.app/Contents/MacOS/CockatriceIII"
echo "ci-osx-build: ${ARCH} ok"
