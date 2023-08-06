#!/bin/sh

set -eu
cd "$(dirname "$0")"
. ../igr_functions.sh

# Tests around before/after link functionality.

rm -rf "$IGR_OUTPUT"/*

spawn_dinit

# service2 depends on service1, and service1 is "before" service2

run_dinitctl $QUIET reload service2

# however, we'll remove the depends-on dependency before starting both
run_dinitctl $QUIET rm-dep regular service2 service1

run_dinitctl $QUIET start --no-wait service1
run_dinitctl $QUIET start service2


# Note service1 takes longer to start, but has a "before" service2 so should still start first.
if ! compare_text "$IGR_OUTPUT"/script-output "$(printf "one\ntwo\n")"; then
    error "$IGR_OUTPUT/script-output didn't contain expected result!"
fi

rm "$IGR_OUTPUT"/script-output

stop_dinit

exit 0
