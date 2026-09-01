#!/bin/sh
# Run a command with a wall-clock timeout (default 30s). Exit 124 on timeout.
# TEST_TIMEOUT overrides the default. Kills the whole process group so a
# spinning Execute68k child cannot outlive the wrapper.

timeout="${TEST_TIMEOUT:-30}"
if [ "$#" -lt 1 ]; then
	echo "usage: $0 command [args...]" >&2
	exit 2
fi

exec perl -e '
	my $timeout = shift;
	$SIG{ALRM} = sub {
		kill "KILL", -$pid if $pid;
		exit 124;
	};
	$pid = fork();
	if ($pid == 0) {
		setpgrp(0, 0);
		exec @ARGV;
		exit 127;
	}
	alarm $timeout;
	waitpid($pid, 0);
	my $status = $?;
	if ($status & 127) {
		exit 1;
	}
	exit ($status >> 8);
' "$timeout" "$@"
