# Dinit
v0.17.0 (beta release)

This is the README for Dinit, the service manager and init system. It is
intended to provide an overview; For full documentation please check the manual pages. 
The impatient may wish to check out the [getting started guide](doc/getting_started.md).

Dinit is used as the init system for [Chimera Linux](https://chimera-linux.org/), and is an init
system option for [Artix Linux](https://artixlinux.org/).

---

_Dinit is free software. You may wish to consider [sponsoring Dinit's development](https://github.com/sponsors/davmac314)_.

---

## Contents

1. [Introduction](#introduction)
    1. [Features](#features)
    2. [Target platforms](#target-platforms)
    3. [Other information](#other-information)
2. [Reporting issues](#reporting-issues)
3. [Configuring services](#configuring-services)
    1. [Service types](#service-types)
    2. [Service description files](#service-description-files)
4. [Running Dinit](#running-dinit)
5. [Controlling services](#controlling-services)
    1. [Service hierarchy and states](#service-hierarchy-and-states)
    2. [Using dinitctl](#using-dinitctl)


## Introduction

_Dinit_ is a service supervisor with dependency support which can also
act as the system "init" program. It was created with the intention of
providing a portable init system with dependency management, that was
functionally superior to many extant inits. Development goals include clean design,
robustness, portability, usability, and avoiding feature bloat (whilst still
handling common - and some less-common - use cases). Dinit is designed to
_integrate with_ rather than subsume or replace other system software.


### Features

Dinit can launch multiple services in parallel, with dependency management
(i.e. if one service's operation depends on another, the latter service will be
started first). It  can monitor the process corresponding to a service, and re-start it
if it dies, and it can do this in an intelligent way - first "rolling back" all dependent
services, and restarting them when their dependencies are satisfied. The _dinitctl_ tool can
be used to start or stop services and check their state.

Dinit is designed to run as either as a system service manager (runs as root,
uses system paths for configuration) or a user process (runs as a user,
uses paths in the user's home directory for configuration).


### Target platforms

Dinit is designed to work on POSIXy operating systems such as Linux and
OpenBSD. It is written in C++ and uses the [Dasynq](http://davmac.org/projects/dasynq/)
event handling library, which was written especially to support Dinit. (Note
that a copy of Dasynq is bundled with Dinit, so a separate copy is not
required for compilation; however, the bundled copy does not include the
documentation or test suite).


### Other information

See [doc/COMPARISON](doc/COMPARISON) for a comparison of Dinit with similar
software packages.

Dinit is licensed under the Apache License, version 2.0. A copy of this
license can be found in the [LICENSE](LICENSE) file.

This software was written by Davin McCall <davmac@davmac.org> with contributions
from many others. See [CONTRIBUTORS](CONTRIBUTORS).

See [BUILD](BUILD) for information on how to build Dinit. See the [doc](doc)
directory for information on design, code style, guidelines for contributions, and
end-user-oriented documentation.

Full documentation for Dinit is available in the form of manual (man) pages:
- [dinit(8)](https://davmac.org/projects/dinit/man-pages-html/dinit.8.html) - the _dinit_ daemon
- [dinit-service(5)](https://davmac.org/projects/dinit/man-pages-html/dinit-service.5.html) -
  service description format and service settings
- [dinitctl(8)](https://davmac.org/projects/dinit/man-pages-html/dinitctl.8.html) - _dinitctl_, a
  utility to control the dinit daemon and manage services
- [dinitcheck(8)](https://davmac.org/projects/dinit/man-pages-html/dinitcheck.8.html) - _dinitcheck_,
  a utility to check service descriptions for errors/lint
- [dinit-monitor(8)](https://davmac.org/projects/dinit/man-pages-html/dinit-monitor.8.html) -
  _dinit-monitor_, a utility to monitor a service and run a command when service state changes

A brief guide to some basic usage is included in the latter part of this README.


## Reporting issues

Please use [Github issues](https://github.com/davmac314/dinit/issues) to report bugs,
and provide as much information as is necessary to reliably reproduce the issue.

Please do not file feature requests unless you are working on system integration (eg. you
are a package maintainer for a distribution that supports Dinit, or you are working to
provide Dinit support for a particular distribution) and need to solve a real problem, or
unless you are willing to provide patches (in this case you can open an issue for discussion -
in which case please also see the [CONTRIBUTING](doc/CONTRIBUTING) file).


## Configuring services

This section and the following sections are intended as an introductory guide, and to give
a feel for what using Dinit is like. For a complete reference, see the _man_ pages:
[dinit(8)](https://davmac.org/projects/dinit/man-pages-html/dinit.8.html) and
[dinit-service(5)](https://davmac.org/projects/dinit/man-pages-html/dinit-service.5.html).


### Service types

A "service" is nominally a persistent process or system state. The two main
types of service are a _process_ service (represented by a an actual process)
and a _scripted_ service (which is started and stopped by running a process -
often a shell script - to completion). There are also _bgprocess_ services
and _internal_ services. See the [dinit-service(5)](https://davmac.org/projects/dinit/man-pages-html/dinit-service.5.html)
manual page for details.

Many programs that you might want to run under Dinit's supervision can run
either "in the foreground" or as a daemon ("in the background"), and the
choice is dictated by a command line switch (for instance the -D and -F
switches to Samba's "smbd"). Although it might seem counterintuitive,
the "foreground" mode should be used for programs registered as process
services in Dinit; this allows Dinit to monitor the process.


### Service description files

Dinit discovers services by reading _service description files_. These files
reside in a directory (`/etc/dinit.d` is the default "system" location, with
`/usr/local/lib/dinit.d` and `/lib/dinit.d` also searched; the default user
location is `$HOME/.config/dinit.d`) and the name of a service description file
matches the name of the service it configures.

For example, a service named "mysql" might be configured via the service description
file named `/etc/dinit.d/mysql`. Service descriptions are loaded lazily, as needed
by Dinit; so, this service description file will usually be read when the mysql
service is first started.

(An example of a complete set of system service descriptions can be found in
the [doc/linux/services](doc/linux/services) directory).

A service description file has a textual format and consists of a number of
parameter settings. Some examples of the available parameters are:

    type = process | bgprocess | scripted | internal
    command = ...
    stop-command = ...
    run-as = (user-id)
    restart = (boolean)
    logfile = ...
    pid-file = ...
    options = ...
    depends-on = (service name)
    depends-ms = (service name)
    waits-for = (service name)
    
Typically, a service which runs as a process will use the `command` setting, and include a
`waits-for` dependency on a number of other services (to ensure that the system is ready
for general operation). For example, a service description for `sshd` might look like the following:

    type = process
    command = /usr/sbin/sshd -D
    waits-for = syslogd
    depends-on = loginready

In this example, `syslogd` and `loginready` are also services (which must have their own service
descriptions).

A wide range of service settings and options are available.
Please see the [manual page](https://davmac.org/projects/dinit/man-pages-html/dinit-service.5.html)
for a full list.


## Running Dinit

The main Dinit executable is called `dinit`.

Dinit can run as the system "init" - the first process started by the kernel
on boot - which is normally done by linking or copying `dinit` to `/sbin/init`.
This is currently supported only on Linux. It requires having suitable service
descriptions in place and should be attempted only by those comfortable
with low-level system administration and recovery. See [doc/linux](doc/linux) directory for more
information.

Dinit can also run as a normal process, and can be started in this case by a
regular user.

By default, regardless of whether it runs as a system or user process, Dinit
will look for and start the service named "boot". This service should be
configured with dependencies which will cause any other desired services to
start. You can specify alternative services to start via the `dinit` command
line (consult the [manual page](https://davmac.org/projects/dinit/man-pages-html/dinit.8.html)
for more information).


## Controlling services

### Service hierarchy and states

Services can depend on other services for operation, and so form a
dependency hierarchy. Starting a service which depends on another
causes that other service to start (and the first service waits until
the latter has started before its process is launched and it is itself
considered started).

Services are considered _active_ when they are not stopped. Services
can also be explicitly marked as active (this normally happens when you
explicitly start a service). Finally, a service with an active dependent
is also considered active.

If a service stops and becomes inactive (i.e. it is not explicitly marked
active and has no active dependents) then any services it depends on will
also be stopped (becoming inactive) unless they have other active dependents,
or they were explicitly started and marked active.

What this means is that, in general, starting an (inactive, stopped)
service and then stopping it will return the system to its prior state -
no dependencies which were started automatically will be left running.


### Using dinitctl

You can use the "dinitctl" utility to start and stop services. Typical invocations
are:

    dinitctl start <service-name>
    dinitctl stop <service-name>
    dinitctl release <service-name>
    dinitctl status <service-name>
    dinitctl list

Note that a _start_ marks the service active, as well as starting it if it is
not already started; the opposite of this is actually _release_, which clears
the active mark and stops it if it has no active dependent services.

The _stop_ command by default acts as a release that also causes the service to
stop. If stopping a service would also require a dependent service to stop, a
warning will be issued; the `--force` option will be required to bypass the
warning, though it is generally advisable to stop the dependent systems manually
one-by-one - indirectly force-stopping the boot service may cause every service
to stop, ending user sessions!

When run as root, dinitctl (by default) communicates with the system instance of
dinit. Otherwise, it communicates with a user (personal) instance. This can be
overridden (using `-u` or `-s` for the user or system instance, respectively), but
note that regular users will generally lack the required permission to communicate
with the system instance, which is intended to be controlled only by the root user. 

Here is an example command for starting a service:

    dinitctl start mysql   # start mysql service

You can "pin" a service in either the stopped or started state, which prevents
it from changing state either due to a dependency/dependent or a direct
command:

    dinitctl start --pin mysql  # start mysql service, pin it as "started"
    dinitctl stop mysql  # removes activation, service doesn't stop due to pin
    dinitctl unpin mysql # release pin; service will now stop

You can pin a service in the stopped state in order to make sure it doesn't
get started accidentally (either via a dependency or directly) when you are
performing administration or maintenance.

Check the state of an individual service using the "status" subcommand:

    dinitctl status mysql

The output will tell you the current service state; for a running service, it
may look something like the following:

    Service: mysql
        State: STARTED
        Activation: explicitly started
        Process ID: 3393

Finally, you can list the state of all loaded services:

    dinitctl list

This may result in something like the following:

    [[+]     ] boot
    [{+}     ] tty1 (pid: 300)
    [{+}     ] tty2 (pid: 301)
    [{+}     ] tty3 (pid: 302)
    [{+}     ] tty4 (pid: 303)
    [{+}     ] loginready (has console)
    [{+}     ] rcboot
    [{+}     ] filesystems
    [{+}     ] udevd (pid: 4)
    [     {-}] mysql

The above represents a number of started services and one stopped service
(mysql). Only the boot service is marked active (`[+]` rather than `{+}`); all
other services are running only because they are (directly or indirectly)
dependencies of boot. Services transitioning state (starting or stopping) are
displayed with an arrow indicating the transition direction:

    [[ ]<<   ] mysql     # starting (and marked active)
    [   >>{ }] mysql     # stopping
    
The brackets indicate the target state, which may not be the state to which
the service is currently transitioning. For example:

    [   <<{ }] mysql     # starting, but will stop after starting
    [{ }>>   ] mysql     # stopping, but will restart once stopped

Remember that a _starting_ service may be waiting for its dependencies to
start, and a _stopping_ service may be waiting for its dependencies to stop.

For a complete summary of `dinitctl` command line options, use:

    dinitctl --help

Or, for more detailed help, check the [manual page for dinitctl](https://davmac.org/projects/dinit/man-pages-html/dinitctl.8.html).
