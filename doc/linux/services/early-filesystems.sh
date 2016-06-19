#!/bin/sh

if [ "$1" = start ]; then

    PATH=/usr/bin:/usr/sbin:/bin:/sbin

    # Must have sysfs mounted for udevtrigger to function.
    mount -n -t sysfs sysfs /sys
    # Ideally devtmpfs will be mounted by kernel:
    mount -n -t devtmpfs tmpfs /dev
    mount -n -t tmpfs -o mode=775 tmpfs /run
    mount -n -t proc -o hidepid=1 proc /proc
    mkdir /run/udev
    mkdir /dev/pts
    mkdir /dev/shm

fi
