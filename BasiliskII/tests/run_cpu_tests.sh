#!/bin/sh
# CPU suite. Per-test isolation is inside cpu_tests (30s). This script only
# wraps the binary when a single --engine is requested (so a wedged engine
# cannot run forever). A full multi-engine run is not capped at 30s.

set -e
cd "$(dirname "$0")"

if [ ! -x ./cpu_tests ]; then
	echo "MISSING cpu_tests (build first with make)"
	exit 1
fi

if [ -n "$1" ]; then
	# Per-engine invocation: 30s is too short for a whole engine battery.
	# Inner run_isolated still applies 30s per test/image.
	exec ./cpu_tests --engine "$1"
fi

exec ./cpu_tests
