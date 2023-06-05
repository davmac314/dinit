#!/bin/sh

set -e
. "$IGR_FUNCTIONS"

rm -f "$TEMP"/output/basic-ran

spawn_dinit

# wait until parent (and therefore 'basic') has fully started
dinitctl --quiet start boot

sleep 0.1 # time for file to be written

if ! compare_text "$TEMP"/output/basic-ran "ran"; then
    stop_dinit
    error "$TEMP/output/basic-ran didn't contain expected result!"
fi

rm "$TEMP"/output/basic-ran

dinitctl --quiet restart basic
sleep 0.1 # time for file write

if ! compare_text "$TEMP"/output/basic-ran "ran"; then
    stop_dinit
    error "$TEMP/output/basic-ran didn't contain expected result!"
fi

stop_dinit
exit 0
