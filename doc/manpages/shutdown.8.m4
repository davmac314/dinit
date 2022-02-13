changequote(`@@@',`$$$')dnl
@@@.TH SHUTDOWN "8" "$$$MONTH YEAR@@@" "Dinit $$$VERSION@@@" "Dinit \- service management system"
.SH NAME
shutdown, halt, poweroff, reboot \- system shutdown 
.\"
.SH SYNOPSIS
.\"
.B shutdown
[\fB\-r\fR|\fB\-h\fR|\fB\-p\fR] [\fB\-\-use\-passed\-cfd\fR]
[\fB\-\-system\fR]
.br
\fBhalt\fR [\fIoptions...\fR]
.br
\fBpoweroff\fR [\fIoptions...\fR]
.br
\fBreboot\fR [\fIoptions...\fR]
.\"
.SH DESCRIPTION
.\"
This manual page is for the shutdown utility included with the \fBDinit\fR
service manager package. See \fBdinit\fR(8).

The shutdown, reboot, poweroff and halt commands can be used to instruct the
service manager daemon to perform a service rollback and then to shutdown the
system. They can also perform shutdown directly, without service rollback.

Note that for consistency with other packages "halt" and "poweroff" aliases
are provided, however they have no special significance. The default action
is to power down the system if called as either "shutdown", "halt", or
"poweroff".
.\"
.SH OPTIONS
.TP
\fB\-\-help\fR
Display brief help text and then exit.
.TP
\fB\-r\fP
Request a shutdown followed by restart. This is the default if executed as
\fBreboot\fR.
.TP
\fB\-h\fP
Shutdown and then halt the system (without powering down).
.TP
\fB\-p\fP
Shutdown and then power down the system. This is the default unless executed
as \fBreboot\fR.
.TP
\fB\-\-use\-passed\-cfd\fR
Instead of attempting to open a socket connection to the service daemon,
use a pre-opened connection that has been passed to the process from its parent
via an open file descriptor. The file descriptor with the connection is identified
by the DINIT_CS_FD environment variable.
.TP
\fB\-\-system\fR
Shut down directly, instead of by issuing a command to the service manager. Use of
this option should be avoided, but it may allow performing a clean shutdown in case
the service manager has stopped responding.

The service manager may invoke \fBshutdown\fR with this option in order to perform
system shutdown after it has rolled back services.
.\"
.SH SEE ALSO
.\"
\fBdinit\fR(8), \fBdinitctl\fR(8)
.\"
.SH AUTHOR
Dinit, and this manual, were written by Davin McCall.
$$$dnl
