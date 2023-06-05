#!/bin/sh

set -e
. "$IGR_FUNCTIONS"

rm -f "$TEMP"/output/args-record

export TEST_VAR_ONE="var one" TEST_VAR_TWO=vartwo TEST_VAR_THREE=varthree
spawn_dinit_oneshot checkargs

if ! compare_text "$TEMP"/output/args-record "1:xxxvar one/yyy 2:vartwovarthree 3:varfour 4:"; then
    error "$TEMP/output/args-record didn't contain expected result!"
fi

exit 0
