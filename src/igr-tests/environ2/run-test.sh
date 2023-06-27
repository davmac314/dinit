#!/bin/sh

set -eu
cd "$(dirname "$0")"
. ../igr_functions.sh

rm -f "$IGR_OUTPUT"/output/env-record

RUSER=$(id -nu)
RUID=$(id -u)
RGID=$(id -g)

# unset to make sure dinit can initialize this itself
for var in USER LOGNAME SHELL; do
    unset $var
done
# $UID & $GID are readonly on MacOS
if [ "$(uname)" != "Darwin" ]; then
    unset UID
    unset GID
fi

# test whether vars from global environment propagate
export TEST_VAR="helloworld"

spawn_dinit_oneshot -e env-dinit checkenv

USER="$RUSER"
# we try to override this one in env-dinit, but it should be set per-service
LOGNAME="$USER"
# these are overriden in env files
SHELL="/bogus/value"

if ! compare_text "$IGR_OUTPUT"/output/env-record "$(echo helloworld;\
                                             echo hello;\
                                             echo override;\
                                             echo "$USER";\
                                             echo "$LOGNAME";\
                                             echo "$SHELL";\
                                             echo "$RUID";\
                                             echo "$RGID")"
then
    error "$IGR_OUTPUT/output/env-record didn't contain expected result!"
fi

exit 0
