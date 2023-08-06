#!/bin/sh
#
# Check that a service without a command configured causes the appropriate error.
#

set -eu
cd "$(dirname "$0")"
. ../igr_functions.sh

rm -f "$IGR_OUTPUT"/dinit-run.log

# If cgroups support, supply cgroup base path to avoid potential "unable to determine
# cgroup" message
CGROUPS_BASE=""
if find_dinit && "$DINIT" --version | grep -q " cgroups"; then
    CGROUPS_BASE="-b \"\""
fi

spawn_dinit_oneshot $CGROUPS_BASE no-command -l "$IGR_OUTPUT"/dinit-run.log

if ! compare_file "$IGR_OUTPUT"/dinit-run.log "dinit-run.expected"; then
    error "$IGR_OUTPUT/dinit-run.log didn't contain expected result!"
fi

exit 0
