#!/bin/sh

set -eu
. "$IGR_FUNCTIONS"

# Tests around before/after link functionality.

rm -rf "$TEMP"/output/*

spawn_dinit -d sd1

# Custom error.
__error() {
    stop_dinit
    exit 1
}

# start parent; should start service2 and then service1 (due to before= in service2).
dinitctl "$QUIET" start parent || __error

if ! compare_text "$TEMP"/output/script-output "$(printf "two\none\n")"; then
    stop_dinit
    error "$TEMP/output/script-output didn't contain expected result!"
fi

rm "$TEMP"/output/script-output

# unloading and reloading service1 should not lose the before= relationship.
dinitctl "$QUIET" stop parent || __error
dinitctl "$QUIET" unload parent || __error
dinitctl "$QUIET" unload service1 || __error

dinitctl "$QUIET" reload service1 || __error
dinitctl "$QUIET" start parent || __error

if ! compare_text "$TEMP"/output/script-output "$(printf "two\none\n")"; then
    stop_dinit
    error "$TEMP/output/script-output didn't contain expected result!"
fi

rm "$TEMP"/output/script-output

stop_dinit
exit 0
