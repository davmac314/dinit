#!/bin/sh

set -eu
cd "$(dirname "$0")"
. ../igr_functions.sh

spawn_dinit main

STAGE=1
if ! compare_cmd "run_dinitctl list" "expected1"; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/actual
    error "'run_dinitctl list' didn't contain expected result!" "Check $IGR_OUTPUT/output/actual"
fi
# both "main" and "secondary" should be running

STAGE=2
if ! compare_cmd "run_dinitctl rm-dep waits-for main secondary" "expected2"; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/actual
    error "'run_dinitctl rm-dep waits-for main secondary' didn't contain expected result!" "Check $IGR_OUTPUT/output/actual"
fi
# "secondary" should stop

STAGE=3
if ! compare_cmd "run_dinitctl list" "expected3"; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/actual
    error "'run_dinitctl list' didn't contain expected result!" "Check $IGR_OUTPUT/output/actual"
fi

STAGE=4
if ! compare_cmd "run_dinitctl add-dep waits-for main secondary" "expected4"; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/actual
    error "'run_dinitctl add-dep waits-for main secondary' didn't contain expected result!" "Check $IGR_OUTPUT/output/actual"
fi
# "secondary" will not automatically start, this is a waits-for dep

STAGE=5
if ! compare_cmd "run_dinitctl list" "expected3"; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/actual
    error "'run_dinitctl list' didn't contain expected result!" "Check $IGR_OUTPUT/output/actual"
fi

STAGE=6
if ! compare_cmd "run_dinitctl wake secondary" "expected5"; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/actual
    error "'run_dinitctl wake secondary' didn't contain expected result!" "Check $IGR_OUTPUT/output/actual"
fi
# if we wake "secondary" it should start and remain started

STAGE=7
if ! compare_cmd "run_dinitctl list" "expected1"; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/actual
    error "'run_dinitctl list' didn't contain expected result!" "Check $IGR_OUTPUT/output/actual"
fi

stop_dinit
exit 0
