#!/bin/sh

set -eu
cd "$(dirname "$0")"
. ../igr_functions.sh

rm -rf "$IGR_OUTPUT"/*

spawn_dinit_oneshot

if ! compare_text "$IGR_OUTPUT"/svc-script "ran"; then
    error "$IGR_OUTPUT/svc-script didn't contain expected result!"
fi

exit 0
