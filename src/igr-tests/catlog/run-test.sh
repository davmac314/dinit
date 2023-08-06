#!/bin/sh

set -eu
cd "$(dirname "$0")"
. ../igr_functions.sh

spawn_dinit $QUIET -d sd

run_dinitctl $QUIET start output

capture_exact_output log_output   run_dinitctl catlog output
capture_exact_output expected_output    printf "%s\\n" "Output..."

if [ "$log_output" != "$expected_output" ]; then
    error "catlog output didn't contain match expected output"
fi

# check output again, this time also clear the buffer
capture_exact_output log_output  run_dinitctl catlog --clear output
if [ "$log_output" != "$expected_output" ]; then
    error "catlog output didn't contain match expected output"
fi

# check a third time, buffer should be clear
capture_exact_output log_output  run_dinitctl catlog output
if [ "$log_output" != "" ]; then
    error "catlog output didn't contain match expected output (clear not successful?)"
fi

stop_dinit
exit 0
