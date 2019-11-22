changequote(`@@@',`$$$')dnl
@@@.TH DINITCHECK "8" "$$$MONTH YEAR@@@" "Dinit $$$VERSION@@@" "Dinit \- service management system"
.SH NAME
dinitcheck \- check service configuration
.\"
.SH SYNOPSIS
.\"
.HP \w'\ 'u
.B dinitcheck
[\fB\-d\fR|\fB\-\-services\-dir\fR \fIdir\fR]
[\fIservice-name\fR...]
.\"
.SH DESCRIPTION
.\"
The \fBdinitcheck\fR utility checks the service configuration for
\fBDinit\fR services (see \fBdinit\fR(8)), and reports any errors it finds.
This allows for finding errors before they can cause a service to fail to
load during system operation.
.LP
Errors reported by \fBdinitcheck\fR include:
.IP \(bu
Syntax errors
.IP \(bu
Invalid parameter values
.IP \(bu
Service dependency cycles
.LP
Unless altered by options specified on the command line, this utility uses the
same search paths (for service description files) as \fBdinit\fR.
.\"
.SH OPTIONSs
.TP
\fB\-d\fR \fIdir\fP, \fB\-\-services\-dir\fR \fIdir\fP
Specifies \fIdir\fP as the directory containing service definition files.
The directory specified will be the only directory searched for service
definitions.

If not specified, the default is \fI$HOME/dinit.d\fR or, for the
system service manager, each of \fI/etc/dinit.d/fR, \fI/usr/local/lib/dinit.d\fR,
and \fI/lib/dinit.d\fR (searched in that order).
.TP
\fB\-\-help\fR
Display brief help text and then exit.
.TP
\fIservice-name\fR
Specifies the name of a service that should be checked (along with its
dependencies). If none are specified, defaults to \fIboot\fR (which requires
that a suitable service description for the \fIboot\fR service exists).
.\"
.SH SEE ALSO
.\"
\fBdinit\fR(8), \fBdinit-service\fR(5).
.\"
.SH AUTHOR
Dinit, and this manual, were written by Davin McCall.
$$$dnl
