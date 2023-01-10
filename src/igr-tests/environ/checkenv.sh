#!/bin/sh

if [ "$TEST_VAR_ONE" = "hello" ]; then
    echo "$DINIT_SOCKET_PATH" >> ./env-record
fi

if [ "$1" = "hello" ]; then
    echo gotenv1 >> ./env-record
elif [ "$1" = "goodbye" ]; then
    echo gotenv2 >> ./env-record
fi

echo "$TEST_VAR_ONE" >> ./env-record
