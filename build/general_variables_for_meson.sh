#!env bash

## For some reasons meson cant read variables from env/files.
## So we need to a wrapper for variables. This script is a wrapper for importing variables to meson.
## Note: We use "printf" instead of "echo" because we dont need "\n" at end of outputs. "printf" by default dont print "\n" at end of output.

set -e # Exit immediately if a command exits with a non-zero status.

cd $(dirname $0)
. ./version.conf

case $1 in
	version)
		printf $VERSION
		;;
	month)
		printf $MONTH
		;;
	year)
		printf $YEAR
		;;
	*)
		echo "Invalid argument. Please set version or month or year as argument"
		exit 1
esac
