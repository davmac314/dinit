#!/bin/sh

# Similar to reload1 test, but with boot service stopped while we reload.

# Start with boot depending on a,b
rm -rf sd
cp -R sd1 sd

../../dinit -d sd -u -p socket -q &
DINITPID=$!

# Give some time for startup
sleep 0.2

../../dinitctl --quiet -p socket start hold

STATUS=PASS

if [ "$(../../dinitctl -p socket list)" != "$(cat initial.expected)" ]; then
   STATUS=FAIL
fi

../../dinitctl --quiet -p socket stop boot

# Put alternate descriptions in place: boot depends on b, c
if [ "$STATUS" = PASS ]; then
    rm -rf sd
    cp -R sd2 sd

    # This should succeed since boot is stopped
    if [ "$(../../dinitctl -p socket reload boot 2>&1)" != "$(cat output2.expected)" ]; then
        STATUS=FAIL
    fi
    
fi

if [ "$STATUS" = PASS ]; then
    ../../dinitctl --quiet -p socket start boot
    if [ "$(../../dinitctl -p socket list)" != "$(cat output3.expected)" ]; then
        STATUS=FAIL
    fi
fi

../../dinitctl --quiet -p socket shutdown
wait $DINITPID

if [ $STATUS = PASS ]; then exit 0; fi
exit 1
