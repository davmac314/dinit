#!/bin/sh

if [ $IS_MESON ]; then
   DINITCHECK_EXEC=$APPS_PATH/dinitcheck
else
   DINITCHECK_EXEC=../../dinitcheck
fi

$DINITCHECK_EXEC -d sd > output.txt 2>&1
if [ $? != 1 ]; then exit 1; fi

STATUS=FAIL
if cmp -s expected.txt output.txt; then
   STATUS=PASS
fi

if [ $STATUS = PASS ]; then exit 0; fi
exit 1
