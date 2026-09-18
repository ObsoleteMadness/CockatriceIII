#!/usr/bin/env bash
# Serialised ad-hoc codesign for the JIT-entitled binaries.
#
# Usage: codesign-jit.sh <entitlements.plist> <binary>
#
# Why this exists
# ---------------
# Every executable here gets a POST_BUILD `codesign --force`, so `make -j`
# runs several of them at once. Roughly one build in five at -j8 (never at
# -j1), a single codesign invocation prints
#
#     <path>: replacing existing signature
#
# and then, after other codesign processes have run, the same invocation
# reports
#
#     <path>: No such file or directory
#
# and exits non-zero. make then deletes the target, so the build fails with a
# perfectly good binary having been linked and signed moments earlier.
# codesign --force stages the signed image through a temporary file and renames
# it into place; that write-back is what fails while other codesign processes
# are active.
#
# Serialising the signing removes the interference without hiding anything: a
# codesign that fails for a real reason (bad entitlements, missing binary,
# unsigned-able file) still fails the build exactly as before. Only the overlap
# is removed.
#
# macOS ships no flock(1), so the mutex is a lock directory -- mkdir is atomic
# on every filesystem this project builds on.

set -euo pipefail

ENTITLEMENTS="${1:?usage: $0 <entitlements.plist> <binary>}"
BINARY="${2:?usage: $0 <entitlements.plist> <binary>}"

LOCK="${TMPDIR:-/tmp}/cockatrice-codesign.lock"
# Must stay below the total wait below (900 * 0.1s = 90s), or a leaked lock
# could never be reclaimed within a single invocation.
STALE_SECONDS=45

for _attempt in $(seq 1 900); do
	if mkdir "$LOCK" 2>/dev/null; then
		# Deliberately not `exec codesign ...`: exec replaces this shell, so the
		# EXIT trap would never run, the lock would leak, and every later signer
		# would starve waiting for it.
		trap 'rmdir "$LOCK" 2>/dev/null || true' EXIT INT TERM
		status=0
		codesign -s - --force --entitlements "$ENTITLEMENTS" "$BINARY" || status=$?
		rmdir "$LOCK" 2>/dev/null || true
		trap - EXIT INT TERM
		exit "$status"
	fi

	# A build killed mid-sign would otherwise wedge every later build, so drop
	# a lock that is clearly older than any real codesign run.
	if [ -d "$LOCK" ]; then
		lock_age=$(( $(date +%s) - $(stat -f %m "$LOCK" 2>/dev/null || date +%s) ))
		if [ "$lock_age" -gt "$STALE_SECONDS" ]; then
			echo "codesign-jit: removing stale lock ${LOCK} (${lock_age}s old)" >&2
			rmdir "$LOCK" 2>/dev/null || true
		fi
	fi

	sleep 0.1
done

echo "codesign-jit: gave up waiting for ${LOCK}" >&2
exit 1
