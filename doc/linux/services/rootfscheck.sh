#!/bin/sh
export PATH=/usr/bin:/usr/sbin:/bin:/sbin

if [ "$1" != "stop" ]; then
  
  ROOTDEV=`findmnt -o SOURCE -n -M /`
  
  echo "Checking root file system..."
  if [ -x /sbin/fsck ]; then
    /sbin/fsck -C -a "$ROOTDEV"
    fsckresult=$?
    if [ $((fsckresult & 4)) -eq 4 ]; then
      echo "***********************"
      echo "WARNING WARNING WARNING"
      echo "***********************"
      echo "The root file system has problems which require user attention."
      echo "A maintennance shell will now be started; system will then be rebooted."
      /sbin/sulogin
      /sbin/reboot --system -r      
    elif [ $(($fsckresult & 2)) -eq 2 ]; then
      echo "***********************"
      echo "WARNING WARNING WARNING"
      echo "***********************"
      echo "The root file system had problems (now repaired): rebooting..."
      sleep 10
      /sbin/reboot --system -r
    fi
  else
    echo "WARNING - Could not find /sbin/fsck"
  fi
  
fi;
