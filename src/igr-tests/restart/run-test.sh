#!/bin/sh

set -eu
cd "$(dirname "$0")"
. ../igr_functions.sh

rm -f "$IGR_OUTPUT"/basic-ran

spawn_dinit

# wait until parent (and therefore 'basic') has fully started.
run_dinitctl $QUIET start boot

sleep 0.1 # time for file to be written.

if ! compare_text "$IGR_OUTPUT"/basic-ran "ran"; then
    error "$IGR_OUTPUT/basic-ran didn't contain expected result!"
fi

rm "$IGR_OUTPUT"/basic-ran

run_dinitctl $QUIET restart basic
sleep 0.1 # time for file write.

if ! compare_text "$IGR_OUTPUT"/basic-ran "ran"; then
    error "$IGR_OUTPUT/basic-ran didn't contain expected result!"
fi

stop_dinit
exit 0
