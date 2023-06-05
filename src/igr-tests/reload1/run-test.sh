#!/bin/sh

set -e
. "$IGR_FUNCTIONS"

# Start with boot depending on a,b
rm -rf sd
cp -R sd1 sd

spawn_dinit

if ! compare_cmd "dinitctl list" "initial.expected"; then
    echo "$CMD_OUT" > "$TEMP"/output/initial.actual
    error "'dinitctl list' didn't contain expected result!"
fi

# Put alternate descriptions in place: boot depends on b, c
rm -rf sd
cp -R sd2 sd
# First attempt should fail, c not started
if ! compare_cmd "dinitctl --quiet reload boot" "output2.expected" err; then
    echo "$CMD_OUT" > "$TEMP"/output/output2.actual
    error "'dinitctl --quiet reload boot 2>&1' didn't contain expected result"
fi
dinitctl "$QUIET" start c
dinitctl "$QUIET" reload boot
if ! compare_cmd "dinitctl --quiet list" "output3.expected"; then
    echo "$CMD_OUT" > "$TEMP"/output/output3.actual
    error "'dinitctl --quiet list' didn't contain expected result!"
fi

stop_dinit

exit 0
