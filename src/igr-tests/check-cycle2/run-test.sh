#!/bin/sh

set -eu
. "$IGR_FUNCTIONS"

run_dinitcheck > "$IGR_OUTPUT"/output/output.txt 2>&1 && exit 1
if [ $? != 1 ]; then exit 1; fi # Note dinitcheck must return 1 exit code.

if ! compare_file expected.txt "$IGR_OUTPUT"/output/output.txt; then
   error "$IGR_OUTPUT/output/output.txt didn't contain expected result!"
fi

exit 0
