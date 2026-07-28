#!/bin/sh
export PATH=/usr/bin:/usr/sbin:/bin:/sbin
umask 0077

set -e

if [ "$1" != "stop" ]; then
  
  # cleanup
  # (delete /tmp etc)
  rm -rf /tmp/* /tmp/.[!.]* /tmp/..?*
  
  # empty utmp, create needed directories
  : > /var/run/utmp
  mkdir -m og-w /var/run/dbus

  # Seed random number generators / entropy pool
  seedrng
  
  # Configure network
  /sbin/ifconfig lo 127.0.0.1

  # You can put other static configuration here:
  #/sbin/ifconfig eth0 192.168.1.38 netmask 255.255.255.0 broadcast 192.168.1.255

  echo "myhost" > /proc/sys/kernel/hostname

  # /usr/sbin/alsactl restore

else

  # The system is being shut down
  
  # Save entropy for next boot
  seedrng

fi;
