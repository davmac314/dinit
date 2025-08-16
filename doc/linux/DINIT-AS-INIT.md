# Dinit as init: using Dinit as your Linux system's init

You can use Dinit, in conjunction with other software, to boot your system.

This document is intended as a guide for building a system around Dinit. The
reader is assumed to be knowledgeable about how a Linux system works and the
various components that it may be comprised of. If making changes to an
existing system, please make backups and/or be prepared to recover a system
that fails to boot. Support cannot be provided.


## Converting an existing system

If running a Linux distribution, it is theoretically possible to replace your
current init system (which on most main distributions is now Systemd, Sys V
init, or OpenRC) with Dinit. However: Be warned that a modern Linux system is
complex and changing your init system will require some effort and
preparation. It is not a trivial task to take a system based on a typical
distribution that uses some particular init system and make it instead boot
with Dinit. You need to set up suitable service description files for your
system; at present there are no automated conversion tools for converting
service descriptions or startup scripts from other systems.

This guide is focused mainly on building a new system "from scratch" rather
than for converting an existing system to use Dinit.

**Do not attempt to convert an existing system to use Dinit, if you are not
fully aware of what is necessary, how to debug boot issues, and how to roll
back any changes.** 

To proceed with minimal risk, it is recommended that you do not replace the
`/sbin/init` executable and instead use the `init=/sbin/dinit` kernel
command-line option when booting in order to test that services are set up
correctly. If your system boots using an initial RAM-based filesystem or RAM
disk - which is likely - then support for the `init=...` option is dependent
on the setup of that initial filesystem.


## Required steps

To get a system up and running, the following are generally necessary:

1. Set up a boot loader - outside the scope of this document.
2. Set up initial RAM-based disk/filesystem (initramfs, initrd). In general
   this will invoke a small custom init (not dinit) which does basic system
   preparation such as loading modules, mounting some essential pseudo
   filesystems (such as /proc, /dev, /sys and /run), finding and mounting the
   root filesystem, switching to the new root and executing /sbin/init (i.e.
   dinit). Further details are outside the scope of this document.
3. Init services - these are run by Dinit and are responsible for the ongoing
   system startup.

The example services (see link below) are mostly designed for a system built
"from scratch" rather than based on an existing distribution, and should be
modified accordingly to keep existing functionality provided by your
distribution and to work with its boot mechanism.

Once you have service descriptions ready, you can test Dinit by adding
"init=/sbin/dinit" (for example) to the kernel command line when booting. 
To have Dinit run as your system init (once you are satisfied that the service
descriptions are correct and that the system is bootable via Dinit), replace
your system's `/sbin/init` with a link to the `dinit` executable. 

*Note*: if your system is based on a distribution and boots via an
"initrd"/"initramfs" (initial RAM-disk image or RAM-based filesystem), it may
or may not honour kernel options such as "init=...", and it may or may not
pass options such as "single" on to Dinit (which enables single-user mode).
In order to be able to follow the advice/instructions in this guide, you might
need to either adjust the ramdisk image or switch to mounting the root
filesystem directly; consult kernel, bootloader and distribution documentation
for details (which are beyond the scope of this guide).


## General notes

For example service description files, please check the [services](services)
subdirectory (and see descriptions of all of them below).

It is recommended to use an initial RAM filesystem which mounts the following
systems before mounting the root filesystem (read-only) and passing control to
dinit:

- `/proc`
- `/sys`
- `/dev` (with a `devtmpfs` or `tmpfs` instance, appropriately populated)
- `/run` (with a `tmpfs` instance)

In particular having a writable `/run` allows the dinit control socket to be
created immediately as dinit starts, which is useful especially for boot
recovery in case the system becomes otherwise unbootable. 

The `/dev` filesystem on linux after boot is usually managed by a "device node
manager", such as Udev (which is now distributed only with Systemd) or
Eudev. Even this is technically optional - you can still populate your root
filesystem with device nodes directly - but I highly recommend using an
automated system.

Various other virtual filesystems are mounted as standard on Linux these
days. They include:

- `/sys` - `sysfs` - representation of devices, buses, drivers etc; used by
  udev etc.
- `/sys/fs/cgroup` - `cgroupfs` - control groups.
- `/proc` - `procfs` - information about running processes, and various kernel
  interfaces.
- `/dev/shm` - `tmpfs` - used for shared memory
- `/dev/pts` - `devpts` - pseudoterminal devices (used by terminal emulators
  and for remote logins).
- `/run` - `tmpfs` - storage for program state, and often used as the location
  for control sockets (including for dinit).

Many Linux distributions are now built around Systemd. Much of what Systemd
manages was previously managed by other utilities/daemons (syslogd, inetd,
cron, cgmanager, etc) and these can still be used to provide their original
functionality, although at the cost of the losing automated integration.

Some packages may rely on the "logind" functionality of Systemd for
session/seat management. This same functionality is also provided by the
Elogind and ConsoleKit2 packages.

The basic procedure for boot (to be implemented by services) is as follows:

- Mount early virtual filesystems (if not done by initramfs).
- Start device node manager.
- Set the system time from the hardware realtime clock, if available.
- Run root filesystem check.
- Remount the root filesystem read-write.
- Various miscellaneous tasks: seed the random number generator, configure the
  loopback interface, cleanup files in /tmp, /var/run and /var/lock
- Start syslog deamon.
- Start other daemons as appropriate (dhcpcd, any networking daemons).
- Start getty instances on virtual terminals (allows user login).

The service description files and scripts in the `services` subdirectory
provide a template for accomplishing the above, but may need some adjustment
for your particular configuration.

The additional software required can be broken into _essential_ and
_optional_ packages, which are detailed in following sections. 


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
boot time) although it is less usable as an interactive shell.

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

Another implementation of D-Bus is **dbus-broker**:
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
which they are expected to start. In general, note that services at this point cannot specify a
`logfile = ...` setting that resides in a normal location (such as `/var/log/...`) since the
root filesystem is not yet read-write; they can log to a file under `/run`, or they can
use `log-type = buffered` to log to an in-memory buffer. 

- `early-filesystems` - this service has no dependencies and so is one of the earliest
  to start. It mounts virtual filesystems including _sysfs_, _devtmpfs_ (on `/dev`)
  and _proc_, via the `early-filesystems.sh` shell script. Note that if startup is via
  an initial ram disk (initrd/initramfs) as is now common, these early filesystems are
  most likely already mounted by that, so this service may not be needed or could be
  edited to remove initrd-mounted filesystems. 
- `udevd` - this services starts the device node manager, udevd (from the eudev package). This
  daemon receives notification of hotplug events from the kernel, and creates device nodes (in
  `/dev`) according to its configuration. Note that "hotplug" events includes initialisation of
  devices even when they are not "hot-pluggable" as such. It is a `type = scripted` service
  because udevd does not support readiness notification; by allowing it to fork, dinit can
  effectively observe when the daemon has initialised. However, it cannot monitor the process. A
  patch to add readiness notification to udev has been submitted
  (https://github.com/eudev-project/eudev/pull/290). 
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
  that output is visible during boot, and is marked `start-interruptible` and
  `skippable` as well as `unmask-intr` so that pressing Ctrl+C can skip the check. The
  default `start-timeout` of 60 seconds is overridden to 0 (no start timeout), since a
  filesystem check may take some time. If the filesystem check requires manual
  intervention, the user is prompted to enter the root password and a maintenance shell
  is spawned (once it is exited, the system is rebooted). The system is also rebooted if
  the filesystem check makes automatic changes that require it.
- `rootrw` - once the root filesystem has been checked, it can be mounted read-write (the
  kernel normally mounts root as read-only). This service has the `starts-rwfs` option set, to
  prompt dinit to create its control socket (if not already done).
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
services can then start (this ordering is enforced by a dependency on `rcboot`):

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

- `recovery` - this service is started by Dinit if boot fails (and if the user, when prompted,
  then chooses the recovery option). It prompts for the root password and then provides a
  shell. It has `restart = false` because once the administrator has finished repairing the system
  configuration, they will exit the recovery shell, and at that point they should be offered the
  various boot failure options (reboot etc) again.
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
option can be used for these services so that their output is visible at startup. There is also
the possibility of using the `log-type = buffer` setting to keep output buffered in memory
instead.
