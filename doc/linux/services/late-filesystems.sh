#!/bin/sh

if [ "$1" = start ]; then

    PATH=/usr/bin:/usr/sbin:/bin:/sbin

    fsck -a /dev/sdb2
    fsckresult=$?
    if [ $fsckresult -eq 0 ]; then
    	mount /dev/sdb2 /mnt/sdb2
    	exit $?
    else
        exit $fsckresult
    fi

fi
