#!/bin/sh

cd "$(dirname "$0")"

mkdir -p output
rm -rf output/*

"$DINIT_EXEC" -d sd -u -p socket -q \
	boot

STATUS=FAIL
if [ -e output/svc-script ]; then
   if [ "$(cat output/svc-script)" = "ran" ]; then
       STATUS=PASS
   fi
fi

if [ $STATUS = PASS ]; then exit 0; fi
exit 1
