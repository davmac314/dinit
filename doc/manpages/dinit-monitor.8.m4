changequote(`@@@',`$$$')dnl
@@@.TH DINIT\-MONITOR "8" "$$$MONTH YEAR@@@" "Dinit $$$VERSION@@@" "Dinit \- service management system"
.SH NAME
dinit\-monitor \- monitor services supervised by Dinit
.\"
.SH SYNOPSIS
.\"
.PD 0
.nh
.HP
.B dinit-monitor
[\fIoptions\fR] {\fB\-c\fR \fIcommand\fR, \fB\-\-command\fR \fIcommand\fR} \fIservice-name\fR [\fIservice-name\fR...]
.\"
.PD
.hy
.\"
.SH DESCRIPTION
.\"
\fBdinit\-monitor\fR is a utility to monitor the state of one or more services managed by the \fBdinit\fR daemon.
Changes in service state are reported by the execution of the specified command.
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
When not specified, the \fIDINIT_SOCKET_PATH\fR environment variable is read, otherwise
Dinit's default values are used.
.\"
.SH STATUS REPORT OPTIONS
.TP
\fB\-\-command\fR \fIcommand\fR, \fB\-c\fR \fIcommand\fR
Execute the specified \fIcommand\fR when the service status changes. In \fIcommand\fR, \fB%n\fR
will be substituted with the service name and \fB%s\fR will be substituted with a textual
description of the new status (\fBstarted\fR, \fBstopped\fR or \fBfailed\fR). A double percent sign
(\fB%%\fR) is substituted with a single percent sign character.
.\"
.SH OPERATION
.\"
The \fBdinit-monitor\fR program will wait until a monitored service changes status, and execute
the notification command. When execution of the notification command completes, dinit-monitor will
resume monitoring of service status.
.LP
The monitoring provided by \fBdinit-monitor\fR is not intended to be used for system-critical
purposes. If system load is particularly high, for example, notifications may be skipped.
.\"
.SH SEE ALSO
\fBdinit\fR(8), \fBdinitctl\fR(8).
.\"
.SH AUTHOR
Dinit, and this manual, were written by Davin McCall.
$$$dnl
