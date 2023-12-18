#!/bin/sh

set -eu
cd "$(dirname "$0")"
. ../igr_functions.sh

rm -f "$IGR_OUTPUT"/basic-ran

export XDG_CONFIG_HOME="$PWD/config/"

find_dinit
"$DINIT" $QUIET -u -p "$SOCKET" -l /dev/null basic

if ! compare_text "$IGR_OUTPUT"/basic-ran "ran"; then
    error "$IGR_OUTPUT/basic-ran didn't contain expected result!"
fi

exit 0
