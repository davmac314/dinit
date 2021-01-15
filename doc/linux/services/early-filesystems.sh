#!/bin/sh

set -e

if [ "$1" = start ]; then

    PATH=/usr/bin:/usr/sbin:/bin:/sbin

    # Must have sysfs mounted for udevtrigger to function.
    mount -n -t sysfs sysfs /sys
    
    # Ideally devtmpfs will be mounted by kernel, we can mount here anyway:
    mount -n -t devtmpfs tmpfs /dev
    mkdir -p /dev/pts /dev/shm
    mount -n -t tmpfs -o nodev,nosuid tmpfs /dev/shm
    mount -n -t devpts -o gid=5 devpts /dev/pts

    # /run, and various directories within it
    mount -n -t tmpfs -o mode=775 tmpfs /run
    mkdir /run/lock /run/udev
    
    # "hidepid=1" doesn't appear to take effect on first mount of /proc,
    # so we mount it and then remount:
    mount -n -t proc -o hidepid=1 proc /proc
    mount -n -t proc -o remount,hidepid=1 proc /proc

fi
