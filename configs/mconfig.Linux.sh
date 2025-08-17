#!/bin/sh
# Generate build configuration for Linux.

cd "$(dirname "$(realpath "$(command -v "$0")")")"

rm -f ../mconfig
../configure
