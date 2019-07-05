#!/bin/sh

rm -f ./env-record

../../dinit -d sd -u -p socket -q \
        -e environment1 \
	checkenv

../../dinit -d sd -u -p socket -q \
        -e environment2 \
	checkenv

STATUS=FAIL
if [ -e env-record ]; then
   if [ "$(cat env-record)" = "$(echo hello; echo goodbye)" ]; then
       STATUS=PASS
   fi
fi

if [ $STATUS = PASS ]; then exit 0; fi
exit 1
