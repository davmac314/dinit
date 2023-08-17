#!/bin/sh

set -eu
cd "$(dirname "$0")"
. ../igr_functions.sh

rm -f "$IGR_OUTPUT"/env-record

RUSER=$(id -nu)
RUID=$(id -u)
RGID=$(id -g)

# unset to make sure dinit can initialize this itself
for var in USER LOGNAME SHELL; do
    unset $var
done
# ignore $UID & $GID unsetting errors
# Some shells make them readonly
for var in UID GID; do
    unset $var > /dev/null 2>&1 || :
done

# test whether vars from global environment propagate
export TEST_VAR="helloworld"

spawn_dinit_oneshot -e env-dinit checkenv

USER="$RUSER"
# we try to override this one in env-dinit, but it should be set per-service
LOGNAME="$USER"
# these are overriden in env files
SHELL="/bogus/value"

if ! compare_text "$IGR_OUTPUT"/env-record "$(echo helloworld;\
                                             echo hello;\
                                             echo override;\
                                             echo "$USER";\
                                             echo "$LOGNAME";\
                                             echo "$SHELL";\
                                             echo "$RUID";\
                                             echo "$RGID")"
then
    error "$IGR_OUTPUT/env-record didn't contain expected result!"
fi

exit 0
