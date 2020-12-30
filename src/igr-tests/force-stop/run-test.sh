#!/bin/sh

../../dinit -d sd -u -p socket -q &
DINITPID=$!

# give time for socket to open
while [ ! -e socket ]; do
    sleep 0.1
done

DINITCTLOUT="$(../../dinitctl -p socket list)"
if [ "$DINITCTLOUT" != "$(cat expected-1)" ]; then
    echo "$DINITCTLOUT" > actual-1
    kill $DINITPID; wait $DINITPID
    exit 1
fi

DINITCTLOUT="$(../../dinitctl -p socket stop critical 2>&1)"
if [ "$DINITCTLOUT" != "$(cat expected-2.err)" ]; then
    echo "$DINITCTLOUT" > actual-2.err
    kill $DINITPID; wait $DINITPID
    exit 1
fi

DINITCTLOUT="$(../../dinitctl -p socket stop --force critical 2>&1)"
if [ "$DINITCTLOUT" != "$(cat expected-3)" ]; then
    echo "$DINITCTLOUT" > actual-3
    kill $DINITPID; wait $DINITPID
    exit 1;
fi

# Note dinit should shutdown since all services stopped.
wait $DINITPID

# Passed:
exit 0
