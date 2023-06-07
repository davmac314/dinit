#!/bin/sh

set -eu

sleep "$2"
echo "$1" >> "$TEMP"/output/script-output
