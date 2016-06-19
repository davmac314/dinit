#!/bin/sh
export PATH=/usr/bin:/usr/sbin:/bin:/sbin

if [ "$1" != "stop" ]; then

  echo "Checking root file system..."
  if [ -x /sbin/fsck ]; then
    /sbin/fsck -C -a /
    fsckresult=$?
    if [ $(($fsckresult & 2)) -eq 2 ]; then
      echo "***********************"
      echo "WARNING WARNING WARNING"
      echo "***********************"
      echo "root file system has problems: rebooting..."
      sleep 10
      /sbin/reboot --system -r
    fi
  else
    echo "WARNING - Could not find /sbin/fsck"
  fi

fi;
