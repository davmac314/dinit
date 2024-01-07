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

STAGE=2
if ! compare_cmd "run_dinitctl start ba" "expected-ba" err; then
    echo "$CMD_OUT" > "$IGR_OUTPUT"/actual-ba
    error "'dinitctl start ba' didn't produce expected result." "Check $IGR_OUTPUT/actual-ba."
fi

stop_dinit
exit 0
