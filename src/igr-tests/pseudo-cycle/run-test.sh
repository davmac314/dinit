#!/bin/sh

set -eu
cd "$(dirname "$0")"
. ../igr_functions.sh

rm -rf "$IGR_OUTPUT"/output/*

spawn_dinit_oneshot

if ! compare_text "$IGR_OUTPUT"/output/svc-script "ran"; then
    error "$IGR_OUTPUT/output/svc-script didn't contain expected result!"
fi

exit 0
