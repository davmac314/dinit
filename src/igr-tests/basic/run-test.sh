#!/bin/sh

set -eu
cd "$(dirname "$0")"
. ../igr_functions.sh

rm -f "$IGR_OUTPUT"/basic-ran

spawn_dinit_oneshot basic

if ! compare_text "$IGR_OUTPUT"/basic-ran "ran"; then
    error "$IGR_OUTPUT/basic-ran didn't contain expected result!"
fi

exit 0
