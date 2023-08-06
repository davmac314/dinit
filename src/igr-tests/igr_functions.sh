# Included from run-test.sh files. DO NOT run independently.

### This file contain basic variables & functions for running Integration tests of Dinit.

# Input variables:

# Directory containing executable binaries. If unset defaults to two-levels up from current
# working directory
DINIT_BINDIR="${DINIT_BINDIR-../..}"


# After sourcing this script, the following output variables are set:
#
# IGR_OUTPUT - directory for test output files (also exported to environment)
# TEST_NAME - name of the current test
# QUIET - will be set to "--quiet" unless DEBUG is set 
#
# Also, the various functions defined below may be used.


## Basic functions
# According to POSIX, echo has unspecified behavior in some cases, for example
# when its first argument is "-n" or backslash ("\").
# So we replace the shell built-in echo with a printf-based function.
# For more information see: http://www.etalabs.net/sh_tricks.html
echo() {
    IFS=" " printf %s\\n "$*"
}

# Issue an error message and exit.
#  $1 - the main error message
#  $2 - (optional) detail/additional info
#  $TEST_NAME - name of the current test
#  $STAGE - (optional) test stage description
error() {
    >&2 echo "${TEST_NAME:-}: Error: $1"
    if [ -n "${2:-}" ]; then
        >&2 echo " ... $2"
    fi
    if [ -n "${STAGE:-}" ]; then
        >&2 echo "${TEST_NAME:-}: Failed at stage $STAGE."
    fi
    if [ -e "${SOCKET:-}" ]; then
        stop_dinit
    fi
    exit 1
}

# Issue a warning message.
#   $1 - the main warning message
#   $2 - (optional) detail/additional info
#   $TEST_NAME - name of the current test
warning() {
    echo
    >&2 echo "${TEST_NAME:-}: Warning: $1"
    if [ -n "${2:-}" ]; then
        >&2 echo " ... $2"
    fi
    echo
}

## Executable path resolvers functions
# These return 0 and set suitable variable when program found, or return 1 on failure.

# Utility / implementation for find_xxx()
#  $1 - name of the executable to locate
#  $2 - name of the variable to set with the path to the executable
find_executable() {
    exename=$1
    varname=$2
    if [ -z "$(eval "echo \${${varname}:-}")" ]; then
        if [ -x "$DINIT_BINDIR/$exename" ]; then
            export "$varname"="$DINIT_BINDIR/$exename"
        else
            return 1
        fi
    fi
}
find_dinit() { find_executable dinit DINIT; }
find_dinitctl() { find_executable dinitctl DINITCTL; }
find_dinitcheck() { find_executable dinitcheck DINITCHECK; }
find_dinitmonitor() { find_executable dinit-monitor DINITMONITOR; }

# Prepare IGR_OUTPUT.
# Three basic modes:
# - IGR_OUTPUT is already set
# - IGR_OUTPUT_BASE is set, append test name and use result as IGR_OUTPUT
# - neither, use "output" within current directory as IGR_OUTPUT
TEST_NAME="${PWD##*/}"
[ -n "$TEST_NAME" ] || error "Failed to guess test name."
if [ -z "${IGR_OUTPUT:-}" ]; then
    if [ -n "${IGR_OUTPUT_BASE:-}" ]; then
        export IGR_OUTPUT="${IGR_OUTPUT_BASE}/${TEST_NAME}"
    else
        export IGR_OUTPUT="$PWD"/output
    fi
fi
export SOCKET="$IGR_OUTPUT/socket"
mkdir -p "$IGR_OUTPUT"
if [ -n "${DEBUG:-}" ]; then
    QUIET=""
else
    QUIET="--quiet"
fi

## Integration tests helper functions

# spawn_dinit: spawn a dinit daemon using "$SOCKET" as socket path.
# Any arguments are passed on to dinit.
# RESULT: Return 0 & sets DINITPID variable on success.
#         Message and exit on failure.
spawn_dinit() {
    find_dinit || error "Cannot find dinit exec path." "Specify 'DINIT_BINDIR' and/or ensure dinit is compiled."
    "$DINIT" $QUIET -u -d sd -p "$SOCKET" -l /dev/null "$@" &
    DINITPID=$!
    # Wait for Dinit socket to show up.
    TIMEOUT=0
    while [ ! -e "$SOCKET" ]; do
        if [ $TIMEOUT -le 600 ]; then
            sleep 0.1
            TIMEOUT=$((TIMEOUT+1))
        else
            error "Starting dinit: reached timeout without detecting socket creation."
        fi
    done
    return 0
}

# Stop the current Dinit instance.
# Takes no arguments.
# RESULT: Returns 0 if Dinit stops.
#         Returns 0 and issue a warning if Dinit already stopped.
stop_dinit() {
    # NOTE: We can't use error() within, using error() will result in an infinite cycle as
    # error() calls this function.
    if [ ! -e "$SOCKET" ]; then
        warning "stop_dinit() called but cannot find any running dinit instance!"
        return 0
    fi
    if find_dinitctl && $DINITCTL $QUIET shutdown -p "$SOCKET"; then
        wait "$DINITPID"
        return 0
    else
        warning "Cannot stop dinit via dinitctl." "Falling back to killing dinit (pid=$DINITPID) via signal."
        kill "$DINITPID" || { echo "${TEST_NAME:-}: Cannot stop Dinit instance!" && exit 1; } >&2
        wait "$DINITPID"
    fi
}

# Spawns a dinit daemon and waits until it exits.
# Arguments are passed on to dinit.
# RESULT: Returns exit code of dinit.
spawn_dinit_oneshot() {
    find_dinit || error "Cannot find dinit exec path." "Specify 'DINIT_BINDIR' and/or ensure dinit is compiled."
    exit_code=0
    "$DINIT" $QUIET -u -d sd -p "$SOCKET" -l /dev/null "$@" || exit_code=$?
    return $exit_code
}

# Run a dinitctl command against the currently running dinit instance.
# Any arguments are passed to dinitctl.
# RESULT: Returns exit code from dinitctl.
run_dinitctl() {
    find_dinitctl || error "Cannot find dinitctl exec path." "Specify 'DINIT_BINDIR' and/or ensure dinit is compiled."
    exit_code=0
    "$DINITCTL" -p "$SOCKET" "$@" || exit_code=$?
    return $exit_code
}

# Run dinitcheck.
# Any arguments are passed to dinitcheck.
# RESULT: Return exit code from dinitcheck.
run_dinitcheck() {
    find_dinitcheck || error "Cannot find dinitcheck exec path." "Specify 'DINIT_BINDIR' and/or ensure dinit is compiled."
    exit_code=0
    "$DINITCHECK" -d sd "$@" || exit_code=$?
    return $exit_code
}

# Compares the contents of a given file with an expected result (text).
# Final newlines are stripped from file contents before comparison.
# Accepts file as $1 and a text as $2.
# RESULT: Returns 0 if content of file is the same with given text.
#         Returns 1 if content of file is not the same with given text.
#         Exits with an error if the file doesn't exist.
compare_text() {
    if [ ! -e "$1" ]; then
        error "$1 file doesn't exist!"
    fi
    FILE="$(cat "$1")" || error "Cannot read given file!"
    if [ "$FILE" = "$2" ]; then
        return 0
    else
        return 1
    fi
}

# Compares the contents of a given file with an expected result (text).
# Newlines in file contents are preserved and compared.
# Accepts file as $1 and a text as $2.
# RESULT: Returns 0 if content of file is the same with given text.
#         Returns 1 if content of file is not the same with given text.
#         Exits with an error if the file doesn't exist.
compare_text_nl() {
    if [ ! -e "$1" ]; then
        error "$1 file doesn't exist!"
    fi
    # capture file contents and preserve final newline (by appending an 'x' and removing it after)
    FILE="$(cat "$1" && echo "x")" || error "Cannot read given file!"
    FILE="${FILE%?}"
    if [ "$FILE" = "$2" ]; then
        return 0
    else
        return 1
    fi
}

# Compare the contents of a given file with the expected result (in another file).
# Accepts actual result file as $1 and expected results file as $2.
# RESULT: Return 0 if content of file is the same as the other file.
#         Return 1 otherwise.
#         Exits with an error if either file doesn't exist.
compare_file() {
    if [ ! -e "$1" ]; then
        error "$1 file doesn't exist!"
    fi
    if [ ! -e "$2" ]; then
        error "$2 file doesn't exist!"
    fi
    if cmp -s "$1" "$2"; then
        return 0
    else
        return 1
    fi
}

# Compare output from a command with the contents of a file.
# Accepts command (with arguments) as $1 and file with expected result as $2.
# To capture stderr as well as stdout, set $3 equal to "err".
# RESULT: CMD_OUT variable contains the command output.
#         return 0 if command output is the same as the file contents.
#         return 1 otherwise.
#         Exits with an error if the file doesn't exist.
compare_cmd() {
    if [ ! -e "$2" ]; then
        error "$2 file doesn't exist!"
    fi
    if [ "${3:-}" = "err" ]; then
        CMD_OUT="$($1 2>&1 || true)"
    elif [ -z "${3:-}" ]; then
        CMD_OUT="$($1 || true)"
    else
        warning "compare_cmd(): Invalid argument for capturing stderr: $3" "Ignoring..."
        CMD_OUT="$($1 || true)"
    fi
    if [ "$CMD_OUT" = "$(cat "$2")" ]; then
        return 0
    else
        return 1
    fi
}

# Capture the exact output of a command, including any final newlines.
#  $1 - variable name to store output in
#  $2...  command to run (with arguments following)
capture_exact_output() {
    name=$1; shift
    cmd=$1; shift
    # execute the command and output an additional '/' which is then stripped
    # (the '/' inhibits the usual stripping of trailing newlines)
    r=0
    output="$(r=0; "$cmd" "$@" || r=$?; echo /; exit $r)" || r=$?
    output="${output%/}"
    eval "$name=\$output"
    return $r
}
