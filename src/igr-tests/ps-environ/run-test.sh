#!/bin/sh

rm -f ./env-record

../../dinit -d sd -u -p socket -q \
	checkenv1

../../dinit -d sd -u -p socket -q \
	checkenv2

STATUS=FAIL
if [ -e env-record ]; then
   if [ "$(cat env-record)" = "$(echo hello; echo goodbye)" ]; then
       STATUS=PASS
   fi
fi

if [ $STATUS = PASS ]; then exit 0; fi
exit 1
