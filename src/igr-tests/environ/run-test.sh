#!/bin/sh

cd "$(dirname "$0")"

export DINIT_SOCKET_PATH="$(pwd)/socket"

rm -f ./env-record

"$DINIT_EXEC" -d sd -u -p socket -q \
        -e environment1 \
	checkenv

"$DINIT_EXEC" -d sd -u -p socket -q \
        -e environment2 \
	checkenv

"$DINIT_EXEC" -d sd -u -p socket -q \
	setenv1

STATUS=FAIL
if [ -e env-record ]; then
   if [ "$(cat env-record)" = "$(echo $DINIT_SOCKET_PATH; echo hello; echo goodbye; echo 3; echo 2; echo 1)" ]; then
       STATUS=PASS
   fi
fi

if [ $STATUS = PASS ]; then exit 0; fi
exit 1
