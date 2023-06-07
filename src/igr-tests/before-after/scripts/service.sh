#!/bin/sh

set -eu

sleep "$2"
echo "$1" >> output/script-output
