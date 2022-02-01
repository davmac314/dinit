#!/bin/sh

rm -f ./args-record

export TEST_VAR_ONE="var one" TEST_VAR_TWO=vartwo TEST_VAR_THREE=varthree
../../dinit -d sd -u -p socket -q \
	checkargs

STATUS=FAIL
if [ -e args-record ]; then
   if [ "$(cat args-record)" = "1:xxxvar one/yyy 2:vartwovarthree 3:" ]; then
       STATUS=PASS
   fi
fi

if [ $STATUS = PASS ]; then exit 0; fi
exit 1
