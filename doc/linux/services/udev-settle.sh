#!/bin/sh

if [ "$1" = start ]; then
    udevadm trigger --action=add
    udevadm settle
fi
