#!/bin/sh

if [ "$TEST_VAR_ONE" = "hello" ]; then
    echo "$DINIT_SOCKET_PATH" >> ./env-record
    if [ "$2" = "$DINIT_SERVICE" ]; then
        echo "$DINIT_SERVICE" >> ./env-record
    fi
fi

if [ "$1" = "hello" ]; then
    echo gotenv1 >> ./env-record
elif [ "$1" = "goodbye" ]; then
    echo gotenv2 >> ./env-record
fi

echo "$TEST_VAR_ONE" >> ./env-record
