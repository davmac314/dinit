#!/bin/sh
#
# Check that a service with a missing dependency causes the appropriate error.
#

set -eu

cd "$(dirname "$0")"

"$DINIT_EXEC" -d sd -u -p socket -q &
DINITPID=$!

# Give some time for startup
sleep 0.2

STATUS=PASS

DINITCTLOUT="$("$DINITCTL_EXEC" -p socket start missing-dep-svc 2>&1 || true)"
if [ "$DINITCTLOUT" != "$(cat output.expected)" ]; then
    echo "$DINITCTLOUT" > output.actual
    STATUS=FAIL
fi

"$DINITCTL_EXEC" --quiet -p socket shutdown
wait $DINITPID

if [ $STATUS = PASS ]; then exit 0; fi
exit 1
