#!/bin/sh

cd "$(dirname "$0")"

export DINIT_SOCKET_PATH="$(pwd)/socket"

rm -f ./env-record

RUSER=$(id -nu)
RUID=$(id -u)
RGID=$(id -g)

# unset to make sure dinit can initialize this itself
unset USER
unset LOGNAME
unset SHELL
unset UID
unset GID

# test whether vars from global environment propagate
export TEST_VAR="helloworld"

"$DINIT_EXEC" -d sd -u -p socket -q \
        -e env-dinit \
	checkenv

USER="$RUSER"
# we try to override this one in env-dinit, but it should be set per-service
LOGNAME="$USER"
# these are overriden in env files
SHELL="/bogus/value"

STATUS=FAIL
if [ -e env-record ]; then
   if [ "$(cat env-record)" = "$(echo helloworld; echo hello; echo override; echo $USER; echo $LOGNAME; echo $SHELL; echo $RUID; echo $RGID)" ]; then
       STATUS=PASS
   fi
fi

if [ $STATUS = PASS ]; then exit 0; fi
exit 1
