#!/bin/sh

set -eu
cd "$(dirname "$0")"
. ../igr_functions.sh

rm -rf "$IGR_OUTPUT"/*

cp -pr sd "$IGR_OUTPUT"
mkdir -p "$IGR_OUTPUT/sd/boot.d"

run_dinitctl $QUIET --offline -d "$IGR_OUTPUT/sd" enable A
   
if [ ! -f "$IGR_OUTPUT/sd/boot.d/A" ]; then
    error "Service A not enabled after enable command; $IGR_OUTPUT/sd/boot.d/A does not exist"
fi

run_dinitctl $QUIET --offline -d "$IGR_OUTPUT/sd" disable A

if [ -f "$IGR_OUTPUT/sd/boot.d/A" ]; then
    error "Service A not disabled after disable command; $IGR_OUTPUT/sd/boot.d/A still exists"
fi

exit 0
