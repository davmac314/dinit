#!/bin/sh
export PATH=/usr/bin:/usr/sbin:/bin:/sbin

set -e

if [ "$1" != "stop" ]; then

  echo "Mounting auxillary filesystems...."
  swapon /swapfile
  mount -avt noproc,nonfs

fi;
