changequote(`@@@',`$$$')dnl
@@@.TH SHUTDOWN "8" "$$$MONTH YEAR@@@" "Dinit $$$VERSION@@@" "Dinit \- service management system"
.SH NAME
$$$SHUTDOWN_PREFIX@@@shutdown, $$$SHUTDOWN_PREFIX@@@halt, $$$SHUTDOWN_PREFIX@@@poweroff, $$$SHUTDOWN_PREFIX@@@reboot, $$$SHUTDOWN_PREFIX@@@soft-reboot \- system shutdown
.\"
.SH SYNOPSIS
.\"
.B $$$SHUTDOWN_PREFIX@@@shutdown
[\fB\-r\fR|\fB\-h\fR|\fB\-p\fR] [\fB\-\-use\-passed\-cfd\fR]
[\fB\-\-system\fR]
.br
\fB$$$SHUTDOWN_PREFIX@@@halt\fR [\fIoptions...\fR]
.br
\fB$$$SHUTDOWN_PREFIX@@@poweroff\fR [\fIoptions...\fR]
.br
\fB$$$SHUTDOWN_PREFIX@@@reboot\fR [\fIoptions...\fR]
.br
\fB$$$SHUTDOWN_PREFIX@@@soft\-reboot\fR [\fIoptions...\fR]
.\"
.SH DESCRIPTION
.\"
This manual page is for the shutdown utility included with the \fBDinit\fR
service manager package. See \fBdinit\fR(8).
.LP
The \fB$$$SHUTDOWN_PREFIX@@@shutdown\fR, \fB$$$SHUTDOWN_PREFIX@@@reboot\fR,
\fB$$$SHUTDOWN_PREFIX@@@soft-reboot\fR, \fB$$$SHUTDOWN_PREFIX@@@poweroff\fR and \fB$$$SHUTDOWN_PREFIX@@@halt\fR
commands can be used to instruct the service manager daemon to perform a service rollback and then to shutdown the system.
They can also perform shutdown directly, without service rollback.
.LP
Note that for consistency with other packages "$$$SHUTDOWN_PREFIX@@@halt" and "$$$SHUTDOWN_PREFIX@@@poweroff" aliases
are provided, however they have no special significance. The default action
is to power down the system if called as either "$$$SHUTDOWN_PREFIX@@@shutdown", "$$$SHUTDOWN_PREFIX@@@halt", or
"$$$SHUTDOWN_PREFIX@@@poweroff".
.\"
.SH OPTIONS
.TP
\fB\-\-help\fR
Display brief help text and then exit.
.TP
\fB\-r\fP
Request a shutdown followed by restart. This is the default if executed as
\fB$$$SHUTDOWN_PREFIX@@@reboot\fR.
.TP
\fB\-k\fP
Shutdown the system and boot directly into a new kernel loaded via \fBkexec\fR(8),
without firmware reinitialisation. Don't forget to load a kernel image with
\fBkexec\fR(8) first!
.TP
\fB\-s\fP
Restart the service manager and all user-space services without restarting the system.
This is the default if executed as \fB$$$SHUTDOWN_PREFIX@@@soft\-reboot\fR.
.IP
The current boot-time options will be passed on to the restarted dinit and remain in effect
(note that system recovery options, for example, may re-initiate the recovery process).
Services must be suitably configured in order for this option to be useful; using this
option in environments where it is not specifically supported is not recommended.
.TP
\fB\-h\fP
Shutdown and then halt the system (without powering down).
.TP
\fB\-p\fP
Shutdown and then power down the system. This is the default unless executed
as \fB$$$SHUTDOWN_PREFIX@@@reboot\fR or \fB$$$SHUTDOWN_PREFIX@@@soft\-reboot\fR.
.TP
\fB\-\-use\-passed\-cfd\fR
Instead of attempting to open a socket connection to the service daemon,
use a pre-opened connection that has been passed to the process from its parent
via an open file descriptor.
The file descriptor with the connection is identified by the contents of the DINIT_CS_FD
environment variable.
.TP
\fB\-\-system\fR
Shut down directly, instead of by issuing a command to the service manager.
Use of this option should be avoided, but it may allow performing a clean shutdown in case
the service manager has stopped responding.
.IP
The service manager may invoke \fB$$$SHUTDOWN_PREFIX@@@shutdown\fR with this option in order to perform
system shutdown after it has rolled back services.
.\"
.SH SHUTDOWN HOOKS
.\"
To allow for special shutdown actions, if an executable file exists at any of the following
locations, it will be executed before the system is shut down but after terminating all other
processes:
.\"
.RS
.IP \(bu
/etc/dinit/shutdown-hook
.IP \(bu
/lib/dinit/shutdown-hook
.RE
.LP
Only the first existing executable file from the above list will be executed. The first location
is intended to allow customisation by the system administrator (and should usually be a script
which on completion executes the 2nd shutdown hook, if present).
The 2nd location is intended for distribution control. 
.LP
If found and successfully executed, the shutdown hook should perform any special shutdown actions
that depend on all processes being terminated.
If the shutdown hook cleanly unmounts (or remounts as read-only) all file systems including the
root file system, it should exit with status 0 (success), which will prevent \fB$$$SHUTDOWN_PREFIX@@@shutdown\fR from
attempting to unmount file systems itself.
If it does not unmount file systems, the script should not exit with status 0. 
.\"
.SH SEE ALSO
.\"
\fBdinit\fR(8), \fBdinitctl\fR(8)
.\"
.SH AUTHOR
Dinit, and this manual, were written by Davin McCall.
$$$dnl
