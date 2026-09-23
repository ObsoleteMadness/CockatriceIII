#!/usr/bin/env bash
# Local Linux build in Docker, the same way the CI Linux job builds.
#
#   scripts/build-linux-docker.sh                  # host arch, build + gate tests
#   scripts/build-linux-docker.sh --arch amd64     # x86_64 (emulated on Apple Silicon)
#   scripts/build-linux-docker.sh --arch arm64     # aarch64
#   scripts/build-linux-docker.sh --cpu-tests      # also run the engine accuracy suite
#   scripts/build-linux-docker.sh --no-tests       # just build
#   scripts/build-linux-docker.sh --out DIR        # copy the Linux binary to DIR
#
# The working tree (tracked files and submodules, including uncommitted
# edits) is streamed into a fresh container, so nothing the Linux build
# produces lands in your checkout. Needs a running Docker daemon.
#
# Under emulation (--arch amd64 on Apple Silicon) the x86 JIT also fails
# mc68000/rox.bin; real x86_64 hardware does not, so treat that as an
# emulator artefact.

set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"

case "$(uname -m)" in
arm64 | aarch64) ARCH=arm64 ;;
*)               ARCH=amd64 ;;
esac
RUN_TESTS=1
RUN_CPU_TESTS=0
OUT_DIR=""

while [ $# -gt 0 ]; do
	case "$1" in
	--arch)      ARCH="${2:?--arch needs arm64 or amd64}"; shift ;;
	--no-tests)  RUN_TESTS=0 ;;
	--cpu-tests) RUN_CPU_TESTS=1 ;;
	--out)       OUT_DIR="${2:?--out needs a directory}"; shift ;;
	-h | --help)
		sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*)
		echo "build-linux-docker: unknown option '$1' (try --help)" >&2
		exit 1
		;;
	esac
	shift
done

case "$ARCH" in
arm64 | amd64) ;;
*) echo "build-linux-docker: --arch must be arm64 or amd64" >&2; exit 1 ;;
esac

if ! docker info >/dev/null 2>&1; then
	echo "build-linux-docker: the Docker daemon is not running" >&2
	exit 1
fi

IMAGE="cockatrice-linux-build:$ARCH"
echo "==> image $IMAGE"
docker build -q --platform "linux/$ARCH" -t "$IMAGE" \
	-f "$ROOT/scripts/docker/linux.Dockerfile" "$ROOT/scripts/docker" >/dev/null

# The container writes the binary here when --out is given
DOCKER_ARGS=(--rm -i --platform "linux/$ARCH")
if [ -n "$OUT_DIR" ]; then
	mkdir -p "$OUT_DIR"
	DOCKER_ARGS+=(-v "$(CDPATH= cd -- "$OUT_DIR" && pwd):/out")
fi

echo "==> build (linux/$ARCH)"
cd "$ROOT"
# macOS tar would otherwise add AppleDouble files and xattr headers
git ls-files -z --recurse-submodules |
	COPYFILE_DISABLE=1 tar --no-mac-metadata --null -T - -cf - 2>/dev/null |
	docker run "${DOCKER_ARGS[@]}" \
		-e RUN_TESTS="$RUN_TESTS" -e RUN_CPU_TESTS="$RUN_CPU_TESTS" \
		"$IMAGE" bash -euo pipefail -c '
			tar -xf - -C /work 2>/dev/null
			cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
			cmake --build build -j"$(nproc)"
			if [ -d /out ]; then
				cp build/BasiliskII/CockatriceIII /out/
				echo "==> copied CockatriceIII to the --out directory"
			fi
			if [ "$RUN_TESTS" = 1 ]; then
				echo "==> gate tests"
				ctest --test-dir build -L gate --output-on-failure
			fi
			if [ "$RUN_CPU_TESTS" = 1 ]; then
				# Reported, never gated: 20 known failures (see BasiliskII/tests/README.md)
				echo "==> cpu tests (informational; 20 known failures)"
				ctest --test-dir build -L cpu --output-on-failure || true
			fi
		'
