#!/bin/sh

set -e
. "$IGR_FUNCTIONS"

rm -f "$TEMP"/output/recorded-output

spawn_dinit_oneshot part1

if ! compare_file "$TEMP"/output/recorded-output expected-output; then
    error "$TEMP/output/recorded-output didn't contain expected result!"
fi

exit 0
