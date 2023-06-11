#!/bin/sh

set -eu
. "$IGR_FUNCTIONS"

rm -f "$IGR_OUTPUT"/output/recorded-output

spawn_dinit_oneshot part1

if ! compare_file "$IGR_OUTPUT"/output/recorded-output expected-output; then
    error "$IGR_OUTPUT/output/recorded-output didn't contain expected result!"
fi

exit 0
