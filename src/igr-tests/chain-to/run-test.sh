#!/bin/sh

cd $(dirname $0)

rm -f ./recorded-output

"$DINIT_EXEC" -d sd -u -p socket -q \
	part1

STATUS=FAIL
if [ -e recorded-output ]; then
   if [ "$(cat recorded-output)" = "$(echo part1; echo part2; echo part3; echo part4)" ]; then
       STATUS=PASS
   fi
fi

if [ $STATUS = PASS ]; then exit 0; fi
exit 1
