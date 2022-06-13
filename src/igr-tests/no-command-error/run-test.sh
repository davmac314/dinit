#!/bin/sh
#
# Check that a service without a command configured causes the appropriate error.
#

rm -f dinit-run.log

# If cgroups support, supply cgroup base path to avoid potential "unable to determine
# cgroup" message
CGROUPS_BASE=""
if ../../dinit --version | grep -q " cgroups"; then
    CGROUPS_BASE="-b \"\""
fi

../../dinit -d sd -u -p socket -q $CGROUPS_BASE \
	no-command -l dinit-run.log

STATUS=FAIL
if [ -e dinit-run.log ]; then
   if [ "$(cat dinit-run.log)" = "$(cat dinit-run.expected)" ]; then
       STATUS=PASS
   fi
fi

if [ $STATUS = PASS ]; then exit 0; fi
exit 1
