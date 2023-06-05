#!/bin/sh

if [ "$TEST_VAR_ONE" = "hello" ]; then
    echo "$SOCKET" >> "$OUTPUT"
    if [ "$2" = "$DINIT_SERVICE" ]; then
        echo "$DINIT_SERVICE" >> "$OUTPUT"
    fi
fi

if [ "$1" = "hello" ]; then
    echo gotenv1 >> "$OUTPUT"
elif [ "$1" = "goodbye" ]; then
    echo gotenv2 >> "$OUTPUT"
fi

echo "$TEST_VAR_ONE" >> "$OUTPUT"
