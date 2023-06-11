#!/bin/sh

set -eu
. "$IGR_FUNCTIONS"

rm -f "$IGR_OUTPUT"/output/args-record

export TEST_VAR_ONE="var one" TEST_VAR_TWO=vartwo TEST_VAR_THREE=varthree
spawn_dinit_oneshot checkargs

if ! compare_text "$IGR_OUTPUT"/output/args-record "1:xxxvar one/yyy 2:vartwovarthree 3:varfour 4:"; then
    error "$IGR_OUTPUT/output/args-record didn't contain expected result!"
fi

exit 0
