#!/bin/sh

set -e
. "$IGR_FUNCTIONS"

# Similar to reload1 test, but with boot service stopped while we reload.

# Start with boot depending on a,b
rm -rf sd
cp -R sd1 sd

spawn_dinit

dinitctl "$QUIET" -p socket start hold

if ! compare_cmd "dinitctl list" "initial.expected"; then
    echo "$CMD_OUT" > "$TEMP"/output/initial.actual
    error "'dinitctl list' didn't contain expected result!"
fi

dinitctl "$QUIET" stop boot

# Put alternate descriptions in place: boot depends on b, c
rm -rf sd
cp -R sd2 sd

# This should succeed since boot is stopped
if ! compare_cmd "dinitctl reload boot" "output2.expected" err; then
    echo "$CMD_OUT" > "$TEMP"/output/output2.actual
    error "'dinitctl reload boot 2>&1' didn't contain expected result"
fi

dinitctl "$QUIET" start boot
if ! compare_cmd "dinitctl list" "output3.expected"; then
    echo "$CMD_OUT" > "$TEMP"/output/output3.actual
    error "'dinitctl list' didn't contain expected result"
fi

stop_dinit

exit 0
