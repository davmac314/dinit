#!/bin/sh

cd $(dirname $0)

rm -f ./env-record

export TEST_VAR_TWO=set-via-script

$DINIT_EXEC -d sd -u -p socket -q \
	checkenv1

$DINIT_EXEC -d sd -u -p socket -q \
	checkenv2

$DINIT_EXEC -d sd -u -p socket -q \
	checkenv3

$DINIT_EXEC -d sd -e dinit-environment -u -p socket -q \
	checkenv4

$DINIT_EXEC -d sd -u -p socket -q \
	checkenv4

STATUS=FAIL
if [ -e env-record ]; then
   if [ "$(cat env-record)" = "$(cat env-expected)" ]; then
       STATUS=PASS
   fi
fi

if [ $STATUS = PASS ]; then exit 0; fi
exit 1
