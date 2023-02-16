#!/bin/sh

cd "$(dirname "$0")"

rm -f ./basic-ran

"$DINIT_EXEC" -d sd -u -p socket -q \
	basic

STATUS=FAIL
if [ -e basic-ran ]; then
   if [ "$(cat basic-ran)" = "ran" ]; then
       STATUS=PASS
   fi
fi

if [ $STATUS = PASS ]; then exit 0; fi
exit 1
