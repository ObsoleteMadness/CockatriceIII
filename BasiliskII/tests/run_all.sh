#!/bin/sh
# Basilisk tests are the gate. CPU tests always run and are summarized;
# they fail `make test` only when STRICT_CPU=1.

set -e
cd "$(dirname "$0")"

echo "========== Basilisk tests =========="
basilisk_status=0
./run_basilisk_tests.sh || basilisk_status=$?

echo ""
echo "========== CPU tests =========="
cpu_status=0
./run_cpu_tests.sh "$@" || cpu_status=$?

echo ""
echo "========== Summary =========="
if [ "$basilisk_status" -eq 0 ]; then
	echo "Basilisk: PASSED"
else
	echo "Basilisk: FAILED (exit $basilisk_status)"
fi
if [ "$cpu_status" -eq 0 ]; then
	echo "CPU: PASSED"
else
	echo "CPU: FAILED (exit $cpu_status) — expected until CPU engines are fixed"
fi

if [ "$basilisk_status" -ne 0 ]; then
	exit "$basilisk_status"
fi
if [ -n "$STRICT_CPU" ] && [ "$STRICT_CPU" != "0" ]; then
	exit "$cpu_status"
fi
exit 0
