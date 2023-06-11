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

# Note service1 takes longer to start, but has a "before" service2 so should still start first.
# service3 is similarly "after" service2.
if [ "$(cat output/script-output)" != "$(printf "one\ntwo\nthree\n")" ]; then
    "$DINITCTL_EXEC" --quiet -p socket shutdown
    return 1
fi

rm output/script-output

# unloading and reloading service2 should not lose the before= or after= relationship
"$DINITCTL_EXEC" --quiet -p socket stop parent
"$DINITCTL_EXEC" --quiet -p socket unload parent
"$DINITCTL_EXEC" --quiet -p socket unload service2

"$DINITCTL_EXEC" --quiet -p socket reload service2
"$DINITCTL_EXEC" --quiet -p socket start parent

if [ "$(cat output/script-output)" != "$(printf "one\ntwo\nthree\n")" ]; then
    "$DINITCTL_EXEC" --quiet -p socket shutdown
    return 1
fi

rm output/script-output

"$DINITCTL_EXEC" --quiet -p socket shutdown
# Success.
