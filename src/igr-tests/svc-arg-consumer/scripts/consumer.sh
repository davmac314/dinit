#!/bin/sh
echo "$1" > "$IGR_OUTPUT/logged-output"
cat - >> "$IGR_OUTPUT/logged-output" 2>&1
