#!/bin/sh

set -e
. "$IGR_FUNCTIONS"

rm -rf "$TEMP"/output/*

spawn_dinit_oneshot

if ! compare_text "$TEMP"/output/svc-script "ran"; then
    error "$TEMP/output/svc-script didn't contain expected result!"
fi

exit 0
