changequote(`@@@',`$$$')dnl
@@@.TH DINIT "8" "$$$MONTH YEAR@@@" "Dinit $$$VERSION@@@" "Dinit \- service management system"
.SH NAME
dinit \- supervise processes and manage services
.\"
.SH SYNOPSIS
.\"
.HP \w'\ 'u
.B dinit
[\fB\-s\fR|\fB\-\-system\fR|\fB\-u\fR|\fB\-\-user\fR] [\fB\-d\fR|\fB\-\-services\-dir\fR \fIdir\fR]
[\fB\-p\fR|\fB\-\-socket\-path\fR \fIpath\fR] [\fB\-e\fR|\fB\-\-env\-file\fR \fIpath\fR]
[\fB\-l\fR|\fB\-\-log\-file\fR \fIpath\fR]
[\fIservice-name\fR...]
.\"
.SH DESCRIPTION
.\"
\fBDinit\fR is a process supervisor and service manager which can also
function as a system \fBinit\fR process. It has a small but functional
feature set, offering service dependency handling, parallel startup,
automatic rate-limited restart of failing processes, and service control
functions.

Dinit can be run as a system instance (when run as the root user or when
specified via command line parameter) or as a user instance. This affects
the default paths used to locate certain files.

When run as PID 1, the first process, Dinit acts as a system manager and
shuts down or reboots the system on request (including on receipt of
certain signals). This is currently fully supported only on Linux. See
\fBRUNNING AS SYSTEM MANAGER / PRIMARY INIT\fR.

Dinit reads service descriptions from files located in a service
description directory, normally one of \fI/etc/dinit.d\fR,
\fI/usr/local/lib/dinit.d\fR or \fI/lib/dinit.d\fR for the system instance
or just \fI$HOME/dinit.d\fR when run as a user process. See \fBSERVICE
DESCRIPTION FILES\fR for details of the service description format.
.\"
.SH OPTIONS
.TP
\fB\-d\fR \fIdir\fP, \fB\-\-services\-dir\fR \fIdir\fP
Specifies \fIdir\fP as the directory containing service definition files.
The directory specified will be the only directory searched for service
definitions.

If not specified, the default is \fI$HOME/dinit.d\fR or, for the
system service manager, each of \fI/etc/dinit.d/fR, \fI/usr/local/lib/dinit.d\fR,
and \fI/lib/dinit.d\fR (searched in that order).
.TP
\fB\-e\fR \fIfile\fP, \fB\-\-env\-file\fR \fIfile\fP
Read initial environment from \fIfile\fP. For the system init process, the
default is \fI/etc/dinit/environment\fR; see \fBFILES\fR.
.TP
\fB\-p\fR \fIpath\fP, \fB\-\-socket\-path\fR \fIpath\fP
Specifies \fIpath\fP as the path to the control socket used to listen for
commands from the \fBdinitctl\fR program. The default for the system service
manager is usually \fI/dev/dinitctl\fR (but can be configured at build time).
For a user service manager the default is \fI$HOME/.dinitctl\fR.
.TP
\fB\-l\fR \fIpath\fP, \fB\-\-log\-file\fR \fIpath\fP
Species \fIpath\fP as the path to the log file, to which Dinit will log status
and error messages. Note that when running as the system service manager, Dinit
does not begin logging until the log service has started. Using this option
inhibits logging via the syslog facility, however, all logging messages are
duplicated as usual to the console (so long as no service owns the console).
.TP
\fB\-s\fR, \fB\-\-system\fR
Run as the system service manager. This is the default if invoked as the root
user. This option affects the default service definition directory and control
socket path.
.TP
\fB\-m\fR, \fB\-\-system\-mgr\fR
Run as the system manager (perform operations directly related to machine startup
and shutdown). This is the default when running as process ID 1. The main user-visible
effect of this option is to invoke the \fBshutdown\fR program when a shutdown is
requested (and after all services have stopped), and to provide some basic support
for system recovery in case the \fBboot\fR service (or other specified service)
cannot be started.
.TP
\fB\-u\fR, \fB\-\-user\fR
Run as a user. This is the opposite of \fB\-\-system\fR, and is the default if
not invoked as the root user.
.TP
\fB\-o\fR, \fB\-\-container\fR
Run in "container mode", i.e. do not perform system management functions (such
as shutdown/reboot). The \fBdinit\fR daemon will simply exit rather than executing
the \fBshutdown\fR program.
.TP
\fB\-q\fR, \fB\-\-quiet\fR
Run with no output to the terminal/console. This disables service status messages
and sets the log level for the console log to \fBNONE\fR.
.TP
\fB\-\-help\fR
Display brief help text and then exit.
\fB\-\-version\fR
Display version number and then exit.
.TP
\fIservice-name\fR
Specifies the name of a service that should be started (along with its
dependencies). If none are specified, defaults to \fIboot\fR (which requires
that a suitable service description for the \fIboot\fR service exists).

\fBNote:\fR on Linux, if \fBdinit\fR is running as PID 1 and with UID 0, it will ignore
service names provided on the command line, other than "single", unless they appear
after a "-o" or "-m" options (or their long forms). This is to filter arguments
that were intended for the kernel and not for \fBinit\fR. If running in a container,
the "-o" option should be used regardless and will inhibit this filtering for any
subsequent service names. 
.\"
.SH SERVICE DESCRIPTION FILES
.\"
Service description files specify the parameters of each service. They are
named for the service they describe, and are found in \fI/etc/dinit.d\fR
for a system instance or \fI$HOME/dinit.d\fR for a user instance.

Service description files are read by Dinit on an "as needed" basis. Once a
service description has been read the configuration can be altered in limited
ways via the \fBdinitctl\fR(8) program.

See \fBdinit-service\fR(5) for details of the format and available parameters.
.\"
.SH SPECIAL SERVICE NAMES
.\"
There are two service names that are "special" to Dinit.

The \fIboot\fR service is the service that Dinit starts by default, if no
other service names are provided when it is started.

The \fIrecovery\fR service is a service that Dinit will offer to start if
boot appears to fail (that is, if all services stop without a shutdown command
having been issued), when Dinit is running as system manager.
.\"
.SH OPERATION
.\"
On starting, Dinit starts the initial service(s) as specified on the command
line. Starting a service also causes the dependencies of that service to
start, and any service processes will not be launched until the dependencies
are satisfied. Similarly, stopping a service first stops any dependent
services.

During execution, Dinit accepts commands via a control socket which is created
by Dinit when it starts. This can be used to order that a service be started
or stopped, to determine service status, or to make certain configuration
changes. See \fBdinitctl\fR(8) for details.

Process-based services are monitored and, if the process terminates, the
service may be stopped or the process may be re-started, according to the
configuration in the service description.  

Once all services stop, the \fBdinit\fR daemon will itself terminate (or, if
running as PID 1, will perform the appropriate type of system shutdown).
.\"
.SS CHARACTER SET HANDLING
.\"
Dinit does no character set translation. Dinit's own output is in the execution
character set as determined at compilation, as is the interpretation of input.
Service names (and other user-defined inputs) are interpreted as byte sequences
and are output as they were read. In general, modern systems use the UTF-8
character set universally and no problems will arise; however, systems configured
to use other character sets may see odd behaviour if the input character set does
not match the output character set, or if either input or output character sets
are not a superset of the execution character set.
.\"
.SS RUNNING AS SYSTEM MANAGER / PRIMARY INIT
.\"
Running as the system manager (primary \fBinit\fR) is currently supported only on
Linux. When run as process ID 1, the \fBdinit\fR daemon assumes responsibility for
system shutdown and restart (partially relying on external utilities which are
part of the Dinit distribution).

When not running as process ID 1, \fBdinit\fR assumes responsibility only for
service management. System shutdown or restart need to be handled by the primary
\fBinit\fR, which should start \fBdinit\fR on normal startup, and terminate
\fBdinit\fR before shutdown, by signalling it and waiting for it to terminate
after stopping services (possibly by invoking \fBdinitctl shutdown\fR).
.\"
.SH FILES
.\"
.TP
\fI/etc/dinit/environment\fR
Default location of the environment file for Dinit when run as a system
instance (for user instances there is no default). Values are specified as
\fINAME\fR=\fIVALUE\fR, one per line, and add to and replace variables present
in the environment when Dinit started. Lines beginning with a hash character
(#) are ignored.
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
\fBdinitctl\fR(8), \fBdinit-service\fR(5), \fBdinitcheck\fR(8).
.\"
.SH AUTHOR
Dinit, and this manual, were written by Davin McCall.
$$$dnl
