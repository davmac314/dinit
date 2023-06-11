#!/bin/sh

set -eu
. "$IGR_FUNCTIONS"

export OUTPUT="$IGR_OUTPUT/output/env-record"
rm -f "$OUTPUT"

export TEST_VAR_TWO=set-via-script

for i in 1 2 3; do
	spawn_dinit_oneshot checkenv$i
done
spawn_dinit_oneshot -e dinit-environment checkenv4
spawn_dinit_oneshot checkenv4

if ! compare_file "$OUTPUT" env-expected; then
    error "$OUTPUT didn't contain expected result!"
fi

exit 0
