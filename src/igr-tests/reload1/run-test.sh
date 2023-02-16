#!/bin/sh

cd "$(dirname "$0")"

# Start with boot depending on a,b
rm -rf sd
cp -R sd1 sd

"$DINIT_EXEC" -d sd -u -p socket -q &
DINITPID=$!

# Give some time for startup
sleep 0.2

STATUS=PASS

DINITCTLOUT="$("$DINITCTL_EXEC" -p socket list)"
if [ "$DINITCTLOUT" != "$(cat initial.expected)" ]; then
    echo "$DINITCTLOUT" > initial.actual
    STATUS=FAIL
fi

# Put alternate descriptions in place: boot depends on b, c
if [ "$STATUS" = PASS ]; then
    rm -rf sd
    cp -R sd2 sd

    # First attempt should fail, c not started
    DINITCTLOUT="$("$DINITCTL_EXEC" --quiet -p socket reload boot 2>&1)"
    if [ "$DINITCTLOUT" != "$(cat output2.expected)" ]; then
        echo "$DINITCTLOUT" > output2.actual
        STATUS=FAIL
    fi

fi

if [ "$STATUS" = PASS ]; then
    "$DINITCTL_EXEC" --quiet -p socket start c
    "$DINITCTL_EXEC" --quiet -p socket reload boot
    DINITCTLOUT="$("$DINITCTL_EXEC" --quiet -p socket list)"
    if [ "$DINITCTLOUT" != "$(cat output3.expected)" ]; then
        echo "$DINITCTLOUT" > output3.actual
        STATUS=FAIL
    fi
fi

"$DINITCTL_EXEC" --quiet -p socket shutdown
wait $DINITPID

if [ $STATUS = PASS ]; then exit 0; fi
exit 1
