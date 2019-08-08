#!/bin/sh

rm -f ./basic-ran

../../dinit -d sd -u -p socket -q \
	parent &
DINITPID=$!

# give time for socket to open
while [ ! -e socket ]; do
    sleep 0.1
done

# wait until parent (and therefore 'basic') has fully started
../../dinitctl --quiet -p socket start parent

sleep 0.1 # time for file to be written

STATUS=FAIL
if [ -e basic-ran ]; then
   if [ "$(cat basic-ran)" = "ran" ]; then
       STATUS=PASS
   fi
fi

if [ $STATUS != PASS ]; then
    ../../dinitctl --quiet -p socket shutdown
    exit 1;
fi

rm basic-ran 

STATUS=FAIL
../../dinitctl --quiet -p socket restart basic
sleep .1 # time for file write
if [ -e basic-ran ]; then
   if [ "$(cat basic-ran)" = "ran" ]; then
       STATUS=PASS
   fi
fi

../../dinitctl --quiet -p socket shutdown
wait $DINITPID

if [ $STATUS = PASS ]; then exit 0; fi
exit 1
