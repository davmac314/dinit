#!/bin/sh

if [ "$TEST_VAR_ONE" = "hello" ]; then
    echo "$DINIT_SOCKET_PATH" >> ./env-record
fi

echo "$TEST_VAR_ONE" >> ./env-record
