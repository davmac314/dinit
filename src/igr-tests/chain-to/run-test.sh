#!/bin/sh

rm -f ./recorded-output

../../dinit -d sd -u -p socket -q \
	part1

STATUS=FAIL
if [ -e recorded-output ]; then
   if [ "$(cat recorded-output)" = "$(echo part1; echo part2; echo part3; echo part4)" ]; then
       STATUS=PASS
   fi
fi

if [ $STATUS = PASS ]; then exit 0; fi
exit 1
