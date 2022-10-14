#!/bin/sh
export PATH=/usr/bin:/usr/sbin:/bin:/sbin

if [ "$1" != "stop" ]; then

  ROOTDEV=`findmnt -v -o SOURCE -n -M /`

  echo "Checking root file system (^C to skip)..."
  if [ -x /sbin/fsck ]; then
    /sbin/fsck -C -a "$ROOTDEV"
    fsckresult=$?
    if [ $((fsckresult & 4)) -eq 4 ]; then
      echo "***********************"
      echo "WARNING WARNING WARNING"
      echo "***********************"
      echo "The root file system has problems which require user attention."
      echo "A maintenance shell will now be started; system will then be rebooted."
      /sbin/sulogin
      /sbin/reboot --use-passed-cfd -r
    elif [ $(($fsckresult & 2)) -eq 2 ]; then
      echo "***********************"
      echo "WARNING WARNING WARNING"
      echo "***********************"
      echo "The root file system had problems (now repaired): rebooting..."
      sleep 5
      /sbin/reboot --use-passed-cfd -r
    fi
  else
    echo "WARNING - Could not find /sbin/fsck"
  fi

fi;
