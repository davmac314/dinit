#!/bin/sh
export PATH=/usr/bin:/usr/sbin:/bin:/sbin

set -e

if [ "$1" != "stop" ]; then

  echo "Mounting auxillary filesystems...."
  mount -t tmpfs -o nodev,nosuid tmpfs /dev/shm
  mount -t devpts -o gid=tty devpts /dev/pts
  swapon /swapfile
  mount -avt noproc,nonfs

fi;
