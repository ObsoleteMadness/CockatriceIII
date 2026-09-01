#!/bin/sh
# Run each basilisk_*_test binary with a 30s spin timeout.

set -e
cd "$(dirname "$0")"
timeout_cmd="./run_with_timeout.sh"
fail=0

for t in basilisk_memory_test basilisk_engine_test basilisk_emulop_test \
	basilisk_patches_test basilisk_scsi_test basilisk_scc_test basilisk_disk_test
do
	if [ ! -x "./$t" ]; then
		echo "MISSING $t (build first with make)"
		fail=1
		continue
	fi
	echo "---- $t ----"
	if ! TEST_TIMEOUT="${TEST_TIMEOUT:-30}" "$timeout_cmd" "./$t"; then
		echo "$t FAILED"
		fail=1
	fi
done

if [ "$fail" -ne 0 ]; then
	echo "Basilisk tests: FAILED"
	exit 1
fi
echo "Basilisk tests: PASSED"
exit 0
