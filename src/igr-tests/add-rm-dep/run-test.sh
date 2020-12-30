#!/bin/sh

../../dinit -d sd -u -p socket -q main &
DINITPID=$!

# give time for socket to open
while [ ! -e socket ]; do
    sleep 0.1
done

STATUS=FAIL

DINITCTL="../../dinitctl -p socket"

while

    stage=1
    out=$($DINITCTL list)
    if [ $? != 0 ]; then break; fi
    if [ "$out" != "$(cat expected1)" ]; then break; fi
    # both "main" and "secondary" should be running

    stage=2
    out=$($DINITCTL rm-dep waits-for main secondary)
    if [ $? != 0 ]; then break; fi
    if [ "$out" != "$(cat expected2)" ]; then break; fi
    # "secondary" should stop

    stage=3
    out=$($DINITCTL list)
    if [ $? != 0 ]; then break; fi
    if [ "$out" != "$(cat expected3)" ]; then break; fi

    stage=4
    out=$($DINITCTL add-dep waits-for main secondary)
    if [ $? != 0 ]; then break; fi
    if [ "$out" != "$(cat expected4)" ]; then break; fi
    # "secondary" will not automatically start, this is a waits-for dep

    stage=5
    out=$($DINITCTL list)
    if [ $? != 0 ]; then break; fi
    if [ "$out" != "$(cat expected3)" ]; then break; fi

    stage=6
    out=$($DINITCTL wake secondary)
    if [ $? != 0 ]; then break; fi
    if [ "$out" != "$(cat expected5)" ]; then break; fi
    # if we wake "secondary" it should start and remain started

    stage=7
    out=$($DINITCTL list)
    if [ $? != 0 ]; then break; fi
    if [ "$out" != "$(cat expected1)" ]; then break; fi

    STATUS=PASS
    false
    
do :; done

kill $DINITPID
wait $DINITPID

if [ $STATUS = PASS ]; then exit 0; fi
echo "failed at stage $stage, with output:" > actual
echo "$out" >> actual
exit 1
