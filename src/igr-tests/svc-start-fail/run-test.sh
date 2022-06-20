#!/bin/sh

rm -f ./basic-ran

../../dinit -d sd -u -p socket -q \
	boot &
DINITPID=$!

# give time for socket to open
while [ ! -e socket ]; do
    sleep 0.1
done

# try to start "bad-command" which will fail
DINITCTLOUT="$(../../dinitctl -p socket start bad-command 2>&1)"
if [ "$DINITCTLOUT" != "$(cat expected-1)" ]; then
    echo "$DINITCTLOUT" > actual-1
    kill $DINITPID; wait $DINITPID
    exit 1
fi

# try to start command which will timeout
DINITCTLOUT="$(../../dinitctl -p socket start timeout-command 2>&1)"
if [ "$DINITCTLOUT" != "$(cat expected-2)" ]; then
    echo "$DINITCTLOUT" > actual-2
    kill $DINITPID; wait $DINITPID
    exit 1
fi

../../dinitctl --quiet -p socket shutdown
wait $DINITPID

exit 0
