#!/usr/bin/env bash
# Runs inside the cockatriceiii-mingw-build image (see Dockerfile). Invoked by
# ../build-local-docker.sh with the repo bind-mounted at /work.
#
# Usage (inside the container): build.sh [x64|x86]

set -euo pipefail

ARCH="${1:-x64}"
SCRIPT_DIR="/work/BasiliskII/mingw"
JOBS="$(nproc)"
MSYS_MIRROR="${MSYS_MIRROR:-https://repo.msys2.org/mingw}"

case "${ARCH}" in
  x64|x86_64|amd64)
    ARCH=x64
    MAKE_ARCH=x64
    TRIPLE="x86_64-w64-mingw32"
    MSYS_REPO="mingw64"
    MSYS_PFX="mingw-w64-x86_64"
    ;;
  x86|i686|i386)
    ARCH=x86
    MAKE_ARCH=x86
    TRIPLE="i686-w64-mingw32"
    MSYS_REPO="mingw32"
    MSYS_PFX="mingw-w64-i686"
    ;;
  *)
    echo "Usage: build.sh [x64|x86] (arm64 has no apt mingw-w64 package; use cross-from-macos.sh arm64 with llvm-mingw instead)" >&2
    exit 2
    ;;
esac

CACHE="${SCRIPT_DIR}/.cross-win/${ARCH}"
PREFIX="${CACHE}/${MSYS_REPO}"

if ! command -v "${TRIPLE}-gcc" >/dev/null 2>&1; then
  echo "error: ${TRIPLE}-gcc not found in this image" >&2
  exit 1
fi

# SDL is pulled from the matching MSYS2 repo, same as cross-from-macos.sh, and
# the cache dir (.cross-win/<arch>/) is shared with that script -- run either
# one first and the other reuses what it downloaded.
latest_msys_pkg() {
  local pkg="$1"
  local html
  html="$(curl -fsSL "${MSYS_MIRROR}/${MSYS_REPO}/")"
  echo "${html}" | grep -oE "${pkg}-[0-9][^\"<> ]*-any\\.pkg\\.tar\\.zst" \
    | grep -v -- '-debug-' | sort -V | tail -1
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
  tar --zstd -xf "${name}"
  rm -f "${name}"
}

if [ ! -f "${PREFIX}/lib/libSDL.dll.a" ] && [ ! -f "${PREFIX}/lib/libSDL.a" ]; then
  echo "Downloading ${ARCH} SDL from MSYS2 into ${CACHE}"
  mkdir -p "${CACHE}"
  (
    cd "${CACHE}"
    rm -rf "${MSYS_REPO}"
    fetch_pkg "${MSYS_PFX}-sdl12-compat"
    fetch_pkg "${MSYS_PFX}-SDL2"
  )
fi

if [ ! -d "${PREFIX}" ]; then
  echo "error: expected MSYS2 prefix ${PREFIX}" >&2
  exit 1
fi

# obj/ and cockatricerc.o are shared by every arch (and with cross-from-macos.sh
# runs on the host, since .cross-win/ and obj/ are bind-mounted from the same
# working tree) -- wipe them on an arch switch so a previous run's objects
# never get fed to the wrong linker.
ARCH_STAMP="${SCRIPT_DIR}/.cross-win/.last-arch"
if [ ! -f "${ARCH_STAMP}" ] || [ "$(cat "${ARCH_STAMP}")" != "docker-${ARCH}" ]; then
  if [ -f "${ARCH_STAMP}" ]; then
    echo "Switching from $(cat "${ARCH_STAMP}") to docker-${ARCH}; clearing stale objects"
  fi
  make -C "${SCRIPT_DIR}" mostlyclean >/dev/null 2>&1 || true
  rm -f "${SCRIPT_DIR}"/*.dll
fi
mkdir -p "$(dirname "${ARCH_STAMP}")"
echo "docker-${ARCH}" > "${ARCH_STAMP}"

EXTRA_MAKE_VARS=()
if [ "${ARCH}" = "x86" ]; then
  # The whole point of building here instead of via cross-from-macos.sh:
  # Ubuntu's i686-w64-mingw32-gcc is DWARF2-exception-based, matching what
  # Rust's prebuilt i686-pc-windows-gnu std expects (unlike Homebrew's macOS
  # cross toolchain, which is SJLJ-based and can't link it -- see
  # BasiliskII/mingw/Makefile). Confirmed working the same way in CI: MSYS2's
  # own i686 GCC is DWARF2 too (.github/workflows/build-and-release.yml).
  EXTRA_MAKE_VARS+=(ENABLE_M68K_RS_CPU=1)
fi

echo "Building Windows ${ARCH} with ${TRIPLE}-g++ (Docker/Ubuntu toolchain)"
make -C "${SCRIPT_DIR}" \
  ARCH="${MAKE_ARCH}" \
  CROSS=1 \
  TARGET="${TRIPLE}" \
  CC="${TRIPLE}-gcc" \
  CXX="${TRIPLE}-g++" \
  WINDRES="${TRIPLE}-windres" \
  AR="${TRIPLE}-ar" \
  RANLIB="${TRIPLE}-ranlib" \
  CC_FOR_BUILD=cc \
  CXX_FOR_BUILD=c++ \
  SDL_PREFIX="${PREFIX}" \
  "${EXTRA_MAKE_VARS[@]}" \
  -j"${JOBS}"

# Same idea as cross-from-macos.sh / CI packaging: copy arch-matching runtime
# DLLs next to the exe so Wine/Windows find them without needing PATH set up.
DLL_DIRS=(
  "${PREFIX}/bin"
  "${PREFIX}/lib"
  "/usr/${TRIPLE}/lib"
  "/usr/${TRIPLE}/bin"
  "/usr/lib/gcc/${TRIPLE}"/*
)

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
for dll in SDL.dll SDL2.dll libwinpthread-1.dll libgcc_s_seh-1.dll libgcc_s_dw2-1.dll libstdc++-6.dll; do
  copy_runtime_dll "${dll}" || true
done

for _pass in 1 2 3; do
  for f in "${SCRIPT_DIR}/CockatriceIII.exe" "${SCRIPT_DIR}"/*.dll; do
    [ -f "${f}" ] || continue
    while IFS= read -r dll; do
      [ -n "${dll}" ] || continue
      copy_runtime_dll "${dll}" || true
    done < <("${TRIPLE}-objdump" -p "${f}" 2>/dev/null | awk '/DLL Name:/ {print $3}')
  done
done

echo
echo "Built ${SCRIPT_DIR}/CockatriceIII.exe"
echo "Run with:  wine ${SCRIPT_DIR}/CockatriceIII.exe"
