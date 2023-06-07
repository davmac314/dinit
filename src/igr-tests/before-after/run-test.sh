#!/bin/sh

set -eu

DINIT_EXEC=${DINIT_EXEC:-../../dinit}
DINITCTL_EXEC=${DINITCTL_EXE:-../../dinitctl}

cd "$(dirname "$0")"

# Tests around before/after link functionality.

mkdir -p output
rm -rf output/*

"$DINIT_EXEC" -q -d sd1 -u -p socket  &

# Give some time for startup
sleep 0.2

# start parent; should start service2 and then service1 (due to before= in service2).
"$DINITCTL_EXEC" --quiet -p socket start parent

if [ "$(cat output/script-output)" != "$(printf "two\none\n")" ]; then
    "$DINITCTL_EXEC" --quiet -p socket shutdown
    return 1
fi

rm output/script-output

# unloading and reloading service1 should not lose the before= relationship
"$DINITCTL_EXEC" --quiet -p socket stop parent
"$DINITCTL_EXEC" --quiet -p socket unload parent
"$DINITCTL_EXEC" --quiet -p socket unload service1

"$DINITCTL_EXEC" --quiet -p socket reload service1
"$DINITCTL_EXEC" --quiet -p socket start parent

if [ "$(cat output/script-output)" != "$(printf "two\none\n")" ]; then
    "$DINITCTL_EXEC" --quiet -p socket shutdown
    return 1
fi

rm output/script-output

"$DINITCTL_EXEC" --quiet -p socket shutdown
# Success.
