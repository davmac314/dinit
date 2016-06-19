#!/bin/sh

if [ "$1" = start ]; then

    PATH=/usr/bin:/usr/sbin:/bin:/sbin

    fsck -a /dev/sdb2
    mount /dev/sdb2 /mnt/sdb2
    mount --bind /mnt/sdb2/src /usr/src
    mount --bind /mnt/sdb2 /mnt/tmp  # hopefully can remove this at some point

fi
