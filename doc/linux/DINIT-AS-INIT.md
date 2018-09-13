## Dinit as init: using Dinit as your Linux system's init

Modern linux is a relatively complex system, these days. You can use Dinit,
in conjunction with other software, to boot your system and replace your
current init system (which on most main distributions is now Systemd, Sys V
init, or OpenRC).

To have Dinit as your system init, replace your system's `/sbin/init` with a
link to the `dinit` executable.

*Note*: if your system boots via an "initrd" (initial ramdisk image), you
might need to either adjust the ramdisk image to include `dinit` or switch
to mounting the root filesystem directly; consult kernel, bootloader and
distribution documentation for details.

Be warned that it is not a trivial task to take a system based on a typical
Linux distribution and make it instead boot with Dinit. You need to set up
suitable service description files for your system; at present there are no
automated conversion tools for converting service descriptions or startup
scripts from other systems.

The additional software required can be broken into _essential_ and
_optional_ packages, which are detailed in following sections. For example service
files, please check the `services` subdirectory.


# General notes

It is common to use "devtmpfs" on /dev, and the kernel can actually mount it
there before it even starts the init process, which can be quite handy; for
one thing it means that a range of device nodes will be available by default
(including /dev/console, which dinit may need to display any output, as well
as the block device used for the root mount, which must be available in
order to perform the initial fsck). You must configure your kernel
appropriately for this to happen.

(actually, it seems that Dinit manages output without /dev/console; probably
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
- /run - tmpfs - storage for program state (replacement for /var/run); used by
  udev and some other programs

These filesystems (particularly /sys, /proc and /run) need to be mounted
quite early as they will be used by early-boot processes.

Many Linux distributions are now built around Systemd. Much of what Systemd
manages was previously managed by other utilities/daemons (syslogd, inetd,
cron, cgmanager, etc) and these can still be used to provide their original
functionality, although at the cost of the losing automated integration.

Some packages may rely on the "logind" functionality of Systemd for
session/seat management. This same functionality is also provided by
Elogind and ConsoleKit2, though I'm not to what degree nor level of
compatibility.

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

The service description files and scripts in the `services` subdirectory
provide a template for accomplishing the above, but may need some adjustment
for your particular configuration.


# Boot performance compared to Systemd

Boot performance using Dinit _theoretically_ suffers slightly compared to
Systemd due to several factors:

- Dinit doesn't handle file system mounts/checks and spawns external
  processes and/or scripts to manage these
- In the absence of full udev integration, it is more difficult to start
  some services as early as possible. For instance, filesystem mounts need
  to wait for the appropriate device to be recognized by the kernel and
  exported as a device node; similarly network interfaces need to be
  recognized before they are configured. With Dinit, the simplest approach
  is to wait until udev "settles" (via "udevadm settle") before attempting
  mounts and network configuration etc.
- Dinit doesn't do full socket activation (yet). What it can do is pre-open a
  socket while it launches a daemon, which probably improves boot times a
  little but potentially still results in more daemons getting started than
  is strictly necessary.

In the future Dinit might provide functionality to perform basic mounts
without spawning an external process, and will almost certainly support full
socket activation. Some of the more fundamental startup script functionality
might also be offered via some kind of intrinsic service(s). However, where
at all possible, the intention is to keep service functionality separate to
Dinit itself.


# Essential packages for building a Dinit-based system

Other than the obvious system C library and C++ library, you'll need a range
of packages to create a functional Dinit-based system.

First, a device node manager. I recommend "Eudev".

- Eudev - the Gentoo fork of Udev; https://github.com/gentoo/eudev
- Vdev - "a device file manager and filesystem" and a "work in progress";
  https://github.com/jcnelson/vdev
- Mdev may also be an option; it is part of the "busybox" utility suite. I
  don't personally have any experience with it.

Then, a "getty" and "login" program. Both can be found in the util-linux
package, at: https://www.kernel.org/pub/linux/utils/util-linux

Also provided in the util-linux package are standard utilities such as fsck
and mount. You'll probably want e2fsprogs (or the equivalent for your chosen
filesystem): http://e2fsprogs.sourceforge.net/

The syslog daemon from GNU Inetutils is basic, but functional - which makes
it a good fit for a Dinit-based system. https://www.gnu.org/software/inetutils

You will need a shell script interpreter / command line, for which you have
a range of options. A common choice is GNU Bash, but many distributions are
using Dash as the /bin/sh shell because it is significantly faster (affecting
boot time).

- Bash: https://www.gnu.org/software/bash
- Dash: http://gondor.apana.org.au/~herbert/dash


# Optional packages for building a Dinit-based system

ConsoleKit2, to act as seat/sesion manager (functionality otherwise
provided by Systemd):
https://github.com/ConsoleKit2/ConsoleKit2

cgmanager, the control group manager; you probably want this if you use
ConsoleKit2, and maybe if you want to use containers:
https://github.com/lxc/cgmanager

(However, I believe that cgmanager works with the old v1 cgroups interface.
I expect that v2 cgroups together with cgroup namespaces as found in newer
kernels will render it obselete).

Both ConsoleKit2 and cgmanager use Dbus:
https://dbus.freedesktop.org/
