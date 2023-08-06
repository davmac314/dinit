#!/bin/sh
# Test for service consuming the output of another service via pipe.

set -eu
cd "$(dirname "$0")"
. ../igr_functions.sh

rm -rf "$IGR_OUTPUT"/*

spawn_dinit

sleep 0.5

# consumer should be running and opened output log file; nothing has been produced
compare_text "$IGR_OUTPUT/logged-output" ""

run_dinitctl $QUIET start producer
run_dinitctl $QUIET stop producer
sleep 0.2
EXPECTED_PRODUCER_OUTPUT="$()"
capture_exact_output EXPECTED_PRODUCER_OUTPUT printf '%s\n' "Producing output..."
compare_text_nl "$IGR_OUTPUT/logged-output" "$EXPECTED_PRODUCER_OUTPUT"

run_dinitctl $QUIET start producer
run_dinitctl $QUIET stop producer
sleep 0.2
compare_text_nl "$IGR_OUTPUT/logged-output" "${EXPECTED_PRODUCER_OUTPUT}${EXPECTED_PRODUCER_OUTPUT}"

stop_dinit
exit 0
