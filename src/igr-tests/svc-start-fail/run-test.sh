#!/bin/sh

set -eu
cd "$(dirname "$0")"
. ../igr_functions.sh

spawn_dinit

if ! compare_cmd "run_dinitctl start bad-command" "expected-1" err; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/actual-1
    error "'run_dinitctl start bad-command 2>&1' didn't contain expected result!" "Check $IGR_OUTPUT/actual-1"
fi

if ! compare_cmd "run_dinitctl start timeout-command" "expected-2" err; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/actual-2
    error "'run_dinitctl start timeout-command 2>&1' didn't contain expected result!" "Check $IGR_OUTPUT/actual-2"
fi

stop_dinit

exit 0
