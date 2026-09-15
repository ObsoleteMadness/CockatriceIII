#!/usr/bin/env bash
# Cross-compile Cockatrice III for Windows using a Linux-hosted MinGW
# toolchain in Docker, instead of cross-from-macos.sh's Homebrew toolchain.
#
# Usage:
#   ./build-local-docker.sh [x64|x86]
#
# Why this exists: Homebrew's i686-w64-mingw32 GCC (used by
# cross-from-macos.sh) is SJLJ-exception-based, but the official Rust
# i686-pc-windows-gnu target's prebuilt std needs DWARF2 unwind symbols, so
# m68k-rs can't link for win32-x86 that way (see BasiliskII/mingw/Makefile).
# Ubuntu's mingw-w64 packages build i686 with DWARF2 instead, matching what
# that std expects -- the same reason win32-x86 CI can build m68k-rs (it
# uses MSYS2's own DWARF2 i686 GCC; see
# .github/workflows/build-and-release.yml). This script gets that same
# DWARF2 toolchain locally via Docker so win32-x86 + m68k-rs is testable
# without a real Windows machine.
#
# arm64 isn't supported here: Ubuntu has no aarch64-w64-mingw32 apt package.
# Use cross-from-macos.sh arm64 (needs an llvm-mingw toolchain on PATH) or
# real CI for that target.
#
# Prerequisites: Docker (or another docker-compatible daemon, e.g. OrbStack)
# running locally.

set -euo pipefail

ARCH="${1:-x64}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
IMAGE="cockatriceiii-mingw-build"

case "${ARCH}" in
  x64|x86_64|amd64|x86|i686|i386) ;;
  *)
    echo "Usage: $0 [x64|x86]" >&2
    echo "(arm64 has no apt mingw-w64 package; use cross-from-macos.sh arm64 with llvm-mingw instead)" >&2
    exit 2
    ;;
esac

if ! command -v docker >/dev/null 2>&1; then
  echo "error: docker is required (e.g. OrbStack, Docker Desktop)" >&2
  exit 1
fi

if ! docker info >/dev/null 2>&1; then
  echo "error: docker daemon is not reachable -- is it running?" >&2
  exit 1
fi

echo "Building ${IMAGE} image"
docker build -t "${IMAGE}" "${SCRIPT_DIR}/docker"

echo "Running build for ${ARCH} in container"
docker run --rm \
  -v "${REPO_ROOT}:/work" \
  "${IMAGE}" \
  "${ARCH}"
