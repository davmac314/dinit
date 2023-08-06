#!/bin/sh

{
    echo "$TEST_VAR"
    echo "$TEST_VAR_BASE"
    echo "$TEST_VAR_ONE"
    echo "$USER"
    echo "$LOGNAME"
    echo "$SHELL"
    echo "$UID"
    echo "$GID"
} >> "$IGR_OUTPUT"/env-record
