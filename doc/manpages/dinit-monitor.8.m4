changequote(`@@@',`$$$')dnl
@@@.TH DINIT\-MONITOR "8" "$$$MONTH YEAR@@@" "Dinit $$$VERSION@@@" "Dinit \- service management system"
.SH NAME
dinit\-monitor \- monitor services or environment supervised by Dinit
.\"
.SH SYNOPSIS
.\"
.PD 0
.nh
.HP
.B dinit-monitor
[\fIoptions\fR] {\fB\-c\fR \fIcommand\fR, \fB\-\-command\fR \fIcommand\fR} \fIservice-or-env\fR [\fIservice-or-env\fR...]
.\"
.PD
.hy
.\"
.SH DESCRIPTION
.\"
\fBdinit\-monitor\fR is a utility to monitor the state of one or more services or environment managed by the \fBdinit\fR daemon.
Changes in service or environment state are reported by the execution of the specified command.
When monitoring environment, positional arguments are not required (all environment will be monitored).
By default, service events are monitored.
.\"
.SH GENERAL OPTIONS
.TP
\fB\-\-help\fR
Display brief help text and then exit.
.TP
\fB\-\-version\fR
Display version and then exit.
.TP
\fB\-e\fR, \fB\-\-exit\fR
Exit after the first command is executed, instead of waiting for more events.
.TP
\fB\-E\fR, \fB\-\-env\fR
Instead of monitoring the services, monitor changes in the global environment.
If no environment variables are passed, all environment is monitored.
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
\fB\-i\fR, \fB\-\-initial\fR
Issue the specified command additionally for the initial status of the services or environment (when \fBdinit\-monitor\fR is started).
Without this option, the command is only executed whenever status changes.
.TP
\fB\-\-str\-started\fR \fIstarted-text\fR
Specify the text used for the substitution of the status in the command (as specified
by the \fB\-\-command\fR option) when a service starts.
.TP
\fB\-\-str\-stopped\fR \fIstopped-text\fR
Specify the text used for the substitution of the status in the command (as specified
by the \fB\-\-command\fR option) when a service stops.
.TP
\fB\-\-str\-failed\fR \fIfailed-text\fR
Specify the text used for the substitution of the status in the command (as specified
by the \fB\-\-command\fR option) when a service fails to start.
.TP
\fB\-\-str\-set\fR \fIset-text\fR
Specify the text used for the substitution of the status in the command (as specified
by the \fB\-\-command\fR option) when an environment variable is set.
.TP
\fB\-\-str\-unset\fR \fIunset-text\fR
Specify the text used for the substitution of the status in the command (as specified
by the \fB\-\-command\fR option) when an environment variable is unset.
.TP
\fB\-\-socket\-path\fR \fIsocket-path\fR, \fB\-p\fR \fIsocket-path\fR
Specify the path to the socket used for communicating with the service manager daemon.
When not specified, the \fIDINIT_SOCKET_PATH\fR environment variable is read, otherwise
Dinit's default values are used.
.\"
.SH STATUS REPORT OPTIONS
.TP
\fB\-\-command\fR \fIcommand\fR, \fB\-c\fR \fIcommand\fR
Execute the specified \fIcommand\fR when the service status changes.
In \fIcommand\fR, \fB%n\fR will be substituted with the service or environment variable name,
\fB%v\fR will be substituted with the environment variable value, and \fB%s\fR will be substituted
with a textual description of the new status (\fBstarted\fR, \fBstopped\fR or \fBfailed\fR for
services, \fBset\fR or \fBunset\fR for environment variables). A double percent sign
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
