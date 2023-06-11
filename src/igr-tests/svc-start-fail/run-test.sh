#!/bin/sh

set -eu
. "$IGR_FUNCTIONS"

rm -f "$IGR_OUTPUT"/output/basic-ran

spawn_dinit

if ! compare_cmd "run_dinitctl start bad-command" "expected-1" err; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/output/actual-1
    error "'run_dinitctl start bad-command 2>&1' didn't contain expected result!" "Check $IGR_OUTPUT/output/actual-1"
fi

if ! compare_cmd "run_dinitctl start timeout-command" "expected-2" err; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/output/actual-2
    error "'run_dinitctl start timeout-command 2>&1' didn't contain expected result!" "Check $IGR_OUTPUT/output/actual-2"
fi

stop_dinit

exit 0
