#!/bin/sh

cd $(dirname $0)

case "$1" in
    setenv1)
        if [ "$FOO" = "foo" -a "$BAR" = "bar" -a "$BAZ" = "baz" ]; then
            echo 1 >> ./env-record
        fi
        ;;
    setenv2)
        if [ "$FOO" = "foo" ]; then
            echo 2 >> ./env-record
            export BAR=bar
            "$DINITCTL_EXEC" setenv BAR BAZ=baz
        fi
        ;;
    setenv3)
        "$DINITCTL_EXEC" setenv FOO=foo
        echo 3 >> ./env-record
        ;;
    *) ;;
esac
