#!/bin/sh

set -eu
. "$IGR_FUNCTIONS"

rm -f "$IGR_OUTPUT"/output/basic-ran

spawn_dinit

# wait until parent (and therefore 'basic') has fully started.
run_dinitctl $QUIET start boot

sleep 0.1 # time for file to be written.

if ! compare_text "$IGR_OUTPUT"/output/basic-ran "ran"; then
    error "$IGR_OUTPUT/output/basic-ran didn't contain expected result!"
fi

rm "$IGR_OUTPUT"/output/basic-ran

run_dinitctl $QUIET restart basic
sleep 0.1 # time for file write.

if ! compare_text "$IGR_OUTPUT"/output/basic-ran "ran"; then
    error "$IGR_OUTPUT/output/basic-ran didn't contain expected result!"
fi

stop_dinit
exit 0
