#!/bin/sh

set -e
. "$IGR_FUNCTIONS"

rm -f "$TEMP"/output/basic-ran

spawn_dinit

# try to start "bad-command" which will fail
#DINITCTLOUT="$("$DINITCTL_EXEC" -p socket start bad-command 2>&1)"
#if [ "$DINITCTLOUT" != "$(cat expected-1)" ]; then
if ! compare_cmd "dinitctl start bad-command" "expected-1" err; then
    echo "$CMD_OUT" > "$TEMP"/output/actual-1
    stop_dinit
    exit 1
fi

# try to start command which will timeout
#DINITCTLOUT="$("$DINITCTL_EXEC" -p socket start timeout-command 2>&1)"
#if [ "$DINITCTLOUT" != "$(cat expected-2)" ]; then
if ! compare_cmd "dinitctl start timeout-command" "expected-2" err; then
    echo "$CMD_OUT" > "$TEMP"/output/actual-2
    stop_dinit
    exit 1
fi

stop_dinit

exit 0
