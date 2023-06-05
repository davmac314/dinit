#!/bin/sh
# append parameter to file

echo "$1" >> "$TEMP"/output/recorded-output

if [ "$1" = "part3" ] || [ "$1" = "part4" ]
then
    false
fi
