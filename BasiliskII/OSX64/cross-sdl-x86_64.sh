#!/usr/bin/env bash
# Compatibility wrapper: the committed Intel SDL prefix lives under
# dist/dependencies/osx/intel. Prefer that script when rebuilding.
set -euo pipefail
exec "$(cd "$(dirname "$0")/../../dist/dependencies/osx" && pwd)/rebuild-intel-sdl.sh" "$@"
