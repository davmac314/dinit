#!/bin/sh

set -e
. "$IGR_FUNCTIONS"

dinitcheck > "$TEMP"/output/output.txt 2>&1 && exit 1
if [ $? != 1 ]; then exit 1; fi # Note dinitcheck must return 1 exit code.

if ! compare_file expected.txt "$TEMP"/output/output.txt; then
   error "$TEMP/output/output.txt didn't contain expected result!"
fi

exit 0
