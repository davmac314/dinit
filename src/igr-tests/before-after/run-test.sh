#!/bin/sh

set -eu
. "$IGR_FUNCTIONS"

# Tests around before/after link functionality.

rm -rf "$IGR_OUTPUT"/output/*

spawn_dinit

# start parent; should start service2 and then service1 (due to before= in service2).
run_dinitctl $QUIET start parent || error "run_dinitctl returned $?!"

# Note service1 takes longer to start, but has a "before" service2 so should still start first.
# service3 is similarly "after" service2.
if ! compare_text "$IGR_OUTPUT"/output/script-output "$(printf "one\ntwo\nthree\n")"; then
    error "$IGR_OUTPUT/output/script-output didn't contain expected result!"
fi

rm "$IGR_OUTPUT"/output/script-output

# unloading and reloading service2 should not lose the before= or after= relationship
run_dinitctl $QUIET stop parent || error "run_dinitctl returned $?!"
run_dinitctl $QUIET unload parent || error "run_dinitctl returned $?!"
run_dinitctl $QUIET unload service2 || error "run_dinitctl returned $?!"

run_dinitctl $QUIET reload service2 || error "run_dinitctl returned $?!"
run_dinitctl $QUIET start parent || error "run_dinitctl returned $?!"

if ! compare_text "$IGR_OUTPUT"/output/script-output "$(printf "one\ntwo\nthree\n")"; then
    error "$IGR_OUTPUT/output/script-output didn't contain expected result!"
fi

rm "$IGR_OUTPUT"/output/script-output

stop_dinit
exit 0
