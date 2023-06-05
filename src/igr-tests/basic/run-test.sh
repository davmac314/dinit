#!/bin/sh

set -e
. "$IGR_FUNCTIONS"

rm -f "$TEMP"/output/basic-ran

spawn_dinit_oneshot

if ! compare_text "$TEMP"/output/basic-ran "ran"; then
    error "$TEMP/output/basic-ran didn't contain expected result!"
fi

exit 0
