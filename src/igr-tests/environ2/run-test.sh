#!/bin/sh

# FIXME: UID (and possibly GID) cannot unsent on MacOS! Skipping...
if [ "$(uname)" = "Darwin" ]; then
    exit 2
fi

set -e
. "$IGR_FUNCTIONS"

rm -f "$TEMP"/output/env-record

RUSER=$(id -nu)
RUID=$(id -u)
RGID=$(id -g)

# unset to make sure dinit can initialize this itself
for var in USER LOGNAME SHELL UID GID; do
    unset $var
done

# test whether vars from global environment propagate
export TEST_VAR="helloworld"

spawn_dinit_oneshot -e env-dinit

USER="$RUSER"
# we try to override this one in env-dinit, but it should be set per-service
LOGNAME="$USER"
# these are overriden in env files
SHELL="/bogus/value"

if ! compare_text "$TEMP"/output/env-record "$(echo helloworld;\
                                             echo hello;\
                                             echo override;\
                                             echo "$USER";\
                                             echo "$LOGNAME";\
                                             echo "$SHELL";\
                                             echo "$RUID";\
                                             echo "$RGID")"
then
    error "$TEMP/output/env-record didn't contain expected result!"
fi

exit 0
