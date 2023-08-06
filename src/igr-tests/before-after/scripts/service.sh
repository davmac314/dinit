#!/bin/sh

set -eu

sleep "$2"
echo "$1" >> "$IGR_OUTPUT"/script-output
