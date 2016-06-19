#!/bin/sh
export PATH=/usr/bin:/usr/sbin:/bin:/sbin
umask 0077

if [ "$1" != "stop" ]; then

  # Get system time from hardware clock
  /sbin/hwclock --adjust
  /sbin/hwclock --hctosys
  
  # cleanup
  # (delete /tmp etc)
  : > /var/run/utmp
  rm -f -r /tmp/*
  rm -f -r /tmp/.[^.]*
  rm -f -r /tmp/..?*
  rm -f /var/locks/*
  rm -f -r /var/run/dbus/*

  # Configure random number generator
  cat /var/state/random-seed > /dev/urandom
  
  # Configure network
  /sbin/ifconfig lo 127.0.0.1

  # You can put other static configuration here:
  #/sbin/ifconfig eth0 192.168.1.38 netmask 255.255.255.0 broadcast 192.168.1.255

  /bin/hostname myhost

  # networking daemons
  /usr/libexec/syslogd
  /usr/sbin/sshd
  /usr/libexec/inetd
  
  # /usr/sbin/alsactl restore

  # Prevent spurious messages from kernel to console
  # (syslog will still catch them). Default in 2.4.18 is "7 4 1 7".
  # in particular this prevents "unknown scancode" messages from unrecognized
  # keys on funky keyboards, and module loading messages.
  echo "3 4 1 7" > /proc/sys/kernel/printk

  # Printing
  #/etc/init.d/hplip start
  /etc/init.d/cups start
  
else

  # The system is being shut down
  # kill some stuff
  #if [ -e /var/run/gpm.pid ]; then kill `cat /var/run/gpm.pid`; fi

  /etc/init.d/cups stop
  
  # echo "Saving random number seed..."
  dd if=/dev/urandom of=/var/state/random-seed bs=512 count=1 2> /dev/null

fi;
