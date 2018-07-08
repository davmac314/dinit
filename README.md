# Dinit
v0.3.0 (pre-release)

This is the README for Dinit, the service manager and init system. It is
intended to provide an overview; For full documentation please check the manual pages. 

## Contents

1. [Introduction](#introduction)
2. [Configuring services](#configuring-services)
    1. [Service types](#service-types)
    2. [Service description files](#service-description-files)
3. [Controlling services](#controlling-services)
    1. [Service hierarchy and states](#service-hierarchy-and-states)
    2. [Using dinitctl](#using-dinitctl)

## Introduction

"Dinit" is a service supervisor with dependency support which can also
act as the system "init" program. It was created with the intention of
providing a portable init system that could serve as a lighter-weight
alternative to the Linux-only Systemd.

Specifically, Dinit can launch multiple services in parallel, with dependency
management (i.e. if one service's operation depends on another, the latter
service will be started first). It  can monitor the process corresponding to a
service, and re-start it if it dies, and it can do this in an intelligent way,
first "rolling back" all dependent services, and restarting them when their
dependencies are satisfied. However, the precise nature of dependency
relations between services is highly configurable.

Dinit includes "dinitctl", a tool to issue commands to the main Dinit
process in order to start or stop services and check their state, as well as
a "shutdown" program (with scripts "halt" and "reboot") to manage shutting
down and restarting the system.

Dinit is designed to work on POSIXy operating systems such as Linux and
OpenBSD. It is written in C++ and uses the [Dasynq](http://davmac.org/projects/dasynq/)
event handling library, which was written especially to support Dinit.

Development goals include clean design, robustness, portability, and
avoiding feature bloat (whilst still handling a variety of use cases).

See [doc/COMPARISON](doc/COMPARISON) for a comparison of Dinit with similar
software packages.

Dinit is licensed under the Apache License, version 2.0. A copy of this
license can be found in the LICENSE file.

Dinit was written by Davin McCall <davmac@davmac.org>.

See BUILD for information on how to build Dinit.


## Configuring services

### Service types

A "service" is nominally a persistent process or system state. The two main
types of service are a _process_ service (represented by a an actual process)
and a _scripted_ service (which is started and stopped by running a process -
often a shell script - to completion). There are also _bgprocess_ services
and _internal_ services.

Many programs that you might want to run under dinit's supervision can run
either "in the foreground" or as a daemon ("in the background"), and the
choice is dictated by a command line switch (for instance the -D and -F
switches to Samba's "smbd"). Although it might seem counterintuitive,
the "foreground" mode should be used for programs registered as process
services in dinit; this allows dinit to monitor the process.

Process services are attractive due to the ease of monitoring (and
restarting) the service, however, they have one inherent problem, which is
that dinit cannot tell when the service is truly started. Once the process
has been launched, dinit assumes that the service has started, but in fact
there will be a short delay before the process sets itself up, starts
listening on sockets, etc; during this time any other process (including
one from a service listed as dependent) which tries to contact it will not
be able to do so. In practice, this is not usually a problem (and external
solutions, like D-Bus, do exist).

A _scripted_ service has separate commands for startup and (optional)
shutdown. Scripted services can be used for tasks such as mounting file
systems that don't need a persistent process, and in some cases can be used
for daemon processes (although Dinit will not be able to supervise a
process that is registered as a scripted service).

A _bgprocess_ service is a mix between a process service and a scripted
service. A command is used to start the service, and once started, the
process ID is expected to be available in a file which Dinit can then
read. Many existing daemons can operate in this way. The process can only be
supervised if Dinit runs as the system "init" (PID 1), or can otherwise mark
itself as a subreaper (which is possible on Linux, FreeBSD and DragonFlyBSD) -
otherwise Dinit can not reliably know when the process has terminated.

(Note, use of bgprocess services type requires care. The file from which the
PID is read is trusted; Dinit may send signals to the specified PID. It
should not be possible for unauthorised users to modify the file contents!)

An _internal_ service is just a placeholder service that can be used to
describe a set of dependencies. An internal service has no corresponding
process.


### Service description files

Dinit discovers services by reading _service description files_. These files
reside in a directory (/etc/dinit.d is the default "system" location, with
"/usr/local/lib/dinit.d" and "/lib/dinit.d" also searched) and their name
matches the name of the service. Service descriptions are loaded lazily, as
needed by Dinit.

A service description file consists of a number of parameter settings.
Settings in the SDF are denoted as a parameter name followed by either an
equal sign or colon and then the parameter value (all on the same line).
Comments begin with a hash mark (#) and extend to the end of the line (they
must be separated from setting values by at least one whitespace character).

Parameter values are interpreted literally, except that:
 - whitespace is collapsed to a single space
 - double quotes can be used around all or part(s) of a parameter to prevent
   whitespace collapse and interpretation of special characters
 - backslash can be used to 'escape' the next character, preventing any
   special meaning from being associated with it. It can be used to include
   non-collapsing whitespace, double-quote marks, and backslashes in the
   parameter value.

Some examples of the available parameters are:

    type = process | bgprocess | scripted | internal
    command = ...
    stop-command = ...
    run-as = (user-id)
    restart = (boolean)
    smooth-recovery = (boolean)
    logfile = ...
    pid-file = ...
    options = ...
    depends-on = (service name)
    depends-ms = (service name)
    waits-for = (service name)
    
Descriptions of individual parameters follows:

    command = (external script or executable, and arguments)

For a 'process' service, this is the process to run.
For a 'scripted' service, this command is run to start the service.

    stop-command = (external script or executable, and arguments)

For a 'scripted' service, this command is run to stop the service.

    run-as = (user-id)
 
Specifies which user to run the process(es) for this service as. The group
id for the process will also be set to the primary group of the specified
user.

    restart = yes | true | no | false

Specifies whether the service should automatically restart if it becomes
stopped (for any reason, including being explicitly requested to stop).
Only active services will restart automatically.

    smooth-recovery = yes | true | no | false
   
For process services only. Specifies that, should the process die, it
can be restarted without bringing the service itself down. This means that
any dependent services do not need to be stopped/restarted. Such recovery
happens regardless of the "restart" setting (if smooth-recovery is enabled,
the service does not reach the stopped state when the process terminates
unexpectedly).

    logfile = (log file path)

Specifies the log file for the service. Output from the service process
will go this file.

    pid-file = (path to file)

For "bgprocess" type services only; specifies the path of the file where
daemon will write its process ID before detaching.

    depends-on = (service name)

This service depends on the named service. Starting this service will
start the named service; the command to start this service will not be
executed until the named service has started. If the named service is
stopped then this service will also be stopped.

    depends-ms = (service name)

Indicates a "milestone dependency" on the named service. This service
requires the named service to start before it starts itself. Once the
named service has started, it remains active due to the dependency, but if
it stops for any reason then the dependency link is broken until the next
time this service is started.

    waits-for = (service name)

When this service is started, wait for the named service to finish
starting (or to fail starting) before commencing the start procedure
for this service. Starting this service will automatically start
the named service.

    options = ( runs-on-console | nosigterm | starts-rwfs | starts-log ) ...

Specifies various options for this service:

`no-sigterm` : specifies that the TERM signal should not be send to the
              process to terminate it. (Another signal can be specified using
              the "termsignal" setting; if no other signal is specified, NO
              signal will be sent).

`runs-on-console` : specifies that this service uses the console; its input
              and output should be directed to the console. A service running
              on the console prevents other services from running on the
              console (they will queue for the console).
              The "interrupt" key (normally control-C) will be active for
              process / scripted services that run on the console. Handling
              of an interrupt is determined by the service process, but
              typically will cause it to terminate.
              
`starts-on-console` : specifies that this service uses the console during
              service startup. This is implied by runs-on-console, but can
              be specified separately for services that need the console
              while they start but not afterwards.
              This setting is not applicable to regular "process" services,
              but can be used for "scripted" and "bgprocess" services. It
              allows for interrupting startup via the "interrupt" key
              (normally control-C). This is useful to allow filesystem checks
              to be interrupted/skipped.

`start-interruptible` : this service can have its startup interrupted
              (cancelled) if it becomes inactive while still starting.
              The SIGINT signal will be sent to the signal to cancel its
              startup. This is meaningful only for scripted and bgprocess
              services. 

Please see the manual page for a full list of service parameters and options.


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
also be marked inactive and stopped unless they have other active
dependents, or were explicitly started and marked active.

What this means is that, in general, starting an (inactive, stopped)
service and then stopping it will return the system to its prior state -
no dependencies which were started automatically will be left running.

### Using dinitctl

You can use the "dinitctl" utility to start and stop services. Typical invocations
are:

    dinitctl start <service-name>
    dinitctl stop <service-name>
    dinitctl release <service-name>

Note that a "start" markes the service active, as well as starting it if it is
not already started; the opposite of this is actually "release", which clears
the active mark and stops it if it has no active dependent services. The "stop"
command by default acts as a "release" which also forces the service to stop
(although it may then immediately restart, depending on how it and its
dependents are configured).

Use the "-s" switch to talk the "system" instance of dinit, rather than a
personal instance, e.g:

    dinitctl -s start mysql   # start system mysql service

For complete details on the command line, use:

    dinitctl --help

You can "pin" a service in either the stopped or started state, which prevents
it from changing state either due to a dependency/dependent or a direct
command:

    dinitctl -s start --pin mysql  # start mysql service, pin it as "started"
    dinitctl -s stop mysql  # issues stop, but doesn't take effect due to pin
    dinitctl -s unpin mysql # release pin; service will now stop

You can pin a service in the stopped state in order to make sure it doesn't
get started accidentally (either via a dependency or directly). You can also
use it to temporarily keep stopped a service that would otherwise restart
immediately when you stopped it (because it, or a dependent, is configured
to restart automatically).

Finally, you can list the state of all loaded services:

    dinitctl -s list

This may result in something like the following:

    [{+}     ] boot
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
(mysql). Services transitioning state (starting or stopping) are displayed
with an arrow indicating the transition direction:

    [{ }<<   ] mysql     # starting
    [   >>{ }] mysql     # stopping
    
The curly brackets indicate the desired state, which may not be the state to
which the service is currently transitioning. For example:

    [   <<{ }] mysql     # starting, but will stop after starting
    [{ }>>   ] mysql     # stopping, but will restart once stopped

Remember that a "starting" service may be waiting for its dependencies to
start, and a "stopping" service may be waiting for its dependencies to stop.
