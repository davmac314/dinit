#!/bin/sh

rm -f ./basic-ran

if [ $IS_MESON ]; then
   cd $(dirname $0)
   $APPS_PATH/dinit -d sd -u -p socket -q \
           basic
else
   ../../dinit -d sd -u -p socket -q \
	   basic
fi

STATUS=FAIL
if [ -e basic-ran ]; then
   if [ "$(cat basic-ran)" = "ran" ]; then
       STATUS=PASS
   fi
fi

if [ $STATUS = PASS ]; then exit 0; fi
exit 1
