changequote(`@@@',`$$$')dnl
@@@.TH DINITCHECK "8" "$$$MONTH YEAR@@@" "Dinit $$$VERSION@@@" "Dinit \- service management system"
.SH NAME
dinitcheck \- check service configuration
.\"
.SH SYNOPSIS
.\"
.nh
.\"
.HP
.B dinitcheck
[\fB\-d\fR|\fB\-\-services\-dir\fR \fIdir\fR]
[\fIservice-name\fR...]
.\"
.hy
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
.SH OPTIONS
.TP
\fB\-d\fR \fIdir\fP, \fB\-\-services\-dir\fR \fIdir\fP
Specifies \fIdir\fP as the directory containing service description files (can
be given multiple times to specify multiple service directories).
Default directories are not searched for services when this option is provided.
.sp
If not specified, the default is \fI$HOME/.config/dinit.d\fR or, for the
system service manager, each of \fI/etc/dinit.d/fR, \fI/run/dinit.d\fR,
\fI/usr/local/lib/dinit.d\fR, and \fI/lib/dinit.d\fR (searched in that order).
.TP
\fB\-\-help\fR
Display brief help text and then exit.
.TP
\fIservice-name\fR
Specifies the name of a service that should be checked (along with its
dependencies).
If none are specified, defaults to \fIboot\fR (which requires that a suitable
service description for the \fIboot\fR service exists).
.\"
.SH NOTES
.\"
For service properties that are subject to environment variable substitution, including
\fBsocket\-listen\fR, \fBlogfile\fR, \fBenv\-file\fR, \fBworking\-dir\fR and \fBpid\-file\fR, the
substitution may have a different result when performed by \fBdinitcheck\fR than when performed by
\fBdinit\fR if the two processes have a different environment.
For this reason \fBdinitcheck\fR will issue a warning whenever substitution is used.
.\"
.SH SEE ALSO
.\"
\fBdinit\fR(8), \fBdinit-service\fR(5).
.\"
.SH AUTHOR
Dinit, and this manual, were written by Davin McCall.
$$$dnl
