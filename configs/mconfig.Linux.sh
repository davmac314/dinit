#!/bin/sh
# Generate build configuration for Linux.

cd "$(dirname "$(realpath "$0")")"

rm -f ../mconfig
../configure
