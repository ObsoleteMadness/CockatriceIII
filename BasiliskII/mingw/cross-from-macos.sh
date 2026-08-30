#!/usr/bin/env bash
# Cross-compile Cockatrice III for Windows from macOS.
#
# Usage:
#   ./cross-from-macos.sh [x64|x86|arm64]
#
# Prerequisites:
#   x64/x86:  brew install mingw-w64 zstd
#   arm64:    an llvm-mingw toolchain on PATH (aarch64-w64-mingw32-clang)
#             plus brew install zstd
#
# SDL is pulled from the matching MSYS2 repo (same source CI uses) into
# .cross-win/<arch>/ so host sdl-config cannot leak macOS SDL into the link.

set -euo pipefail

ARCH="${1:-x64}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CACHE="${SCRIPT_DIR}/.cross-win/${ARCH}"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
MSYS_MIRROR="${MSYS_MIRROR:-https://repo.msys2.org/mingw}"

case "${ARCH}" in
  x64|x86_64|amd64)
    ARCH=x64
    MAKE_ARCH=x64
    TRIPLE="x86_64-w64-mingw32"
    MSYS_REPO="mingw64"
    MSYS_PFX="mingw-w64-x86_64"
    EXTRACT_ROOT="mingw64"
    ;;
  x86|i686|i386)
    ARCH=x86
    MAKE_ARCH=x86
    TRIPLE="i686-w64-mingw32"
    MSYS_REPO="mingw32"
    MSYS_PFX="mingw-w64-i686"
    EXTRACT_ROOT="mingw32"
    ;;
  arm64|aarch64)
    ARCH=arm64
    MAKE_ARCH=arm64
    TRIPLE="aarch64-w64-mingw32"
    MSYS_REPO="clangarm64"
    MSYS_PFX="mingw-w64-clang-aarch64"
    EXTRACT_ROOT="clangarm64"
    ;;
  *)
    echo "Usage: $0 [x64|x86|arm64]" >&2
    exit 2
    ;;
esac

pick_tool() {
  local name="$1"
  shift
  local c
  for c in "$@"; do
    if command -v "$c" >/dev/null 2>&1; then
      echo "$c"
      return 0
    fi
  done
  echo "error: no ${name} for ${TRIPLE}." >&2
  if [ "${ARCH}" = "arm64" ]; then
    echo "Install llvm-mingw and put ${TRIPLE}-clang on PATH." >&2
  else
    echo "Install the cross compiler with:  brew install mingw-w64 zstd" >&2
  fi
  exit 1
}

CC="$(pick_tool CC "${TRIPLE}-gcc" "${TRIPLE}-clang")"
CXX="$(pick_tool CXX "${TRIPLE}-g++" "${TRIPLE}-clang++")"
WINDRES="$(pick_tool WINDRES "${TRIPLE}-windres" "windres")"
AR="$(pick_tool AR "${TRIPLE}-ar" "llvm-ar")"
RANLIB="$(pick_tool RANLIB "${TRIPLE}-ranlib" "llvm-ranlib")"

if ! command -v curl >/dev/null 2>&1; then
  echo "error: curl is required" >&2
  exit 1
fi

latest_msys_pkg() {
  local pkg="$1"
  local html
  html="$(curl -fsSL "${MSYS_MIRROR}/${MSYS_REPO}/")"
  echo "${html}" | grep -oE "${pkg}-[0-9][^\"<> ]*-any\\.pkg\\.tar\\.zst" \
    | grep -v -- '-debug-' | sort -V | tail -1
}

extract_pkg() {
  local file="$1"
  if tar --zstd -tf "${file}" >/dev/null 2>&1; then
    tar --zstd -xf "${file}"
  elif command -v unzstd >/dev/null 2>&1; then
    unzstd -c "${file}" | tar -xf -
  else
    echo "error: need tar --zstd or unzstd (brew install zstd)" >&2
    exit 1
  fi
}

fetch_pkg() {
  local pkg="$1"
  local name url
  name="$(latest_msys_pkg "${pkg}")"
  if [ -z "${name}" ]; then
    echo "error: could not find ${pkg} in ${MSYS_REPO}" >&2
    exit 1
  fi
  url="${MSYS_MIRROR}/${MSYS_REPO}/${name}"
  echo "Fetching ${name}"
  curl -fsSL -o "${name}" "${url}"
  extract_pkg "${name}"
  rm -f "${name}"
}

PREFIX="${CACHE}/${EXTRACT_ROOT}"
if [ ! -f "${PREFIX}/lib/libSDL.dll.a" ] && [ ! -f "${PREFIX}/lib/libSDL.a" ]; then
  echo "Downloading ${ARCH} SDL from MSYS2 into ${CACHE}"
  mkdir -p "${CACHE}"
  (
    cd "${CACHE}"
    rm -rf "${EXTRACT_ROOT}"
    fetch_pkg "${MSYS_PFX}-sdl12-compat"
    fetch_pkg "${MSYS_PFX}-SDL2"
  )
fi

if [ ! -d "${PREFIX}" ]; then
  echo "error: expected MSYS2 prefix ${PREFIX}" >&2
  exit 1
fi

echo "Building Windows ${ARCH} with ${CXX}"
make -C "${SCRIPT_DIR}" \
  ARCH="${MAKE_ARCH}" \
  CROSS=1 \
  CC="${CC}" \
  CXX="${CXX}" \
  WINDRES="${WINDRES}" \
  AR="${AR}" \
  RANLIB="${RANLIB}" \
  CC_FOR_BUILD="${CC_FOR_BUILD:-cc}" \
  CXX_FOR_BUILD="${CXX_FOR_BUILD:-c++}" \
  SDL_PREFIX="${PREFIX}" \
  -j"${JOBS}"

# Same idea as CI packaging: copy arch-matching runtime DLLs next to the
# exe (SDL, SDL2, libwinpthread, libgcc, …) from the MinGW toolchain and
# the MSYS2 prefix. Wine/Windows look in the exe directory first.
TOOLBIN="$(cd "$(dirname "$(command -v "${CC}")")" && pwd)"
DLL_DIRS=(
  "${PREFIX}/bin"
  "${PREFIX}/lib"
  "${TOOLBIN}"
  "${TOOLBIN}/../${TRIPLE}/bin"
  "${TOOLBIN}/../${TRIPLE}/lib"
)
pthread_a="$("${CC}" -print-file-name=libwinpthread.a 2>/dev/null || true)"
if [ -n "${pthread_a}" ] && [ "${pthread_a}" != "libwinpthread.a" ] && [ -f "${pthread_a}" ]; then
  DLL_DIRS+=("$(dirname "${pthread_a}")")
  DLL_DIRS+=("$(dirname "${pthread_a}")/../bin")
fi

copy_runtime_dll() {
  local dll="$1"
  local dest="${SCRIPT_DIR}/${dll}"
  [ -f "${dest}" ] && return 0
  local d
  for d in "${DLL_DIRS[@]}"; do
    if [ -f "${d}/${dll}" ]; then
      cp "${d}/${dll}" "${dest}"
      echo "  copied ${dll} <- ${d}"
      return 0
    fi
  done
  return 1
}

echo "Staging runtime DLLs next to CockatriceIII.exe"
for dll in SDL.dll SDL2.dll libwinpthread-1.dll libgcc_s_seh-1.dll libgcc_s_dw2-1.dll libstdc++-6.dll libc++.dll libunwind.dll; do
  copy_runtime_dll "${dll}" || true
done

OBJDUMP="$(command -v "${TRIPLE}-objdump" || true)"
if [ -n "${OBJDUMP}" ]; then
  for _pass in 1 2 3; do
    for f in "${SCRIPT_DIR}/CockatriceIII.exe" "${SCRIPT_DIR}"/*.dll; do
      [ -f "${f}" ] || continue
      while IFS= read -r dll; do
        [ -n "${dll}" ] || continue
        copy_runtime_dll "${dll}" || true
      done < <("${OBJDUMP}" -p "${f}" 2>/dev/null | awk '/DLL Name:/ {print $3}')
    done
  done
fi

echo
echo "Built ${SCRIPT_DIR}/CockatriceIII.exe"
echo "Runtime DLLs staged beside the exe (including libwinpthread if required)."
echo "Run with:  wine ${SCRIPT_DIR}/CockatriceIII.exe"
