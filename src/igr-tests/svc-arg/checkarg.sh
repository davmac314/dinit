#!/bin/sh

echo "$1" >> "$OUTPUT"

if [ -n "$FOOVAR" ]; then
    echo "$FOOVAR" >> "$OUTPUT"
fi
