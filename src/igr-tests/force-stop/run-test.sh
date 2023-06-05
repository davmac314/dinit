#!/bin/sh

set -e
. "$IGR_FUNCTIONS"

spawn_dinit

if ! compare_cmd "dinitctl list" "expected-1"; then
    echo "$CMD_OUT" > "$TEMP"/output/actual-1
    stop_dinit
    exit 1
fi

if ! compare_cmd "dinitctl stop critical" "expected-2.err" err; then
    echo "$CMD_OUT" > "$TEMP"/output/actual-2.err
    stop_dinit
    exit 1
fi

if ! compare_cmd "dinitctl stop --force critical" "expected-3" err; then
    echo "$CMD_OUT" > "$TEMP"/output/actual-3
    stop_dinit
    exit 1;
fi

# Note dinit should shutdown since all services stopped.
wait "$DINITPID"

# Passed:
exit 0
