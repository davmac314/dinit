#!/bin/sh

if [ $IS_MESON ]; then
   cd $(dirname $0)
   DINIT_EXEC=$APPS_PATH/dinit
   DINITCTL_EXEC=$APPS_PATH/dinitctl
else
   DINIT_EXEC=../../dinit
   DINITCTL_EXEC=../../dinitctl
fi

# Similar to reload1 test, but with boot service stopped while we reload.

# Start with boot depending on a,b
rm -rf sd
cp -R sd1 sd

$DINIT_EXEC -d sd -u -p socket -q &
DINITPID=$!

# Give some time for startup
sleep 0.2

$DINITCTL_EXEC --quiet -p socket start hold

STATUS=PASS

DINITCTLOUT="$($DINITCTL_EXEC -p socket list)"
if [ "$DINITCTLOUT" != "$(cat initial.expected)" ]; then
    echo "$DINITCTLOUT" > initial.actual
    STATUS=FAIL
fi

$DINITCTL_EXEC --quiet -p socket stop boot

# Put alternate descriptions in place: boot depends on b, c
if [ "$STATUS" = PASS ]; then
    rm -rf sd
    cp -R sd2 sd

    # This should succeed since boot is stopped
    DINITCTLOUT="$($DINITCTL_EXEC -p socket reload boot 2>&1)"
    if [ "$DINITCTLOUT" != "$(cat output2.expected)" ]; then
        echo "$DINITCTLOUT" > output2.actual
        STATUS=FAIL
    fi

fi

if [ "$STATUS" = PASS ]; then
    $DINITCTL_EXEC --quiet -p socket start boot
    DINITCTLOUT="$($DINITCTL_EXEC -p socket list)"
    if [ "$DINITCTLOUT" != "$(cat output3.expected)" ]; then
        echo "$DINITCTLOUT" > output3.actual
        STATUS=FAIL
    fi
fi

$DINITCTL_EXEC --quiet -p socket shutdown
wait $DINITPID

if [ $STATUS = PASS ]; then exit 0; fi
exit 1
