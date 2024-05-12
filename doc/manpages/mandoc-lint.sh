#!/bin/sh

# DON'T RUN THIS SCRIPT DIRECTLY, Use "make lint" target.

# POSIX "echo" behaviour is unspecified behavior in some cases, for example
# when the first argument is "-n" or backslash ("\").
# So, replace the shell built-in echo with a printf based function.
# For more info see http://www.etalabs.net/sh_tricks.html
echo()
{
    IFS=" " printf %s\\n "$*"
}

# "mandoc" doesn't support suppressing specific warning
suppress_warning()
{
    buffer="$(echo "$buffer" | grep -v "$1")"
}

type mandoc >/dev/null 2>&1 || (echo "Missing mandoc for linting, Exiting..." && exit 1)
error=""

for man in dinit.8 dinitctl.8 dinitcheck.8 dinit-monitor.8 dinit-service.5 shutdown.8; do
    buffer=$(mandoc -Tlint -Wwarning "$man")
    # There is an annoying warning about date from mandoc:
    #
    # mandoc: ./dinit-service.5:1:23: WARNING: cannot parse date, using it verbatim: TH January 2024
    #
    # We want to get rid of it.
    suppress_warning "WARNING: cannot parse date, using it verbatim:"
    if [ -n "$buffer" ]; then
        error=1
        echo "$buffer"
        continue
    fi
done

if [ -n "$error" ]; then
    echo "There is some error/warning around man-pages, Exiting with 1..."
    exit 1
fi

echo "Every man-page looks good, Exiting with 0..."

exit 0
