#!/bin/sh
#
# Check that a service with a missing dependency causes the appropriate error.
#

set -eu
cd "$(dirname "$0")"
. ../igr_functions.sh

spawn_dinit

if ! compare_cmd "run_dinitctl start missing-dep-svc" "output.expected" err; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/output.actual
    error "'dinitctl start missing-dep-svc 2>&1' didn't contain expected result!" "Check $IGR_OUTPUT/output.actual"
fi

stop_dinit

exit 0
