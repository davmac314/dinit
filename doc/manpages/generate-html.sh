#!/bin/sh

mkdir -p html

cat > html/style.css <<- EOM
body    { margin-left: auto; margin-right: auto; width: 60em; padding-left: 1em; padding-right: 1em; padding-top: 0.5em; background: cornsilk; box-shadow: 0 0.8em 2em #000; }
body > h1:first-child { margin-top: 0; }
html    { background: darkslategray }  a { color: inherit; text-decoration: none }
section > p { margin-left: 3em; }
table.head, table.foot { width: 100%; }
td.head-rtitle, td.foot-os { text-align: right; }
td.head-vol { text-align: center; }
dd { margin-left: 3em; }
.Bd-indent { margin-left: 3em; }
.Nd, .Bf, .Op { display: inline; }
.Pa, .Ad { font-style: italic; }
.Ms { font-weight: bold; }
.Bl-diag > dt { font-weight: bold; }
code.Nm, .Fl, .Cm, .Ic, code.In, .Fd, .Fn, .Cd { font-weight: bold; font-family: inherit; }
EOM

cat dinit.8         | mandoc -man -Thtml -Ostyle=style.css > html/dinit.8.html
cat dinitcheck.8    | mandoc -man -Thtml -Ostyle=style.css > html/dinitcheck.8.html
cat dinitctl.8      | mandoc -man -Thtml -Ostyle=style.css > html/dinitctl.8.html
cat dinit-service.5 | mandoc -man -Thtml -Ostyle=style.css > html/dinit-service.5.html
