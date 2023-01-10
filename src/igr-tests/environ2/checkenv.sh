#!/bin/sh

echo "$TEST_VAR" >> ./env-record
echo "$TEST_VAR_BASE" >> ./env-record
echo "$TEST_VAR_ONE" >> ./env-record
echo "$USER" >> ./env-record
echo "$LOGNAME" >> ./env-record
echo "$SHELL" >> ./env-record
echo "$UID" >> ./env-record
echo "$GID" >> ./env-record
