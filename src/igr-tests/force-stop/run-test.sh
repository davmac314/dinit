#!/bin/sh

set -eu
cd "$(dirname "$0")"
. ../igr_functions.sh

spawn_dinit

if ! compare_cmd "run_dinitctl list" "expected-1"; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/actual-1
    error "'run_dinitctl list' didn't contain expected result!" "Check $IGR_OUTPUT/output/actual-1"
fi

if ! compare_cmd "run_dinitctl stop critical" "expected-2.err" err; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/actual-2.err
    error "'run_dinitctl stop critical 2>&1' didn't contain expected result!" "Check $IGR_OUTPUT/output/actual-2.err"
fi

if ! compare_cmd "run_dinitctl stop --force critical" "expected-3" err; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/actual-3
    error "'run_dinitctl stop --force critical 2>&1' didn't contain expected result!" "Check $IGR_OUTPUT/output/actual-3"
fi

# Note dinit should shutdown since all services stopped.
wait "$DINITPID"

exit 0
