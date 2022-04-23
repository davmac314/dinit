changequote(`@@@',`$$$')dnl
@@@.TH DINIT-SERVICE "5" "$$$MONTH YEAR@@@" "Dinit $$$VERSION@@@" "Dinit \- service management system"
.SH NAME
Dinit service description files
.\"
.SH SYNOPSIS
.\"
.ft CR
/etc/dinit.d/\fIservice-name\fR, $HOME/.config/dinit.d/\fIservice-name\fR
.ft
.\"
.SH DESCRIPTION
.\"
The service description files for \fBDinit\fR each describe a service. The name
of the file corresponds to the name of the service it describes. 
.LP
Service description files specify the various attributes of a service. A
service description file is named after the service it represents, and is
a plain-text file with simple key-value format.
The description files are located in a service description directory; by default,
the system process searches \fI/etc/dinit.d\fR, \fI/usr/local/lib/dinit.d\fR and
\fI/lib/dinit.d\fR, while a user process searches \fI$HOME/.config/dinit.d\fR.
.LP
All services have a \fItype\fR and a set of \fIdependencies\fR. These are discussed
in the following subsections. The type, dependencies, and other attributes are
specified via property settings, the format of which are documented in the
\fBSERVICE PROPERTIES\fR subsection, which also lists the available properties.
.\"
.SS SERVICE TYPES
.\"
There are four basic types of service:
.IP \(bu
\fBProcess\fR services. This kind of service runs as a single process; starting
the service simply requires starting the process; stopping the service is
accomplished by stopping the process (via sending it a signal).
.IP \(bu
\fBBgprocess\fR services ("background process" services). This kind of
service is similar to a regular process service, but the process daemonizes
or otherwise forks from the original process which starts it, and the
process ID is written to a file.
Dinit can read the process ID from the file and, if it is running as the system
init process, can supervise it.
.IP \(bu
\fBScripted\fR services are services which are started and stopped by a
command (which need not actually be a script, despite the name).
They can not be supervised.
.IP \(bu
\fBInternal\fR services do not run as an external process at all. They can
be started and stopped without any external action.
They are useful for grouping other services (via service dependencies).
.LP
Independent of their type, the state of services can be linked to other
services via dependency relationships, which are discussed in the next section.
.\"
.SS SERVICE DEPENDENCIES
.\"
A service dependency relationship, broadly speaking, specifies that for one
service to run, another must also be running.
The first service is the \fIdependent\fR service and the latter is the \fIdependency\fR
service (we will henceforth generally refer to the the dependency relationship as the
\fIrelationship\fR and use \fIdependency\fR to refer to the service).
A dependency relationship is specified via the properties of the dependent.
There are different relationship types, as follows:
.IP \(bu
A \fBneed\fR (or "hard") relationship specifies that the dependent must wait
for the dependency to be started before it starts, and that the dependency
must remain started while the dependent is started.
Starting the dependent will start the dependency, and stopping the dependency will stop the
dependent. This type of relationship is specified using a \fBdepends-on\fR property.
.IP \(bu
A \fBmilestone\fR relationship specifies that the dependency must
start successfully before the dependent starts.
Starting the dependent will therefore start the dependency.
Once started, the relationship is satisfied; if the dependency then stops, it
has no effect on the dependent.
However, if the dependency fails to start or has its startup cancelled, the dependent will
not start (and will return to the stopped state).
This type of relationship is specified using a \fBdepends-ms\fR property.
.IP \(bu
A \fBwaits-for\fR relationship specifies that the dependency must
start successfully, or fail to start, before the dependent starts.
Starting the dependent will attempt to first start the dependency, but failure will
not prevent the dependent from starting.
If the dependency starts, stopping it will have no effect on the dependent.
This type of relationship is specified using a \fBwaits-for\fR property.
.LP
Note that process-based services always wait for their dependency relationships
to be satisfied (by the dependency starting, or failing to start in case of a waits-for
relationship) before their process is launched.
Conversely, a termination signal will not in general be sent to a service process until
the service has no active dependents.
.LP
Since in general dependencies should remain started so long as their dependent
does, an attachment forms between the two once both are started.
This attachment is released when the dependent stops, and the dependency will then stop, unless
it has other attachments or it has been explicitly started independently.
Attachments between a dependent and dependency are re-created if a dependency
starts (or starts again) while the dependent is still started.
.\"
.SS SERVICE PROPERTIES
.\"
This section described the various service properties that can be specified
in a service description file. The properties specify the type of the service,
dependencies of the service, and other service configuration.
.LP
Each line of the file can specify a single
property value, expressed as `\fIproperty-name\fR = \fIvalue\fR'. Comments
begin with a hash mark (#) and extend to the end of the line (they must be
separated from setting values by at least one whitespace character).
Values are interpreted literally, except that:
.\"
.IP \(bu
White space (comprised of spaces, tabs, etc) is collapsed to a single space, except
leading or trailing white space around the property value, which is stripped.
.IP \(bu
Double quotes (") can be used around all or part of a property value, to
prevent whitespace collapse and prevent interpretation of other special
characters (such as "#") inside the quotes.
The quote characters are not considered part of the property value.
.IP \(bu
A backslash (\\) can be used to escape the next character, causing it to
lose any special meaning and become part of the property value.
A double backslash (\\\\) is collapsed to a single backslash within the parameter value.
.LP
Setting a property generally overrides any previous setting (from prior lines).
However some properties are set additively; these include dependency relationships and \fBoptions\fR
properties.
.LP
The following properties can be specified:
.TP
\fBtype\fR = {process | bgprocess | scripted | internal}
Specifies the service type; see the \fBSERVICE TYPES\fR section.
.TP
\fBcommand\fR = \fIcommand-string\fR
Specifies the command, including command-line arguments, for starting the process.
Applies only to \fBprocess\fR, \fBbgprocess\fR and \fBscripted\fR services.
.TP
\fBstop\-command\fR = \fIcommand-string\fR
Specifies the command to stop the service (optional). Applicable to \fBprocess\fR, \fBbgprocess\fR and
\fBscripted\fR services.  If specified for \fBprocess\fR or \fBbgprocess\fR services, the "stop
command" will be executed in order to stop the service, instead of signalling the service process. 
.TP
\fBworking\-dir\fR = \fIdirectory\fR
Specifies the working directory for this service. For a scripted service, this
affects both the start command and the stop command.
The value is subject to variable substitution (see \fBVARIABLE SUBSTITUTION\fR).
.TP
\fBrun\-as\fR = \fIuser-id\fR
Specifies which user to run the process(es) for this service as.
Specify as a username or numeric ID.
If specified by name, the group for the process will also be set to the primary
group of the specified user.
.TP
\fBenv\-file\fR = \fIfile\fR
Specifies a file containing value assignments for environment variables, in the same
format recognised by the \fBdinit\fR command's \fB\-\-env\-file\fR option.
The file is read (or re-read) whenever the service is started; the values read do not
affect for the processing performed for the \fBsub\-vars\fR load option, which is done
when the service description is loaded.
The precise behaviour of this setting may change in the future.
It is recommended to avoid depending on the specified file contents being reloaded
whenever the service process starts.
.sp
The path specified is subject to variable substitution (see \fBVARIABLE SUBSTITUTION\fR).
.TP
\fBrestart\fR = {yes | true | no | false}
Indicates whether the service should automatically restart if it stops, including due to
unexpected process termination or a dependency stopping.
Note that if a service stops due to user request, automatic restart is inhibited.
.TP
\fBsmooth\-recovery\fR = {yes | true | no | false}
Applies only to \fBprocess\fR and \fBbgprocess\fR services.
When set true/yes, an automatic process restart can be performed without first stopping any
dependent services.
This setting is meaningless if the \fBrestart\fR setting is set to false.
.TP
\fBrestart\-delay\fR = \fIXXX.YYYY\fR
Specifies the minimum time (in seconds) between automatic restarts. Enforcing a sensible
minimum prevents Dinit from consuming a large number of process cycles in case a process
continuously fails immediately after it is started.
The default is 0.2 (200 milliseconds).
.TP
\fBrestart\-limit\-interval\fR = \fIXXX.YYYY\fR
Sets the interval (in seconds) over which restarts are limited.
If a process automatically restarts more than a certain number of times (specified by the
\fBrestart-limit-count\fR setting) in this time interval, it will not be restarted again.
The default value is 10 seconds.
.TP
\fBrestart\-limit\-count\fR = \fINNN\fR
Specifies the maximum number of times that a service can automatically restart
over the interval specified by \fBrestart\-limit\-interval\fR.
Specify a value of 0 to disable the restart limit.
The default value is 3.
.TP
\fBstart\-timeout\fR = \fIXXX.YYY\fR
Specifies the time in seconds allowed for the service to start.
If the service takes longer than this, its process group is sent a SIGINT signal
and enters the "stopping" state (this may be subject to a stop timeout, as
specified via \fBstop\-timeout\fR, after which the process group will be
terminated via SIGKILL).
The timeout period begins only when all dependencies have been stopped.
The default timeout is 60 seconds.
Specify a value of 0 to allow unlimited start time.
.TP
\fBstop\-timeout\fR = \fIXXX.YYY\fR
Specifies the time in seconds allowed for the service to stop.
If the service takes longer than this, its process group is sent a SIGKILL signal
which should cause it to terminate immediately.
The timeout period begins only when all dependent services have already stopped.
The default timeout is 10 seconds.
Specify a value of 0 to allow unlimited stop time.
.TP
\fBpid\-file\fR = \fIpath-to-file\fR
For \fBbgprocess\fR type services only; specifies the path of the file where
daemon will write its process ID before detaching.
Dinit will read the contents of this file when starting the service, once the initial process
exits, and will supervise the process with the discovered process ID.
Dinit may also send signals to the process ID to stop the service; if \fBdinit\fR runs as a
privileged user the path should therefore not be writable by unprivileged users.
.sp
The value is subject to variable substitution (see \fBVARIABLE SUBSTITUTION\fR).
.TP
\fBdepends\-on\fR = \fIservice-name\fR
This service depends on the named service.
Starting this service will start the named service; the command to start this service will not be executed
until the named service has started.
If the named service is stopped then this service will also be stopped.
.TP
\fBdepends\-ms\fR = \fIservice-name\fR
This service has a "milestone" dependency on the named service. Starting this
service will start the named service; this service will not start until the
named service has started, and will fail to start if the named service does
not start.
Once the named (dependent) service reaches the started state, however, the
dependency may stop without affecting the dependent service.
.TP
\fBwaits\-for\fR = \fIservice-name\fR
When this service is started, wait for the named service to finish starting
(or to fail starting) before commencing the start procedure for this service.
Starting this service will automatically start the named service.
If the named service fails to start, this service will start as usual (subject to
other dependencies being met).
.TP
\fBwaits\-for.d\fR = \fIdirectory-path\fR
For each file name in \fIdirectory-path\fR which does not begin with a dot,
add a \fBwaits-for\fR dependency to the service with the same name.
Note that contents of files in the specified directory are not significant; expected
usage is to have symbolic links to the associated service description files,
but this is not required.
Failure to read the directory contents, or to find any of the services named within,
is not considered fatal.
.sp
The directory path, if not absolute, is relative to the directory containing the service
description file.
.TP
\fBchain\-to\fR = \fIservice-name\fR
When this service terminates (i.e. starts successfully, and then stops of its
own accord), the named service should be started.
Note that the named service is not loaded until that time; naming an invalid service will
not cause this service to fail to load.
.sp
This can be used for a service that supplies an interactive "recovery mode"
for another service; once the user exits the recovery shell, the primary
service (as named via this setting) will then start.
It also supports multi-stage system startup where later service description files reside on
a separate filesystem that is mounted during the first stage; such service
descriptions will not be found at initial start, and so cannot be started
directly, but can be chained via this directive.
.sp
The chain is not executed if the initial service was explicitly stopped,
stopped due to a dependency stopping (for any reason), if it will restart
(including due to a dependent restarting), or if its process terminates
abnormally or with an exit status indicating an error.
However, if the \fBalways-chain\fR option is set the chain is started regardless of the
reason and the status of this service termination.
.TP
\fBsocket\-listen\fR = \fIsocket-path\fR
Pre-open a socket for the service and pass it to the service using the
\fBsystemd\fR activation protocol.
This by itself does not give so called "socket activation", but does allow any
process trying to connect to the specified socket to do so immediately after
the service is started (even before the service process is properly prepared
to accept connections).
.sp
The path value is subject to variable substitution (see \fBVARIABLE SUBSTITUTION\fR).
.TP
\fBsocket\-permissions\fR = \fIoctal-permissions-mask\fR
Gives the permissions for the socket specified using \fBsocket\-listen\fR.
Normally this will be 600 (user access only), 660 (user and group
access), or 666 (all users).
The default is 666.
.TP
\fBsocket\-uid\fR = {\fInumeric-user-id\fR | \fIusername\fR}
Specifies the user (name or numeric ID) that should own the activation socket.
If \fBsocket\-uid\fR is specified as a name without also specifying \fBsocket-gid\fR, then
the socket group is the primary group of the specified user (as found in the
system user database, normally \fI/etc/passwd\fR).
If the \fBsocket\-uid\fR setting is not provided, the socket will be owned by the user id of the \fBdinit\fR process.
.TP
\fBsocket\-gid\fR = {\fInumeric-group-id\fR | \fIgroup-name\fR}
Specifies the group of the activation socket. See discussion of \fBsocket\-uid\fR.
.TP
\fBterm\-signal\fR = {none | HUP | INT | TERM | QUIT | USR1 | USR2 | KILL}
Specifies the signal to send to the process when requesting it
to terminate (applies to `process' and `bgprocess' services only).
The default is SIGTERM.
See also \fBstop\-timeout\fR.
.TP
\fBready\-notification\fR = {\fBpipefd:\fR\fIfd-number\fR | \fBpipevar:\fR\fIenv-var-name\fR}
Specifies the mechanism, if any, by which a process service will notify that it is ready
(successfully started).
If not specified, a process service is considered started as soon as it has begun execution.
The two options are:
.RS
.IP \(bu
\fBpipefd:\fR\fIfd-number\fR \(em the service will write a message to the specified file descriptor,
which \fBdinit\fR sets up as the write end of a pipe before execution.
This mechanism is compatible with the S6 supervision suite.
.IP \(bu
\fBpipevar:\fR\fIenv-var-name\fR \(em the service will write a message to file descriptor identified
using the contents of the specified environment variable, which will be set by \fBdinit\fR before
execution to a file descriptor (chosen arbitrarily) attached to the write end of a pipe.
.RE
.TP
\fBlogfile\fR = \fIlog-file-path\fR
Specifies the log file for the service.
Output from the service process (standard output and standard error streams) will be appended to this file.
This setting has no effect if the service is set to run on the console (via the \fBruns\-on\-console\fR,
\fBstarts\-on\-console\fR, or \fBshares\-console\fR options).
The value is subject to variable substitution (see \fBVARIABLE SUBSTITUTION\fR).
.TP
\fBoptions\fR = \fIoption\fR...
Specifies various options for this service. See the \fBOPTIONS\fR section.
This directive can be specified multiple times to set additional options.
.TP
\fBload\-options\fR = \fIload_option\fR...
Specifies options for interpreting other settings when loading this service description.
Currently there is only one available option, \fBsub\-vars\fR, which specifies that command-line arguments
(or parts thereof) in the form of \fB$NAME\fR should be replaced with the contents of the
environment variable with the specified name.
See \fBVARIABLE SUBSTITUTION\fR for details.
Note command-line variable substitution occurs after splitting the line into separate arguments and so
a single environment cannot be used to add multiple arguments to a command line.
If a designated variable is not defined, it is replaced with an empty (zero-length) string, possibly producing a
zero-length argument.
Environment variable variables are taken from the environment of the \fBdinit\fR process, and values
specified via \fBenv\-file\fR or \fBready\-notification\fR are not available.
This functionality is likely to be re-worked or removed in the future; use of this option should
be avoided if possible.
.TP
\fBinittab\-id\fR = \fIid-string\fR
When this service is started, if this setting (or the \fBinittab\-line\fR setting) has a
specified value, an entry will be created in the system "utmp" database which tracks
processes and logged-in users.
Typically this database is used by the "who" command to list logged-in users.
The entry will be cleared when the service terminates.
.sp
The \fBinittab\-id\fR setting specifies the "inittab id" to be written in the entry for
the process.
The value is normally quite meaningless.
However, it should be distinct (or unset) for separate processes.
It is typically limited to a very short length.
.sp
The "utmp" database is mostly a historical artifact.
Access to it on some systems is prone to denial-of-service by unprivileged users.
It is therefore recommended that this setting not be used.
However, "who" and similar utilities may not work correctly without this setting
(or \fBinittab\-line\fR) enabled appropriately.
.sp
This setting has no effect if Dinit was not built with support for writing to the "utmp"
database.
.TP
\fBinittab\-line\fR = \fItty-name-string\fR
This specifies the tty line that will be written to the "utmp" database when this service
is started.
Normally, for a terminal login service, it would match the terminal device name on which
the login process runs, without the "/dev/" prefix.
.sp
See the description of the \fBinittab\-id\fR setting for details.
.TP
\fBrlimit\-nofile\fR = \fIresource-limits\fR
Specifies the number of file descriptors that a process may have open simultaneously.
See the \fBRESOURCE LIMITS\fR section.
.TP
\fBrlimit\-core\fR = \fIresource-limits\fR
Specifies the maximum size of the core dump file that will be generated for the process if it
crashes (in a way that would result in a core dump).
See the \fBRESOURCE LIMITS\fR section.
.TP
\fBrlimit\-data\fR = \fIresource-limits\fR
Specifies the maximum size of the data segment for the process, including statically allocated
data and heap allocations.
Precise meaning may vary between operating systems.
See the \fBRESOURCE LIMITS\fR section.
.TP
\fBrlimit\-addrspace\fR = \fIresource-limits\fR
Specifies the maximum size of the address space of the process.
See the \fBRESOURCE LIMITS\fR section.
Note that some operating systems (notably, OpenBSD) do not support this limit; the
setting will be ignored on such systems.
.\"
.SS OPTIONS
.\"
These options are specified via the \fBoptions\fR parameter. 
.\"
.TP
\fBruns\-on\-console\fR
Specifies that this service uses the console; its input and output should be
directed to the console (or precisely, to the device to which Dinit's standard
output stream is connected).
A service running on the console prevents other services from running on the
console (they will queue for the console).
.sp
Proper operation of this option (and related options) assumes that \fBdinit\fR
is itself attached correctly to the console device (or a terminal, in which case
that terminal will be used as the "console").
.sp
The \fIinterrupt\fR key (normally control-C) will be active for process / scripted
services that run on the console.
Handling of an interrupt is determined by the service process, but typically will
cause it to terminate.
.TP
\fBstarts\-on\-console\fR
Specifies that this service uses the console during service startup.
This is identical to \fBruns\-on\-console\fR except that the console will be released
(available for running other services) once the service has started.
It is applicable only for \fBbgprocess\fR and \fBscripted\fR services.
.sp
As for the \fBruns\-on\-console\fR option, the \fIinterrupt\fR key will be enabled
while the service has the console.
.TP
\fBshares\-console\fR
Specifies that this service should be given access to the console (input and output
will be connected to the console), but that it should not exclusively hold the
console. A service given access to the console in this way will not delay the startup of services
which require exclusive access to the console (see \fBstarts\-on\-console\fR,
\fBruns\-on\-console\fR) nor will it be itself delayed if such services are already running.
.sp
This is mutually exclusive with both \fBstarts\-on\-console\fR and \fBruns\-on\-console\fR;
setting this option unsets both those options, and setting either of those options unsets
this option.
.TP
\fBstarts\-rwfs\fR
This service mounts the root filesystem read/write (or at least mounts the
normal writable filesystems for the system).
This prompts Dinit to create its control socket, if it has not already managed to do so.
.TP
\fBstarts\-log\fR
This service starts the system log daemon.
Dinit will begin logging via the \fI/dev/log\fR socket.
.TP
\fBpass\-cs\-fd\fR
Pass an open Dinit control socket to the process when launching it (the
\fIDINIT_CS_FD\fR environment variable will be set to the file descriptor of
the socket).
This allows the service to issue commands to Dinit even if the regular control socket is not available yet.
.sp
Using this option has security implications! The service which receives the
control socket must close it before launching any untrusted processes.
You should not use this option unless the service is designed to receive a Dinit
control socket.
.TP
\fBstart\-interruptible\fR
This service can have its startup interrupted (cancelled) if it becomes inactive
while still starting, by sending it the SIGINT signal.
This is meaningful only for \fBbgprocess\fR and \fBscripted\fR services.
.TP
\fBskippable\fR
For scripted services, indicates that if the service startup process terminates
via an interrupt signal (SIGINT), then the service should be considered started.
Note that if the interrupt was issued by Dinit to cancel startup, the service
will instead be considered stopped.
.sp
This can be combined with options such as \fBstarts\-on\-console\fR to allow
skipping certain non-essential services (such as filesystem checks) using the
\fIinterrupt\fR key (typically control-C).
.TP
\fBsignal\-process-only\fR
Signal the service process only, rather than its entire process group, whenever
sending it a signal for any reason.
.TP
\fBalways\-chain\fR
Alters behaviour of the \fBchain-to\fR property, forcing the chained service to
always start on termination of this service (instead of only when this service
terminates with an exit status indicating success).
.RE
.LP
The next section contains example service descriptions including some of the
parameters and options described above.
.\"
.SS RESOURCE LIMITS
.\"
There are several settings for specifying process resource limits: \fBrlimit\-nofile\fR,
\fBrlimit\-core\fR, \fBrlimit\-data\fR and \fBrlimit\-addrspace\fR.
See the descriptions of each above.
These settings place a limit on resource usage directly by the process.
Note that resource limits are inherited by subprocesses, but that usage of a resource
and subprocess are counted separately (in other words, a process can effectively bypass
its resource limits by spawning a subprocess and allocating further resources within it).
.sp
Resources have both a \fIhard\fR and \fIsoft\fR limit.
The soft limit is the effective limit, but note that a process can raise its soft limit up
to the hard limit for any given resource.
Therefore the soft limit acts more as a sanity-check; a process can exceed the soft limit
only by deliberately raising it first.
.sp
Resource limits are specified in the following format:
.sp
.RS
\fIsoft-limit\fR:\fIhard-limit\fR
.RE
.sp
Either the soft limit or the hard limit can be omitted (in which case it will be unchanged).
A limit can be specified as a dash, `\fB\-\fR', in which case the limit will be removed.
If only one value is specified with no colon separator, it affects both the soft and hard limit.
.\"
.SS VARIABLE SUBSTITUTION
.\"
Some service properties specify a path to a file or directory, or a command line.
For these properties, the specified value may contain one or more environment
variable names, each preceded by a single `\fB$\fR' character, as in `\fB$NAME\fR'.
In each case the value of the named environment variable will be substituted.
The name must begin with a non-punctuation, non-space, non-digit character, and ends
before the first control character, space, or punctuation character other than `\fB.\fR',
`\fB\-\fR' or `\fB_\fR'.
To avoid substitution, a single `\fB$\fR' can be escaped with a second, as in `\fB$$\fR'.
.sp
Variables for substitution come from the \fBdinit\fR environment at the time the service is loaded.
In particular, variables set via \fBenv\-file\fR are not visible to the substitution function.
.\"
.SH EXAMPLES
.LP
Here is an example service description for the \fBmysql\fR database server.
It has a dependency on the \fBrcboot\fR service (not shown) which is
expected to have set up the system to a level suitable for basic operation.
.sp
.RS
.nf
.gcolor blue
.ft CR
# mysqld service
type = process
command = /usr/bin/mysqld --user=mysql
logfile = /var/log/mysqld.log
smooth-recovery = true
restart = false
depends-on = rcboot # Basic system services must be ready
.ft
.gcolor
.RE
.fi
.LP
Here is an examples for a filesystem check "service", run by a script
(\fI/etc/dinit.d/rootfscheck.sh\fR).
The script may need to reboot the system, but the control socket may not have been
created, so it uses the \fBpass-cs-fd\fR option to allow the \fBreboot\fR command
to issue control commands to Dinit.
It runs on the console, so that output is visible and the process can be interrupted
using control-C, in which case the check is skipped but dependent services continue to start.
.sp
.RS
.nf
.gcolor blue
.ft CR
# rootfscheck service
type = scripted
command = /etc/dinit.d/rootfscheck.sh
restart = false
options = starts-on-console pass-cs-fd
options = start-interruptible skippable
depends-on = early-filesystems  # /proc and /dev
depends-on = device-node-daemon
.ft
.gcolor
.fi
.RE
.sp
More examples are provided with the Dinit distribution.
.\"
.SH AUTHOR
Dinit, and this manual, were written by Davin McCall.
$$$dnl
