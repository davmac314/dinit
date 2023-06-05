#!/bin/sh
#
# Check that a service with a missing dependency causes the appropriate error.
#

set -eu
. "$IGR_FUNCTIONS"

spawn_dinit

if ! compare_cmd "dinitctl start missing-dep-svc" "output.expected" err; then
    echo "$CMD_OUT" > "$TEMP"/output/output.actual
    stop_dinit
    error "" # FIXME
fi

stop_dinit

exit 0
