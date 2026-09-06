#!/usr/bin/env bash
# Rebuild the committed x86_64 SDL 1.2 prefix used by Intel / universal
# macOS builds. Run this on Apple Silicon when bumping SDL, then commit
# dist/dependencies/osx/intel/ (headers + dylibs + sdl-config).
#
# Usage:
#   ./rebuild-intel-sdl.sh
#   PREFIX=/tmp/sdl-x86_64 ./rebuild-intel-sdl.sh
#
# Downloads sdl3 / sdl2-compat / sdl12-compat sources via native ARM
# Homebrew (`brew unpack`), then compiles them with host cmake and
# CMAKE_OSX_ARCHITECTURES=x86_64. Does not install Intel Homebrew or
# Rosetta. Requires cmake and /opt/homebrew.
#
# The installed dylibs are rewritten to @rpath so the prefix is not
# tied to this machine's absolute path. CI links with
# -Wl,-rpath,<prefix>/lib.

set -euo pipefail
export HOMEBREW_NO_AUTO_UPDATE=1
export HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1
export HOMEBREW_NO_INSTALL_CLEANUP=1

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-$SCRIPT_DIR/intel}"
# Scratch trees stay out of git (see .gitignore).
BUILD_ROOT="${BUILD_ROOT:-$SCRIPT_DIR/.build}"
SRC="$BUILD_ROOT/src"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

if ! command -v cmake >/dev/null 2>&1; then
	echo "rebuild-intel-sdl: cmake is required (Xcode CLT or brew install cmake)" >&2
	exit 1
fi
if ! command -v brew >/dev/null 2>&1; then
	echo "rebuild-intel-sdl: native Homebrew (arm64) is required to unpack SDL sources" >&2
	exit 1
fi

echo "Rebuilding x86_64 SDL prefix at $PREFIX"
rm -rf "$PREFIX" "$BUILD_ROOT"
mkdir -p "$SRC" "$PREFIX"

echo "Unpacking sdl3, sdl2-compat, sdl12-compat sources via native brew..."
brew unpack --patch --force --destdir "$SRC" sdl3 sdl2-compat sdl12-compat

# brew unpack names follow the tarball (SDL3-3.4.16, sdl2-compat-2.32.72, ...).
find_unpacked() {
	local pattern="$1"
	local match
	match="$(find "$SRC" -maxdepth 1 -type d \( -iname "${pattern}-*" -o -iname "${pattern}" \) | head -1)"
	if [ -z "$match" ] || [ ! -d "$match" ]; then
		echo "rebuild-intel-sdl: no unpacked tree matching $pattern in $SRC" >&2
		ls -la "$SRC" >&2
		exit 1
	fi
	printf '%s\n' "$match"
}
SDL3_DIR="$(find_unpacked SDL3)"
SDL2COMPAT_DIR="$(find_unpacked sdl2-compat)"
SDL12_DIR="$(find_unpacked sdl12-compat)"

# Shared cmake flags: host compiler, x86_64 objects, install into PREFIX.
cmake_x86() {
	local src="$1"
	shift
	cmake -S "$src" -B "$src/build-x86_64" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_OSX_ARCHITECTURES=x86_64 \
		-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DCMAKE_PREFIX_PATH="$PREFIX" \
		"$@"
	cmake --build "$src/build-x86_64" -j"$JOBS"
	cmake --install "$src/build-x86_64"
}

echo "Building SDL3 (x86_64)..."
cmake_x86 "$SDL3_DIR" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_TESTS=OFF

echo "Building sdl2-compat (x86_64)..."
cmake_x86 "$SDL2COMPAT_DIR" -DSDL2COMPAT_TESTS=OFF

echo "Building sdl12-compat (x86_64)..."
cmake_x86 "$SDL12_DIR" -DSDL12COMPAT_TESTS=OFF

if [ ! -x "$PREFIX/bin/sdl-config" ]; then
	echo "rebuild-intel-sdl: sdl-config missing after install ($PREFIX)" >&2
	exit 1
fi

# Keep the committed prefix small: 1.2 headers, runtime dylibs, SDLmain.
rm -rf "$PREFIX/share" "$PREFIX/lib/cmake" "$PREFIX/include/SDL2" "$PREFIX/include/SDL3" \
	"$PREFIX/lib/pkgconfig" "$PREFIX/bin/sdl2-config"
rm -f "$PREFIX/lib"/libSDL2_test.a "$PREFIX/lib"/libSDL3_test.a "$PREFIX/lib"/libSDL2main.a

# Make each real dylib relocatable so CI/checkouts do not need this PREFIX path.
for dylib in "$PREFIX/lib"/*.dylib; do
	[ -e "$dylib" ] || continue
	[ -L "$dylib" ] && continue
	install_name_tool -id "@rpath/$(basename "$dylib")" "$dylib"
	otool -L "$dylib" | awk 'NR>1 {print $1}' | while IFS= read -r dep; do
		case "$dep" in
		"$PREFIX"/lib/*)
			install_name_tool -change "$dep" "@rpath/$(basename "$dep")" "$dylib"
			;;
		esac
	done
done

# cmake writes an absolute prefix=; replace with a relocatable sdl-config.
cat > "$PREFIX/bin/sdl-config" << 'EOF'
#!/bin/sh
# Relocatable sdl-config for the committed x86_64 prefix.
prefix="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
exec_prefix=${prefix}
libdir=${prefix}/lib
includedir=${prefix}/include
usage="Usage: $0 [--prefix[=DIR]] [--exec-prefix[=DIR]] [--version] [--cflags] [--libs]"
if test $# -eq 0; then
	echo "${usage}" 1>&2
	exit 1
fi
while test $# -gt 0; do
	case "$1" in
	--prefix=*) prefix="${1#--prefix=}" ;;
	--prefix) echo "$prefix" ;;
	--exec-prefix=*) exec_prefix="${1#--exec-prefix=}" ;;
	--exec-prefix) echo "$exec_prefix" ;;
	--version) echo 1.2.76 ;;
	--cflags) echo "-I${includedir} -I${includedir}/SDL -D_THREAD_SAFE" ;;
	--libs) echo "-L${libdir} -lSDLmain -lSDL -Wl,-framework,Cocoa" ;;
	*) echo "${usage}" 1>&2; exit 1 ;;
	esac
	shift
done
EOF
chmod +x "$PREFIX/bin/sdl-config"

{
	echo "Cockatrice III committed x86_64 SDL prefix"
	echo "built: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "host: $(uname -m) $(sw_vers -productVersion 2>/dev/null || true)"
	echo "formulae:"
	brew info --json=v2 sdl3 sdl2-compat sdl12-compat 2>/dev/null \
		| python3 -c 'import json,sys; d=json.load(sys.stdin);
[print("  %s %s" % (f["name"], f.get("versions",{}).get("stable",""))) for f in d.get("formulae",[])]' \
		|| true
	echo "lipo:"
	for dylib in "$PREFIX/lib"/*.dylib; do
		[ -e "$dylib" ] || continue
		[ -L "$dylib" ] && continue
		echo "  $(lipo -info "$dylib")"
	done
} > "$PREFIX/VERSION.txt"

echo "Installed x86_64 SDL prefix:"
"$PREFIX/bin/sdl-config" --prefix --cflags --libs
lipo -info "$PREFIX/lib"/libSDL*.dylib
lipo -info "$PREFIX/lib"/libSDL*.dylib | grep -q x86_64
echo "Commit dist/dependencies/osx/intel/ after reviewing VERSION.txt."
