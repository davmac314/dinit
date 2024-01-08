#!/bin/sh

set -eu
cd "$(dirname "$0")"
. ../igr_functions.sh

spawn_dinit boot

# "after"-cycle:
#  ac depends-on ac1, ac2
#  ac1 is "after" ac2
#  ac2 is "after" ac1
STAGE=1
if ! compare_cmd "run_dinitctl start ac" "expected-ac" err; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/actual-ac
    error "'dinitctl start ac' didn't produce expected result." "Check $IGR_OUTPUT/actual-ac."
fi

# before-after conflict
#  ba depends on ba1, ba2
#  ba2 is both before and after ba1
STAGE=2
if ! compare_cmd "run_dinitctl start ba" "expected-ba" err; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/actual-ba
    error "'dinitctl start ba' didn't produce expected result." "Check $IGR_OUTPUT/actual-ba."
fi

# "before_self" is before itself
STAGE=3
if ! compare_cmd "run_dinitctl start before_self" "expected-before_self" err; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/actual-before_self
    error "'dinitctl start before_self' didn't produce expected result." "Check $IGR_OUTPUT/actual-before_self."
fi

# "after_self" is after itself
STAGE=4
if ! compare_cmd "run_dinitctl start after_self" "expected-after_self" err; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/actual-after_self
    error "'dinitctl start after_self' didn't produce expected result." "Check $IGR_OUTPUT/actual-after_self."
fi

stop_dinit
exit 0
