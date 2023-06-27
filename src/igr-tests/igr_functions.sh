# Included from run-test.sh files.

### This file contain basic variables for running Integration tests of
### Dinit.

## Basic functions
# According to POSIX, echo has some unspecified behavior in some cases, for example
# when its first argument is "-n" or backslash ("\"). We prefer to rely on
# POSIX documented things.
# So we replace shell built-in echo with a printf based function.
# For more info see http://www.etalabs.net/sh_tricks.html
echo() {
    IFS=" " printf %s\\n "$*"
}
# NOTE: Always quote your error/warning() messages.
error() {
    >&2 echo "${TEST_NAME:-}: Error: $1"
    # Built-in sub_error
    if [ -n "${2:-}" ]; then
        >&2 echo " ... $2"
    fi
    if [ -n "${STAGE:-}" ]; then
        echo "${TEST_NAME:-}: Failed at stage $STAGE."
    fi
    if [ -e "${SOCKET:-}" ]; then
        stop_dinit # A dinit instance is running, stopping...
    fi
    exit 1
}
warning() {
    echo
    >&2 echo "${TEST_NAME:-}: Warning: $1"
    # Built-in sub_warning
    if [ -n "${2:-}" ]; then
        >&2 echo " ... $2"
    fi
    echo
}

## Executable path resolvers functions
# These functions return 0 and set suitable variable when program found.
# And return 1 on failure.
find_dinit() {
    if [ -z "${DINIT:-}" ]; then
        if [ -x "$EXEC_PREFIX/dinit" ]; then
            export DINIT="$EXEC_PREFIX/dinit"
        else
            return 1
        fi
    fi
    return 0
}
find_dinitctl() {
    if [ -z "${DINITCTL:-}" ]; then
        if [ -x "$EXEC_PREFIX/dinitctl" ]; then
            export DINITCTL="$EXEC_PREFIX/dinitctl"
        else
            return 1
        fi
    fi
    return 0
}
find_dinitcheck() {
    if [ -z "${DINITCHECK:-}" ]; then
        if [ -x "$EXEC_PREFIX/dinitcheck" ]; then
            export DINITCHECK="$EXEC_PREFIX/dinitcheck"
        else
            return 1
        fi
    fi
    return 0
}
find_dinitmonitor() {
    if [ -z "${DINITMONITOR:-}" ]; then
        if [ -x "$EXEC_PREFIX/dinit-monitor" ]; then
            export DINITMONITOR="$EXEC_PREFIX/dinit-monitor"
        else
            return 1
        fi
    fi
    return 0
}

## Change directory to run-test.sh location
cd "$(dirname "$0")" || error "Can't change directory to script location."

## Prepare $IGR_OUTPUT
TEST_NAME="${PWD##*/}"
if [ -z "$TEST_NAME" ]; then error "Failed to guess test name."; fi
if [ -z "${IGR_OUTPUT:-}" ]; then
    IGR_OUTPUT="$PWD"
    if [ -z "$IGR_OUTPUT" ]; then error "Failed to guess igr output files location."; fi
    export IGR_OUTPUT
else
    # Igr tests are used by meson.
    mkdir -p "$IGR_OUTPUT/$TEST_NAME/"
    IGR_OUTPUT="$IGR_OUTPUT/$TEST_NAME/"
    export IGR_OUTPUT
fi
export SOCKET="$IGR_OUTPUT/socket"
mkdir -p "$IGR_OUTPUT"/output/
if [ -n "${DEBUG:-}" ]; then
    QUIET=""
else
    QUIET="--quiet"
fi

## Integration tests helper functions
# This function spawn a dinit daemon with "$SOCKET" socket path.
# It's pass its flags as dinit daemon flags.
# RESULT: return 0 & sets "$DINITPID" variable on success.
#         throw an error on failure.
spawn_dinit() {
    find_dinit || error "Cannot find dinit exec path." "Check 'EXEC_PREFIX' variable value"
    "$DINIT" $QUIET -u -d sd -p "$SOCKET" "$@" &
    DINITPID=$!
    # Wait for Dinit socket shows up.
    TIMEOUT=0
    while [ ! -e "$SOCKET" ]; do
        if [ $TIMEOUT -le 600 ]; then
            sleep 0.1
            TIMEOUT=$((TIMEOUT+1))
        else
            error "Dinit reaches timeout but socket didn't create."
        fi
    done
    return 0
}

# This function stop current Dinit instance.
# Doesn't accept anything.
# RESULT: return 0 if Dinit stops.
#         return 0 and throw a warning if Dinit already stopped.
#         throw an error on failure.
# NOTE: Don't use error(), using error() will result a inifinite cycle.
stop_dinit() {
    if [ ! -e "$SOCKET" ]; then
        warning "stop_dinit() called but cannot find any running dinit instance!"
        return 0
    fi
    if find_dinitctl && $DINITCTL $QUIET shutdown -p "$SOCKET"; then
        wait "$DINITPID"
        return 0
    else
        warning "Cannot stopping dinit via dinitctl." "Fallback to killing DINITPID."
        kill "$DINITPID" || ( echo "${TEST_NAME:-}: Cannot stop Dinit instance!" && exit 1 )
        wait "$DINITPID"
    fi
}

# This function spawn a dinit daemon but blocks until dinit exits.
# It's pass its flags as dinit daemon flags.
# RESULT: waits for dinit daemon and return exit code of dinit on sccuess.
#         throw an error on failure.
spawn_dinit_oneshot() {
    find_dinit || error "Cannot find dinit exec path." "Check 'EXEC_PREFIX' variable value."
    "$DINIT" $QUIET -u -d sd -p "$SOCKET" "$@"
}

# This function find dinitctl and allow access to dinit daemon via dinitctl.
# It's pass its flags as dinitctl flags.
# RESULT: Any return code from dinitctl on sccuess.
#         Throw an error on failure.
run_dinitctl() {
    find_dinitctl || error "Cannot find dinitctl exec path." "Check 'EXEC_PREFIX' variable value."
    "$DINITCTL" -p "$SOCKET" "$@"
}

# This function find dinitcheck and allow access to dinitcheck.
# It's pass its flags as dinitcheck flags.
# RESULT: Any return code from dinitcheck on sccuess.
#         Throw an error on failure.
run_dinitcheck() {
    find_dinitcheck || error "Cannot find dinitcheck exec path." "Check 'EXEC_PREFIX' variable value."
    "$DINITCHECK" -d sd "$@"
}

# This function compare content of given file with expected result.
# Accepts file as $1 and a text as $2.
# RESULT: return 0 if content of file is the same with given text.
#         return 1 if content of file is not the same with given text.
#         throw an error on any other failure.
compare_text() {
    if [ ! -e "$1" ]; then
        error "$1 file isn't exist!"
    fi
    FILE="$(cat "$1" || error "Cannot read given file!")"
    if [ "$FILE" = "$2" ]; then
        return 0
    else
        return 1
    fi
}

# This function compare content of given file with expected result in another file.
# Accepts file as $1 and another file as $2.
# RESULT: return 0 if content of file is the same with another file.
#         return 1 if content of file is not the same with another file.
#         throw an error on any other failure.
compare_file() {
    if [ ! -e "$1" ]; then
        error "$1 file isn't exist!"
    fi
    if [ ! -e "$2" ]; then
        error "$2 file isn't exist!"
    fi
    if cmp -s "$1" "$2"; then
        return 0
    else
        return 1
    fi
}

# This function compare command result with content of a file.
# Accepts command as $1 and expected file as $2.
# Also if you need to capture stderr, set "err" as $3.
# RESULT: set $CMD_OUT variable contain command output.
#         return 0 if command output is the same with content of file.
#         return 1 if command output isn't the same with content of file.
#         throw an error on any other failure.
compare_cmd() {
    if [ ! -e "$2" ]; then
        error "$2 file isn't exist!"
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
