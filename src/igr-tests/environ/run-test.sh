#!/bin/sh

set -eu
. "$IGR_FUNCTIONS"

rm -f "$IGR_OUTPUT"/output/env-record

find_dinitctl # Scripts need dinitctl.
export OUTPUT="$IGR_OUTPUT/output/env-record"

spawn_dinit_oneshot -e environment1 checkenv
spawn_dinit_oneshot -e environment2 checkenv
spawn_dinit_oneshot setenv1

if ! compare_text "$OUTPUT" "$(echo "$SOCKET";\
                             echo checkenv;\
                             echo gotenv1;\
                             echo hello;\
                             echo gotenv2;\
                             echo goodbye;\
                             echo 3;\
                             echo 2;\
                             echo 1)"
then
    error "$OUTPUT didn't contain expected result!"
fi

exit 0
