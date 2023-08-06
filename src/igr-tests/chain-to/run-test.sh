#!/bin/sh

set -eu
cd "$(dirname "$0")"
. ../igr_functions.sh

rm -f "$IGR_OUTPUT"/recorded-output

spawn_dinit_oneshot part1

if ! compare_file "$IGR_OUTPUT"/recorded-output expected-output; then
    error "$IGR_OUTPUT/recorded-output didn't contain expected result!"
fi

exit 0
