#!/bin/sh

set -e

cd $(dirname $0)
mkdir -p man-pages-html

cat ../manpages/dinit.8         | mandoc -man -Thtml -Ostyle=style.css | grep -v style.css > man-pages-html/dinit.8.html
cat ../manpages/dinitcheck.8    | mandoc -man -Thtml -Ostyle=style.css | grep -v style.css > man-pages-html/dinitcheck.8.html
cat ../manpages/dinitctl.8      | mandoc -man -Thtml -Ostyle=style.css | grep -v style.css > man-pages-html/dinitctl.8.html
cat ../manpages/dinit-monitor.8 | mandoc -man -Thtml -Ostyle=style.css | grep -v style.css > man-pages-html/dinit-monitor.8.html
cat ../manpages/dinit-service.5 | mandoc -man -Thtml -Ostyle=style.css | grep -v style.css > man-pages-html/dinit-service.5.html
