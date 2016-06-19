## Dinit as init: using Dinit as your Linux system's init

Modern linux is a relatively complex system, these days. You can use Dinit,
in conjunction with other software, to boot your system and replace your current
init system (which on most main distributions is now Systemd, Sys V init, or
OpenRC).

The additional software required can be broken into _essential_ and
_optional_ packages, which are detailed in following sections. For example service
files, please check the "services" subdirectory.


# General notes

It is common to use "devtmpfs" on /dev, and the kernel can actually mount it
there before it even starts the init process, which can be quite handy; for
one thing it means that a range of device nodes will be available by default
(including /dev/console, which dinit may need to display any output, as well
as the block device used for the root mount, which must be available in
order to perform the initial fsck). You must configure your kernel
appropriately for this to happen.

(actually, it seems that dinit manages output without /dev/console; probably
the kernel is giving it appropriate stdin/out/err file descriptors. I'm not
sure if this was the case for older kernels).

The /dev filesystem on linux after boot is usually managed by a "device node
manager", such as Udev (which is now distributed only with Systemd) or
Eudev. Even this is technically optional - you can still populate your root
filesystem with device nodes directly - but I highly recommend using an
automated system.

Various other virtual filesystems are mounted as standard on Linux these
days. They include:

- /sys - sysfs - representation of devices, buses, drivers etc; used by udev etc.
- /sys/fs/cgroup - cgroupfs - control groups
- /proc - procfs - information about running processes, and various kernel
  interfaces
- /dev/shm - tmpfs - used for shared memory
- /dev/pts - devpts - pseudoterminal devices
- /run - tmpfs - storage for program state; used by udev

These filesystems (particularly /sys, /proc and /run) need to be mounted
quite early as they will be used by early-boot processes.

Many Linux distributions are now built around Systemd. Much of what Systemd
manages was previously managed by other utilities/daemons (syslogd, inetd,
cron, cgmanager, etc) and these can still be used to provide their original
functionality, although at the cost of the losing automated integration.

Some packages may rely on the "logind" functionality of Systemd for
session/seat management. This same functionality is also provided by
ConsoleKit2, though I'm not to what degree nor level of compatibility.

In general I've found it quite possible to run a desktop system with Dinit
in place of SystemD, but my needs are minimal. If you're running a
full-fledged desktop environment like Gnome or KDE you may experience
problems (which, I believe, should not be intractable, but which may require
implementation/shims of Systemd APIs in some cases).

The basic procedure for boot (to be implemented by services) is as follows:

- mount early virtual filesystems
- start device node manager
- trigger device node manager (udevadm trigger --action=add) to add
  boot-time device nodes (possibly not necessary if using kernel-mounted
  devtmpfs)
- run root filesystem check
- remount root filesystem read-write
- start syslog deamon
- start other daemons as appropriate (dhcpcd, any networking daemons)
- start getty instances virtual terminals

The service description files and scrips in the "services" subdirectory
provide a template for accomplishing the above, but may need some adjustment
for your particular configuration.


# Essential packages

First, a device node manager. I recommend "Eudev".

- Eudev - the Gentoo fork of Udev; https://github.com/gentoo/eudev
- Vdev - "a device file manager and filesystem" and a "work in progress";
  https://github.com/jcnelson/vdev
- Mdev may also be an option; it is part of the "busybox" utility suite.

Then, a "getty" and "login" program. Both can be found in the util-linux
package, at: https://www.kernel.org/pub/linux/utils/util-linux


# Optional packages

ConsoleKit2, to act as seat/sesion manager (functionality otherwise
provided by Systemd):
https://github.com/ConsoleKit2/ConsoleKit2

cgmanager, the control group manager; you probably want this if you use
ConsoleKit2, and maybe if you want to use containers:
https://github.com/lxc/cgmanager

However, I believe that cgmanage works with the old (v1) cgroups interface.
I expect that v2 cgroups together with cgroup namespaces as found in newer
kernels will render it obselete.
