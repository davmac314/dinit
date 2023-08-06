#!/bin/sh

set -eu
cd "$(dirname "$0")"
. ../igr_functions.sh

# Similar to reload1 test, but with boot service stopped while we reload.

# Start with boot depending on a,b
rm -rf sd
cp -R sd1 sd

spawn_dinit

run_dinitctl $QUIET start hold

if ! compare_cmd "run_dinitctl list" "initial.expected"; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/initial.actual
    error "'run_dinitctl list' didn't contain expected result!" "Check $IGR_OUTPUT/inital.actual"
fi

run_dinitctl $QUIET stop boot

# Put alternate descriptions in place: boot depends on b, c
rm -rf sd
cp -R sd2 sd

# This should succeed since boot is stopped
if ! compare_cmd "run_dinitctl reload boot" "output2.expected" err; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/output2.actual
    error "'run_dinitctl reload boot 2>&1' didn't contain expected result" "Check $IGR_OUTPUT/output2.actual"
fi

run_dinitctl $QUIET start boot
if ! compare_cmd "run_dinitctl list" "output3.expected"; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/output3.actual
    error "'run_dinitctl list' didn't contain expected result" "Check $IGR_OUTPUT/output3.actual"
fi

stop_dinit

exit 0
