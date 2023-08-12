# Dinit as init: using Dinit as your Linux system's init

You can use Dinit, in conjunction with other software, to boot your system and
replace your current init system (which on most main distributions is now
Systemd, Sys V init, or OpenRC).

Be warned that a modern Linux system is complex and changing your init system
will require some effort and preparation. It is not a trivial task to take a
system based on a typical Linux distribution that uses some particular init
system and make it instead boot with Dinit. You need to set up suitable
service description files for your system; at present there are no automated
conversion tools for converting service descriptions or startup scripts from
other systems. For example service files, please check the [services](services)
subdirectory (and see descriptions of all of them below).

Once you have service descriptions ready, you can test Dinit by adding
"init=/sbin/dinit" (for example) to the kernel command line when booting. 
To have Dinit run as your system init (once you are satisfied that the service
descriptions are correct and that the system is bootable via Dinit), replace
your system's `/sbin/init` with a link to the `dinit` executable. 

*Note*: if your system boots via an "initrd" (initial ramdisk image), you
might need to either adjust the ramdisk image to include `dinit` or switch
to mounting the root filesystem directly; consult kernel, bootloader and
distribution documentation for details.

The additional software required can be broken into _essential_ and
_optional_ packages, which are detailed in following sections. 


## General notes

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
Elogind and ConsoleKit2, though I'm not sure to what degree nor level of
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
  boot-time device nodes (or run additional actions for nodes already created
  if using kernel-mounted devtmpfs)
- set the system time from the hardware realtime clock
- run root filesystem check
- remount root filesystem read-write
- start syslog deamon
- various miscellaneous tasks: seed the random number generator, configure the
  loopback interface, cleanup files in /tmp, /var/run and /var/lock
- start other daemons as appropriate (dhcpcd, any networking daemons)
- start getty instances on virtual terminals

The service description files and scripts in the `services` subdirectory
provide a template for accomplishing the above, but may need some adjustment
for your particular configuration.


## Essential packages for building a Dinit-based system

Other than the obvious system C library and C++ library, you'll need a range
of packages to create a functional Dinit-based system.

First, a device node manager. I recommend "Eudev".

- Eudev - the Gentoo fork of Udev; https://github.com/gentoo/eudev
- Mdev may also be an option; it is part of the "busybox" utility suite. I
  don't personally have any experience with it.

Then, a "getty" and "login" program. Both can be found in the util-linux
package, at: https://www.kernel.org/pub/linux/utils/util-linux

Also provided in the util-linux package are standard utilities such as fsck
and mount. You'll probably want e2fsprogs (or the equivalent for your chosen
filesystem): http://e2fsprogs.sourceforge.net/

There are plenty of syslog daemons; the one I recommend is troglobit's enhanced
version of sysklogd. The syslog daemon from GNU Inetutils is another option. 

- Troglobit's sysklogd: https://github.com/troglobit/sysklogd
- Inetutils: https://www.gnu.org/software/inetutils

You will need a shell script interpreter / command line, for which you have
a range of options. A common choice is GNU Bash, but many distributions are
using Dash as the /bin/sh shell because it is significantly faster (affecting
boot time) although it is basically unusable as an interactive shell.

- Bash: https://www.gnu.org/software/bash
- Dash: http://gondor.apana.org.au/~herbert/dash


## Optional packages for building a Dinit-based system

**elogind**, to act as seat/session manager (extracted from Systemd's logind):
https://github.com/elogind/elogind

Alternatively, **ConsoleKit2**:
https://github.com/ConsoleKit2/ConsoleKit2

**cgmanager**, the control group manager; you probably want this if you use
ConsoleKit2, and maybe if you want to use containers:
https://github.com/lxc/cgmanager

(However, I believe that cgmanager works with the old v1 cgroups interface.
I expect that v2 cgroups together with cgroup namespaces as found in newer
kernels will render it obsolete).

The above use **D-Bus**:
https://dbus.freedesktop.org/

Another implementation of D-Bus is dbus-broker:
https://github.com/bus1/dbus-broker


## Explanation of example services

A set of example service description files can be found in the [services](services)
subdirectory; these can be used to boot a real system, assuming the appropriate
package dependencies are in place. Here are explanations for each package:

- `boot` - the internal service which is started on boot. Its dependencies are mostly
  listed in the `boot.d` directory. However, the ttyX services are directly listed
  as _depends-ms_ type dependencies in the service description file. The `boot.d`
  directory simplifies enabling services for the package manager, and enables the
  use of `dinitctl enable` and `dinitclt disable` commands to enable or disable
  particular services.
  
Having covered `boot`, we'll go through the other services in roughly the order in
which they are expected to start:
  
- `early-filesystems` - this service has no dependencies and so is one of the earliest
  to start. It mounts virtual filesystems including _sysfs_, _devtmpfs_ (on `/dev`)
  and _proc_, via the `early-filesystems.sh` shell script. Note that if startup is via
  an initial ram disk (initrd, initfs) as is now common, these early filesystems are
  most likely already mounted by that, so this service may not be needed or could be
  edited to remove initrd-mounted filesystems. 
- `udevd` - this services starts the device node manager, udevd (from the eudev package).
  This daemon receives notification of hotplug events from the kernel, and creates
  device nodes (in `/dev`) according to its configuration. Note that "hotplug" events
  includes initialisation of devices even when they are not "hot-pluggable".
- `udev-trigger` - this is a scripted service which triggers device add actions
  for all currently present devices. This is required for `udevd` to process devices
  which already existed when it started.
- `udev-settle` - this is a scripted service which waits until udevd judges that the
  device list has "settled", that is, there are no more attached devices which are still
  potentially to be found and reported by the kernel. This is something of a hack, and
  should not really be relied on, but is convenient to keep our example scripts
  reasonably simple. A less hacky alternative would be to have triggered services
  representing particular devices (disks, network interfaces, etc) that other services
  require; see the 'netdev-enp3s0' service for example.
- `hwclock` - this sets the current time (according to the kernel) from the hardware
  clock. It depends on `udevd` as it needs the hardware clock device node to be present.
- `modules` - this service runs the `modules.sh` script, which checks whether the kernel
  supports modules via the proc filesystem, so it depends-on the `early-filesystems`
  service. If modules are supported by the kernel, the `/etc/modules` file is read;
  each line can contain the name of a kernel module (which will then be loaded) and
  arguments.
- `rootfscheck` - via the `rootfscheck.sh` script, this service runs a filesystem check
  on the root filesystem (if it is marked dirty). The script runs "on the console" so
  that output is visible during boot, and is marked `start-interruptible` and `skippable`
  so that pressing Ctrl+C can skip the check. The default `start-timeout` of 60 seconds is
  overridden to 0 (no start timeout), since a filesystem check may take some time. If the
  filesystem check requires manual intervention, the user is prompted to enter the root
  password and a maintenance shell is spawned (once it is exited, the system is rebooted).
  The system is also rebooted if the filesystem check makes automatic changes that require
  it.
- `rootrw` - once the root filesystem has been checked, it can be mounted read-write (the
  kernel normally mounts root as read-only).
- `auxfscheck` - runs fsck for the auxillary filesystems (apart from the root filesystem)
  which are needed for general system operation. Any filesystems listed in `/etc/fstab`
  will be checked, depending on how they are configured.
- `filesystems` - this service mounts any auxillary filesystems. It also enables the swap
  (the example script expects a swapfile at `/swapfile`). It depends on `auxfscheck`, i.e.
  it does not mount filesystems before they have been checked.
- `rcboot` - this service runs the `rcboot.sh` script, which performs a number of basic
  functions:
  - cleans up the `/tmp` directory, the `/var/lock` directory and the `/var/run` directory
  - creates directories that may be needed under `/var/run`
  - copies saved entropy to the `/dev/urandom` device
  - configures the "lo" (loopback) network device (relies on `ifconfig` from the GNU
    inetutils package)
  - sets the hostname
  On shutdown, it saves entropy from `/dev/urandom` so that it can be restored next boot.
  
By the time `rcboot` has started, the system is quite functional. The following additional
services can then start:

- `syslogd` - the logging daemon. This service has the `starts-log` option set, so that
  Dinit will commence logging (from its buffer) once the service starts. The example service
  relies on troglobit's sysklogd. Unfortunately it must be a `bgprocess` as it does not
  support signalling readiness via a file descriptor.
- `late-filesystems` - check and mount any filesystems which are not needed for general
  system operation (i.e. "user" filesystems). It's not expected that other services will
  depend on this service. This service uses the `late-filesystems.sh` script; configure
  late filesystems via that script.
- `dbusd` - starts the DBus daemon (system instance), which is used by other services to
  provide an interface to user processes
- `dhcpcd` - starts a DHCP client daemon on a network interface (the example uses `enp3s0`).
- 'netdev-enp3s0' - a triggered service representing the availablility of the `enp3s0` network
  interface. See the service description file for details. Note that the 'udev-settle` service
  somewhat makes this redundant, as would use of a suitable network manager; it is provided for
  example purpsoses.
- `sshd` - starts the SSH daemon.

We want most of the preceding services to be started before we allow a user to login. To that
end, we have:

- `loginready` - an internal service, which depends on `rcboot`, `dbusd`, `udevd` and `syslogd`.
  This service has option `runs-on-console` set, to prevent Dinit from outputting service
  status messages to the console once login is possible.
- `ttyX` where X is 1-6 - a service which starts a login prompt on the corresponding virtual
  terminal (and which depends on `loginready`). Note that the tty services do _not_ have the
  `runs-on-console` option, since that would conflict with `loginready` (and with each other)
  and ultimately prevent the tty services running, as only one service can run on the console
  at a time.

There are two additional services, which are not depended on by any other service, and so do
not normally start at all:

- `recovery` - this service is started by Dinit if boot fails (and if the user when prompted
  then chooses the recovery option). It prompts for the root password and then provides a
  shell.
- `single` - this is a "single user mode" startup service which simply runs a shell. An
  unprivileged user cannot normally start this; doing so requires putting "single" on the
  kernel command line. When the shell exits, the `chain-to` setting will cause normal
  startup to resume (i.e. via the `boot` service).

While they are a little rough around the edges, these service definitions demonstrate the
essentials of getting a system up and running.


## Testing and debugging tips

You can pass arbitrary arguments to dinit by using a shell script in the place of `/sbin/init`,
which should `exec` dinit (so as to give it the same PID). Don't forget to make the script
executable and to include the shebang line (`#!/bin/sh` or similar).

You can run a shell directly on a virtual terminal by adding a `ttyN` service or modifying one
of the existing ones (see the example services). You'll still need getty to setup the
terminal for the shell; an example setting:
```
command = /sbin/agetty tty6 linux-c -n -l /bin/bash
```
You can remove most or all dependencies from this service so that it starts early, and set the
`term-signal = none`, as well as setting `stop-timeout = 0` (i.e. disabling stop timeout), so that
it will not be killed at shutdown (you will need to manually exit the shell to complete shutdown).
This means you always have a shell available to check system state when something is going wrong.
While this is not something you want to enable permanently, it can be a good tool to debug a
reproducible boot issue or shutdown issue.


## Caveats

For services which specify a `logfile`, the location must be writable when the service starts
(otherwise the service will fail to start). Typically this means that nearly all services must
depend on the service that makes the root filesystem writable. For services that must start
before the root filesystem becomes writable, it may be possible to log in `/run` or another
directory that is mounted with a RAM-based filesystem; alternatively, the `shares-console`
option can be used for these services so that their output is visible at startup.
