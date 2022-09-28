#!/bin/sh

if [ $IS_MESON ]; then
   cd $(dirname $0)
   DINIT_EXEC=$APPS_PATH/dinit
   DINITCTL_EXEC=$APPS_PATH/dinitctl
else
   DINIT_EXEC=../../dinit
   DINITCTL_EXEC=../../dinitctl
fi

rm -f ./basic-ran

$DINIT_EXEC -d sd -u -p socket -q \
	boot &
DINITPID=$!

# give time for socket to open
while [ ! -e socket ]; do
    sleep 0.1
done

# try to start "bad-command" which will fail
DINITCTLOUT="$($DINITCTL_EXEC -p socket start bad-command 2>&1)"
if [ "$DINITCTLOUT" != "$(cat expected-1)" ]; then
    echo "$DINITCTLOUT" > actual-1
    kill $DINITPID; wait $DINITPID
    exit 1
fi

# try to start command which will timeout
DINITCTLOUT="$($DINITCTL_EXEC -p socket start timeout-command 2>&1)"
if [ "$DINITCTLOUT" != "$(cat expected-2)" ]; then
    echo "$DINITCTLOUT" > actual-2
    kill $DINITPID; wait $DINITPID
    exit 1
fi

$DINITCTL_EXEC --quiet -p socket shutdown
wait $DINITPID

exit 0
