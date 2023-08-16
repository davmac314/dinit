changequote(`@@@',`$$$')dnl
@@@.TH DINIT "8" "$$$MONTH YEAR@@@" "Dinit $$$VERSION@@@" "Dinit \- service management system"
.SH NAME
dinit \- supervise processes and manage services
.\"
.SH SYNOPSIS
.\"
.nh
.\"
.HP
.B dinit
[OPTION]... [\fIservice-name\fR]...
.\"
.hy
.\"
.SH DESCRIPTION
.\"
\fBDinit\fR is a process supervisor and service manager which can also
function as a system \fBinit\fR process.
It has a small but functional feature set, offering service dependency handling, parallel startup,
automatic rate-limited restart of failing processes, and service control functions.
.LP
Dinit can be run as a system instance (when run as the root user or when
specified via command line parameter) or as a user instance.
This affects the default paths used to locate certain files.
.LP
When run as PID 1, the first process, Dinit by default acts as a system manager and
shuts down or reboots the system on request (including on receipt of certain signals).
This is currently fully supported only on Linux.
See \fBRUNNING AS SYSTEM MANAGER / PRIMARY INIT\fR.
.LP
Dinit reads service descriptions from files located in a service
description directory, normally one of \fI/etc/dinit.d\fR, \fI/run/dinit.d\fR,
\fI/usr/local/lib/dinit.d\fR or \fI/lib/dinit.d\fR for the system instance
or just \fI$HOME/.config/dinit.d\fR when run as a user process.
See \fBSERVICE DESCRIPTION FILES\fR for details of the service description format.
.\"
.SH OPTIONS
.TP
\fB\-d\fR \fIdir\fP, \fB\-\-services\-dir\fR \fIdir\fP
Specifies \fIdir\fP as the directory containing service definition files.
This can be specified multiple times for multiple service directories.
The default directories are not searched for services when this option is provided.
.sp
If not specified, the default is \fI$HOME/.config/dinit.d\fR or, for the
system service manager, each of \fI/etc/dinit.d\fR, \fI/run/dinit.d/\fR,
\fI/usr/local/lib/dinit.d\fR, and \fI/lib/dinit.d\fR (searched in that order).
.TP
\fB\-e\fR \fIfile\fP, \fB\-\-env\-file\fR \fIfile\fP
Read initial environment from \fIfile\fP.
For the system init process, the default is \fI/etc/dinit/environment\fR; see \fBFILES\fR.
.TP
\fB\-p\fR \fIpath\fP, \fB\-\-socket\-path\fR \fIpath\fP
Specifies \fIpath\fP as the path to the control socket used to listen for
commands from the \fBdinitctl\fR program.
The default for the system service manager is usually \fI/dev/dinitctl\fR (but can be configured at build time).
For a user service manager the default is either \fI$XDG_RUNTIME_DIR/dinitctl\fR
or \fI$HOME/.dinitctl\fR, depending on whether \fI$XDG_RUNTIME_DIR\fR is set.
.TP
\fB\-F\fR \fIfd\fP, \fB\-\-ready\-fd\fR \fIfd\fP
Specifies \fIfd\fP as the file descriptor number to report readiness to.
Readiness means that the control socket is open and the service manager is
ready to accept commands (e.g. via \fBdinitctl\fR). It does not mean that
services are finished starting yet. The path to the currently open control
socket is written on the file descriptor.
.TP
\fB\-l\fR \fIpath\fP, \fB\-\-log\-file\fR \fIpath\fP
Species \fIpath\fP as the path to the log file, to which Dinit will log status
and error messages.
Note that when running as the system service manager, Dinit
does not begin logging until the log service has started.
Using this option inhibits logging via the syslog facility, however, all logging messages are
duplicated as usual to the console (so long as no service owns the console).
.TP
\fB\-s\fR, \fB\-\-system\fR
Run as the system service manager.
This is the default if invoked as the root user.
This option affects the default service definition directory and control socket path.
.TP
\fB\-m\fR, \fB\-\-system\-mgr\fR
Run as the system manager (perform operations directly related to machine startup
and shutdown).
This is the default when running as process ID 1.
The main user-visible effect of this option is to invoke the \fB$$$SHUTDOWN_PREFIX@@@shutdown\fR program when a shutdown is
requested (and after all services have stopped), and to provide some basic support
for system recovery in case the \fBboot\fR service (or other specified service)
cannot be started.
.TP
\fB\-u\fR, \fB\-\-user\fR
Run as a user service manager.
This is the opposite of \fB\-\-system\fR, and is the default if not invoked as the root user.
.TP
\fB\-o\fR, \fB\-\-container\fR
Run in "container mode", i.e. do not perform system management functions (such
as shutdown/reboot).
The \fBdinit\fR daemon will simply exit rather than executing the \fB$$$SHUTDOWN_PREFIX@@@shutdown\fR program.
.TP
\fB\-q\fR, \fB\-\-quiet\fR
Run with no output to the terminal/console.
This disables service status messages and sets the log level for the console log to \fBNONE\fR.
.TP
\fB\-b\fR \fIpath\fR, \fB\-\-cgroup\-path\fR \fIpath\fR
Specify the path to resolve relative cgroup paths against.
If service description settings contain relative cgroup paths, they will be resolved relative to
this path.
This option is only available if \fBdinit\fR is built with cgroups support.
.TP
\fB\-\-help\fR
Display brief help text and then exit.
.TP
\fB\-\-version\fR
Display version number and then exit.
.TP
[\fB\-t\fR] \fIservice-name\fR, [\fB\-\-service\fR] \fIservice-name\fR
Specifies the name of a service that should be started (along with its
dependencies).
If none are specified, defaults to \fIboot\fR (which requires that a suitable service description
for the \fIboot\fR service exists). Multiple services can be specified in which case they will each
be started.
.sp
\fBNote:\fR on Linux, if \fBdinit\fR is running as PID 1 and with UID 0, it may ignore "naked"
service names (without preceding \fB\-\-service\fR/\fB\-t\fR) provided on the command line.
See the \fBCOMMAND LINE FROM KERNEL\fR section.
.\"
.SH SERVICE DESCRIPTION FILES
.\"
Service description files specify the parameters of each service.
They are named for the service they describe, and are found in one of several directories
(including \fI/etc/dinit.d\fR) for a system instance or \fI$HOME/.config/dinit.d\fR for a user instance
(see also \fB\-\-services\-dir\fR option).
.LP
Service description files are read by Dinit on an "as needed" basis.
Once loaded, a service description is never automatically unloaded (even if the service
stops or fails).
A service description can however be unloaded (if the service is stopped) or reloaded
(with some limitations) via \fBdinitctl\fR(8) using the \fBunload\fR and \fBreload\fR subcommands
respectively.
.LP
See \fBdinit-service\fR(5) for details of the format and available parameters.
.\"
.SH SPECIAL SERVICE NAMES
.\"
There are two service names that are "special" to Dinit.
.LP
The \fIboot\fR service is the service that Dinit starts by default, if no
other service names are provided when it is started.
.LP
The \fIrecovery\fR service is a service that Dinit will offer to start if
boot appears to fail (that is, if all services stop without a shutdown command
having been issued), when Dinit is running as system manager.
.\"
.SH OPERATION
.\"
On starting, Dinit starts the initial service(s) as specified on the command line.
Starting a service also causes the dependencies of that service to start, and any service
processes will not be launched until the dependencies are satisfied.
Similarly, stopping a service first stops any dependent services.
.LP
During execution, Dinit accepts commands via a control socket which is created
by Dinit when it starts.
This can be used to order that a service be started or stopped, to determine service status, or to
make certain configuration changes.
See \fBdinitctl\fR(8) for details.
Dinit attempts to check for the existence of an already-active socket first, and will refuse to
start if one exists.
Unfortunately, this check cannot be done atomically, and should not be relied upon generally as a
means to avoid starting two instances of dinit.
.LP
Process-based services are monitored and, if the process terminates, the service may be stopped or
the process may be re-started, according to the configuration in the service description.
.LP
Once all services stop, the \fBdinit\fR daemon will itself terminate (or, if
running as system manager, will perform the appropriate type of system shutdown).
.\"
.SS CHARACTER SET HANDLING
.\"
Dinit does no character set translation.
Dinit's own output is in the execution character set as determined at compilation, as is the interpretation of input.
Service names (and other user-defined inputs) are interpreted as byte sequences and are output as they were read.
In general, modern systems use the UTF-8 character set universally and no problems will arise;
however, systems configured to use other character sets may see odd behaviour if the input
character set does not match the output character set, or if either input or output character sets
are not a superset of the execution character set.
.\"
.SS RUNNING AS SYSTEM MANAGER / PRIMARY INIT
.\"
Running as the system manager (primary \fBinit\fR) is currently supported only on
Linux.
When run as process ID 1, the \fBdinit\fR daemon by default assumes responsibility for
system shutdown and restart (partially relying on external utilities which are
part of the Dinit distribution).
.LP
When not running as a system manager, \fBdinit\fR assumes responsibility only for
service management.
System shutdown or restart need to be handled by the primary \fBinit\fR, which should start
\fBdinit\fR on normal startup, and terminate \fBdinit\fR before shutdown, by signalling it and
waiting for it to terminate after stopping services (possibly by invoking \fBdinitctl shutdown\fR).
.\"
.SH COMMAND LINE FROM KERNEL
.LP
When running as PID 1, \fBdinit\fR may process the command line differently, to compensate for kernel behaviour.
.LP
On Linux, kernel command line options that are not recognised by the kernel will be passed on to \fBdinit\fR.
However, bugs in some kernel versions may cause some options that are recognised to also be passed to \fBdinit\fR.
Also, boot managers may insert command-line options such as "\fBauto\fR" (which indicates an "unattended" boot).
Therefore, \fBdinit\fR ignores all "word like" options other than "\fBsingle\fR", which it treats as
the name of the service to start (thus allowing "single user mode", assuming that a suitable service description exists).
Options beginning with "\fB--\fR" will not be recognised by the kernel and will be passed to (and processed by) \fBdinit\fR;
for example \fB\-\-quiet\fR can be used to suppress console output. Options containing "=" that are unrecognised by the
kernel (or some that are, due to bugs) are passed to init via the environment rather than via the command line.
.LP
There are several ways to work around this.
Service names following the \fB\-\-container\fR (\fB\-o\fR) or \fB\-\-system\-mgr\fR (\fB\-m\fR) options are not ignored.
Also, the \fB\-\-service\fR (\fB\-t\fR) option can be used to force a service name to be recognised regardless of operating mode.
.\"
.SH FILES
.\"
.TP
\fI/etc/dinit/environment\fR
Default location of the environment file for Dinit when run as a system
instance (for user instances there is no default).
Values are specified as \fINAME\fR=\fIVALUE\fR, one per line, and add to and replace variables present
in the environment when Dinit started (the "original environment").
Lines beginning with a hash character (#) are ignored.
.IP
The following special commands can be used (each on a single line):
.RS
.TP
\fB!clear\fR
Clears the environment completely (prevents inheritance of any variables from the original environment).
.TP
\fB!unset\fR \fIvar-name\fR...
Unsets the specified variables.
Any previously specified value for these variables is forgotten, and they will not inherit any
value from the original environment. 
.TP
\fB!import\fR \fIvar-name\fR...
Imports the value of the named variables from the original environment, overriding the effect of any
value set previously as well as the effect of previous \fB!unset\fR and \fB!clear\fR commands.
.RE
.TP
\fI/etc/dinit.d\fR, \fI/run/dinit.d\fR, \fI/usr/local/lib/dinit.d\fR, \fI/lib/dinit.d\fR
Default locations for service description files. The directories are searched in the order listed.
.TP
\fI$HOME/.config/dinit.d\fR
Default location for service description files for user instances. 
.\"
.SH SIGNALS
.LP
When run as a system manager, SIGINT stops all services and performs a reboot (on Linux, this signal can be
generated using the control-alt-delete key combination); SIGTERM stops services and halts the system; and
SIGQUIT performs an immediate shutdown with no service rollback.
.LP
When run as a user process or system service manager only, SIGINT and SIGTERM both stop services
and exit Dinit; SIGQUIT exits Dinit immediately.
.\"
.SH SEE ALSO
.\"
\fBdinitctl\fR(8), \fBdinit-service\fR(5), \fBdinitcheck\fR(8), \fB$$$SHUTDOWN_PREFIX@@@shutdown(8)\fR.
.\"
.SH AUTHOR
Dinit, and this manual, were written by Davin McCall.
$$$dnl
