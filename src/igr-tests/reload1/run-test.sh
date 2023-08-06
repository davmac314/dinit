#!/bin/sh

set -eu
cd "$(dirname "$0")"
. ../igr_functions.sh

# Start with boot depending on a,b
rm -rf sd
cp -R sd1 sd

spawn_dinit

if ! compare_cmd "run_dinitctl list" "initial.expected"; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/initial.actual
    error "'run_dinitctl list' didn't contain expected result!" "Check $IGR_OUTPUT/initial.actual"
fi

# Put alternate descriptions in place: boot depends on b, c
rm -rf sd
cp -R sd2 sd
# First attempt should fail, c not started
if ! compare_cmd "run_dinitctl --quiet reload boot" "output2.expected" err; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/output2.actual
    error "'run_dinitctl --quiet reload boot 2>&1' didn't contain expected result" "Check $IGR_OUTPUT/output2.expected"
fi
run_dinitctl $QUIET start c
run_dinitctl $QUIET reload boot
if ! compare_cmd "run_dinitctl --quiet list" "output3.expected"; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/output3.actual
    error "'run_dinitctl --quiet list' didn't contain expected result!" "Check $IGR_OUTPUT/output3.actual"
fi

stop_dinit

exit 0
