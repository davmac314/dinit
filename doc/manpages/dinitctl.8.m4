changequote(`@@@',`$$$')dnl
@@@.TH DINITCTL "8" "$$$MONTH YEAR@@@" "Dinit $$$VERSION@@@" "Dinit \- service management system"
.SH NAME
dinitctl \- control services supervised by Dinit
.\"
.SH SYNOPSIS
.\"
.PD 0
.nh
.HP
.B dinitctl
[\fIoptions\fR] \fBstart\fR [\fB\-\-no\-wait\fR] [\fB\-\-pin\fR] \fIservice-name\fR
.HP
.B dinitctl
[\fIoptions\fR] \fBstop\fR [\fB\-\-no\-wait\fR] [\fB\-\-pin\fR] [\fB\-\-ignore\-unstarted\fR] \fIservice-name\fR
.HP
.B dinitctl
[\fIoptions\fR] \fBstatus\fR \fIservice-name\fR
.HP
.B dinitctl
[\fIoptions\fR] \fBis-active\fR \fIservice-name\fR
.HP
.B dinitctl
[\fIoptions\fR] \fBis-failed\fR \fIservice-name\fR
.HP
.B dinitctl
[\fIoptions\fR] \fBrestart\fR [\fB\-\-no\-wait\fR] [\fB\-\-ignore\-unstarted\fR] \fIservice-name\fR
.HP
.B dinitctl
[\fIoptions\fR] \fBwake\fR [\fB\-\-no\-wait\fR] \fIservice-name\fR
.HP
.B dinitctl
[\fIoptions\fR] \fBrelease\fR [\fB\-\-ignore\-unstarted\fR] \fIservice-name\fR
.HP
.B dinitctl
[\fIoptions\fR] \fBunpin\fR \fIservice-name\fR
.HP
.B dinitctl
[\fIoptions\fR] \fBunload\fR \fIservice-name\fR
.HP
.B dinitctl
[\fIoptions\fR] \fBreload\fR \fIservice-name\fR
.HP
.B dinitctl
[\fIoptions\fR] \fBlist\fR
.HP
.B dinitctl
[\fIoptions\fR] \fBshutdown\fR
.HP
.B dinitctl
[\fIoptions\fR] \fBadd-dep\fR \fIdependency-type\fR \fIfrom-service\fR \fIto-service\fR
.HP
.B dinitctl
[\fIoptions\fR] \fBrm-dep\fR \fIdependency-type\fR \fIfrom-service\fR \fIto-service\fR
.HP
.B dinitctl
[\fIoptions\fR] \fBenable\fR [\fB\-\-from\fR \fIfrom-service\fR] \fIto-service\fR
.HP
.B dinitctl
[\fIoptions\fR] \fBdisable\fR [\fB\-\-from\fR \fIfrom-service\fR] \fIto-service\fR
.HP
.B dinitctl
[\fIoptions\fR] \fBtrigger\fR \fIservice-name\fR
.HP
.B dinitctl
[\fIoptions\fR] \fBuntrigger\fR \fIservice-name\fR
.HP
.B dinitctl
[\fIoptions\fR] \fBsetenv\fR [\fIname\fR[=\fIvalue\fR] \fI...\fR]
.\"
.PD
.hy
.\"
.SH DESCRIPTION
.\"
\fBdinitctl\fR is a utility to control services being managed by the
\fBdinit\fR daemon.
It allows starting and stopping services, and listing service status, amongst other actions.
It functions by issuing commands to the daemon via a control socket.
.\"
.SH GENERAL OPTIONS
.TP
\fB\-\-help\fR
Display brief help text and then exit.
.TP
\fB\-\-version\fR
Display version and then exit.
.TP
\fB\-s\fR, \fB\-\-system\fR
Control the system init process (this is the default when run as root).
This option determines the default path to the control socket used to communicate with the \fBdinit\fR daemon
process (it does not override the \fB\-p\fR option).
.TP
\fB\-u\fR, \fB\-\-user\fR
Control the user init process (this is the default when not run as root).
This option determines the default path to the control socket used to communicate with the \fBdinit\fR daemon process
(it does not override the \fB\-p\fR option).
.TP
\fB\-\-socket\-path\fR \fIsocket-path\fR, \fB\-p\fR \fIsocket-path\fR
Specify the path to the socket used for communicating with the service manager daemon.
When not specified, the value from the \fBDINIT_SOCKET_PATH\fR environment variable is used, with
the default path (as documented for \fBdinit\fR(8)) used if the variable is unset.
.TP
\fB\-\-use\-passed\-cfd\fR
Instead of attempting to open a socket connection to the service daemon,
use a pre-opened connection that has been passed to the dinitctl process from its parent
via an open file descriptor.
The file descriptor with the connection is identified by the contents of the DINIT_CS_FD
environment variable.
.TP
\fB\-\-quiet\fR
Suppress status output, except for errors. 
.\"
.SH COMMAND OPTIONS
.TP
\fB\-\-no\-wait\fR
Do not wait for issued command to complete; exit immediately.
.TP
\fB\-\-pin\fR
Pin the service in the requested state. The service will not leave the state until it is unpinned.
.sp
A service that is pinned stopped cannot be marked active, that is, start commands issued to the
service have no effect.
Dependents (via hard dependency relationships) of the pinned service will be unable to start.
.sp
A service that is pinned started cannot be stopped, however its explicit activation can be removed
(eg via the \fBstop\fR or \fBrelease\fR commands).
Once unpinned, a service which is not explicitly activated, and which has no active dependents,
will automatically stop. If a pinned-started service fails to start, the pin is removed.
.sp
Note that a pin takes effect while the service is starting/stopping, before it reaches the target
state. Stopping or restarting a service that is pinned started and which is already starting or
started is not possible. Similarly, starting a service which is pinned stopped and which is stopping
or stopped is not possible.
.TP
\fB\-\-force\fR
Stop the service even if it will require stopping other services which depend on the specified service.
When applied to the \fBrestart\fR command, this will cause the dependent services to also be restarted.
.TP
\fB\-\-ignore\-unstarted\fR
If the service is not started or doesn't exist, ignore the command and return an exit code indicating
success.
.TP
\fIservice-name\fR
Specifies the name of the service to which the command applies.
.TP
\fBstart\fR
Start the specified service.
The service is marked as explicitly activated and will not be stopped automatically if its dependents stop.
If the service is currently stopping it will generally continue to stop before it is then restarted.
.TP
\fBstop\fR
Stop the specified service, and remove explicit activation.
If the service has (non-soft) dependents, an error will be displayed and no further action taken,
unless the \fB\-\-force\fR option is used. If the service is pinned started (and not already stopped or
stopping) an error will be displayed and no further action taken.
.sp
The \fBrestart\fR option (see \fBdinit-service\fR(5)) applied to the stopped service will not cause the
service to restart when it is stopped via this command (that is, this command inhibits automatic restart).
This also applies to any dependents that must also be stopped at the same time.
.sp
Any pending \fBstart\fR orders (including for restarts) are cancelled,
though a service which is starting will continue its startup before then stopping (unless the service is
configured to have an interruptible startup or is otherwise at a stage of startup which can be safely
interrupted).
.TP
\fBstatus\fR
Give a status report on the specified service.
This will show the current state (and target state, if different), and information such as process
ID (pid) if applicable.
If the service is stopped for any reason other than a normal stop, the reason for the service
stopping will be displayed (along with any further relevant information, if available).
.TP
\fBis-active\fR
Check if the specified service is currently active.
The service counts as active if it is known it is currently started. Any other state, including
protocol and parse errors, will exit without returning success. Unless quiet, the current service
status (STOPPED, STARTING, STARTED, STOPPING) is printed out to standard output.
.TP
\fBis-failed\fR
Check if the specified service is currently failed.
The service counts as failed if it is known it is currently stopped either because of startup
failure, timeout or dependency failure. Any other state, including protocol and parse errors,
will exit without returning success. Unless quiet, the current srevice status is printed out
to standard output like with \fBis-active\fR.
.TP
\fBrestart\fR
Restart the specified service. The service will be stopped and then restarted, without affecting explicit
activation status or dependency links from dependents.
.TP
\fBwake\fR
Start the specified service after reattaching dependency links from all active dependents of the specified
service.
The service will not be marked explicitly activated, and so will stop if all the dependents stop.
.TP
\fBrelease\fR
Clear the explicit activation mark from a service (the service will then stop if it has no active dependents).
.TP
\fBunpin\fR
Remove start- and stop- pins from a service.
If a started service is not explicitly activated and has no active dependents, it will stop.
If a started service has a dependency service which is stopping, it will stop.
If a stopped service has a dependent service which is starting, it will start.
Otherwise, any pending start/stop commands will be carried out.
.TP
\fBunload\fR
Completely unload a service.
This can only be done if the service is stopped and has no loaded dependents (i.e. dependents must
be unloaded before their dependencies).
.TP
\fBreload\fR
Attempt to reload a service description.
This is intended as a convenience for making simple changes to a service, without having to stop,
remove dependencies to and unload the service. However it is not completely equivalent to doing a
proper unload/reload; some altered settings may not take effect until the service is restarted,
and some cannot be changed at all while the service is running.
.sp
In particular, the type of a running service cannot be changed; nor can the \fBinittab-id\fR, \fBinittab-line\fR,
or \fBpid-file\fR settings, or the \fBruns-on-console\fR or \fBshares-console\fR flags.
If any hard dependencies are added to a running service, the dependencies must already be started.
.TP
\fBlist\fR
List loaded services and their state.
Before each service, one of the following state indicators is displayed:
.sp
.RS
.nf
\f[C]\m[blue][{+}\ \ \ \ \ ]\m[]\fR \[em] service has started.
\f[C]\m[blue][{\ }<<\ \ \ ]\m[]\fR \[em] service is starting.
\f[C]\m[blue][\ \ \ <<{\ }]\m[]\fR \[em] service is starting, will stop once started.
\f[C]\m[blue][{\ }>>\ \ \ ]\m[]\fR \[em] service is stopping, will start once stopped.
\f[C]\m[blue][\ \ \ >>{\ }]\m[]\fR \[em] service is stopping.
\f[C]\m[blue][\ \ \ \ \ {-}]\m[]\fR \[em] service has stopped.
.fi
.sp
The << and >> symbols represent a transition state (starting and stopping respectively); curly braces
indicate the target state (left: started, right: stopped); square brackets are used if the service
is marked active (target state will always be started if this is the case).
.sp
An 's' in place of '+' means that the service startup was skipped (possible only if the service is
configured as skippable).
An 'X' in place of '-' means that the service failed to start, or that the
service process unexpectedly terminated with an error status or signal while running.
.sp
Additional information, if available, will be printed after the service name: whether the service owns,
or is waiting to acquire, the console; the process ID; the exit status or signal that caused termination.
.RE
.TP
\fBshutdown\fR
Stop all services (without restart) and terminate Dinit.
If issued to the system instance of Dinit, this will also shut down the system.
.TP
\fBadd-dep\fR
Add a dependency between two services.
The \fIdependency-type\fR must be one of \fBregular\fR, \fBmilestone\fR or \fBwaits-for\fR.
Note that adding a regular dependency requires that the service states are consistent with the
dependency (i.e. if the "from" service is started, the "to" service must also be started).
Circular dependency chains may not be created.
.TP
\fBrm-dep\fR
Remove a dependency between two services.
The \fIdependency-type\fR must be one of \fBregular\fR, \fBmilestone\fR or \fBwaits-for\fR.
If the "to" service is not otherwise active it may be stopped as a result of removing the dependency.
.TP
\fBenable\fR
Persistently enable a \fBwaits-for\fR dependency between two services.
This is much like \fBadd-dep\fR but it also starts the dependency if the dependent is started
(without explicit activation, so the dependency will stop if the dependent stops), and it creates
a symbolic link in the directory specified via the \fBwaits-for.d\fR directive in the service
description (there must be only one such directive) so that the dependency will survive between
sessions.
.sp
If the \fB--from\fR option is not used to specify the dependent, the dependency is created from the
\fBboot\fR service by default.
.TP
\fBdisable\fR
Permanently disable a \fBwaits-for\fR dependency between two services.
This is the complement of the \fBenable\fR command; see the description above for more information.
.sp
Note that the \fBdisable\fR command affects only the dependency specified (or implied).
It has no other effect, and a service that is "disabled" may still be started if it is a dependency of
another started service.
.TP
\fBtrigger\fR
Mark the specified service (which must be a \fItriggered\fR service) as having its external trigger set.
This will allow the service to finish starting. 
.TP
\fBuntrigger\fR
Clear the trigger for the specified service (which must be a \fItriggered\fR service).
This will delay the service from starting, until the trigger is set. If the service has already started,
this will have no immediate effect.
.TP
\fBsetenv\fR
Export one or more variables into the activation environment.
The value can be provided on the command line or retrieved from the environment \fBdinitctl\fR is
called in.
Any subsequently started or restarted service will have these environment variables available.
This is particularly useful for user services that need access to session information.
.\"
.SH SERVICE OPERATION
.\"
Normally, services are only started if they have been explicitly activated (\fBstart\fR command) or if
a started service depends on them.
Therefore, starting a service also starts all services that the first depends on; stopping the
same service then also stops the dependency services, unless they are also required by another
explicitly activated service or have been explicitly activated themselves.
.LP
A service can be pinned in either the started or stopped state.
This is mainly intended to be used to prevent automated stop or start of a service, including
via a dependency or dependent service, during a manual administrative procedure.
.LP
Stopping a service manually will prevent it (and its dependents) from automatically restarting (i.e. it
will override the \fBrestart\fR setting in the service configuration).
.\"
.SH ENVIRONMENT VARIABLES
.\"
The following environment variables may control \fBdinitctl\fR's operation: 
.TP
\fBDINIT_SOCKET_PATH\fR
The path to the socket used to communicate with the \fBdinit\fR(8) daemon. May be overridden by certain
command line options. If not set, and not overridden, the \fBdinit\fR defaults are used.
.\"
.SH SEE ALSO
\fBdinit\fR(8), \fBdinit-service\fR(5), \fB$$$SHUTDOWN_PREFIX@@@shutdown(8)\fR.
.\"
.SH AUTHOR
Dinit, and this manual, were written by Davin McCall.
$$$dnl
