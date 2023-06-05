#!/bin/sh
#
# Check that a service without a command configured causes the appropriate error.
#

set -e
. "$IGR_FUNCTIONS"

rm -f "$TEMP"/output/dinit-run.log

# If cgroups support, supply cgroup base path to avoid potential "unable to determine
# cgroup" message
CGROUPS_BASE=""
if __find_dinit && "$DINIT" --version | grep -q " cgroups"; then
    CGROUPS_BASE="-b \"\""
fi

spawn_dinit_oneshot $CGROUPS_BASE no-command -l "$TEMP"/output/dinit-run.log

if ! compare_file "$TEMP"/output/dinit-run.log "dinit-run.expected"; then
    error "$TEMP/output/dinit-run.log didn't contain expected result!"
fi

exit 0
