#ifndef SERVICE_CONSTANTS_H
#define SERVICE_CONSTANTS_H

#include "mconfig.h"

/* Service states */
enum class service_state_t {
    STOPPED,    // service is not running.
    STARTING,   // service is starting, and will start (or fail to start) in time.
    STARTED,    // service is running,
    STOPPING    // service script is stopping and will stop.
};

/* Service types */
enum class service_type_t {
    DUMMY,      // Dummy service, used to detect cyclice dependencies
    PROCESS,    // Service runs as a process, and can be stopped by
                // sending the process a signal (usually SIGTERM)
    BGPROCESS,  // Service runs as a process which "daemonizes" to run in the
                // "background".
    SCRIPTED,   // Service requires an external command to start,
                // and a second command to stop
    INTERNAL    // Internal service, runs no external process
};

/* Service events */
enum class service_event_t {
    STARTED,           // Service was started (reached STARTED state)
    STOPPED,           // Service was stopped (reached STOPPED state)
    FAILEDSTART,       // Service failed to start (possibly due to dependency failing)
    STARTCANCELLED,    // Service was set to be started but a stop was requested
    STOPCANCELLED      // Service was set to be stopped but a start was requested
};

/* Shutdown types */
enum class shutdown_type_t {
    NONE,              // No explicit shutdown
    REMAIN,            // Continue running with no services
    HALT,              // Halt system without powering down
    POWEROFF,          // Power off system
    REBOOT             // Reboot system
};

/* Reasons for why service stopped */
enum class stopped_reason_t
{
    NORMAL,

    // Start failures:
    DEPFAILED, // A dependency failed to start
    FAILED,    // failed to start (process terminated)
	EXECFAILED, // failed to start (couldn't launch process)
    TIMEDOUT,  // timed out when starting

    // Failure(?) after starting:
    TERMINATED // process terminated
};

inline bool did_finish(stopped_reason_t reason)
{
    return reason == stopped_reason_t::TERMINATED;
}

/* Execution stage */
enum class exec_stage {
    ARRANGE_FDS, READ_ENV_FILE, SET_NOTIFYFD_VAR, SETUP_ACTIVATION_SOCKET, SETUP_CONTROL_SOCKET,
    CHDIR, SETUP_STDINOUTERR, ENTER_CGROUP, SET_RLIMITS, SET_UIDGID,
    /* values for future expansion: */
    SPARE1, SPARE2, SPARE3, SPARE4, SPARE5, SPARE6, SPARE7, SPARE8,
    /* must be last: */ DO_EXEC
};

/* Strings describing the execution stages (failure points). */
const char * const exec_stage_descriptions[/* static_cast<int>(exec_stage::DO_EXEC) + 1 */] = {
        "arranging file descriptors",   // ARRANGE_FDS
        "reading environment file",     // READ_ENV_FILE
        "setting environment variable", // SET_NOTIFYFD_VAR
        "setting up activation socket", // SETUP_ACTIVATION_SOCKET
        "setting up control socket",    // SETUP_CONTROL_SOCKET
        "changing directory",           // CHDIR
        "setting up standard input/output descriptors", // SETUP_STDINOUTERR
        #if SUPPORT_CGROUPS
        "entering cgroup",              // ENTER_CGROUP
        #else
        "",                             // ENTER_CGROUP (placeholder)
        #endif
        "setting resource limits",      // SET_RLIMITS
        "setting user/group ID",        // SET_UIDGID
        nullptr,                        // SPARE1
        nullptr,                        // SPARE2
        nullptr,                        // SPARE3
        nullptr,                        // SPARE4
        nullptr,                        // SPARE5
        nullptr,                        // SPARE6
        nullptr,                        // SPARE7
        nullptr,                        // SPARE8
        "executing command"             // DO_EXEC
};

static_assert(sizeof(exec_stage_descriptions) == (sizeof(char *) * (static_cast<int>(exec_stage::DO_EXEC) + 1)),
        "exec_stage_descriptions missing a stage description");

enum class dependency_type
{
    REGULAR,
    SOFT,       // dependency starts in parallel, failure/stop does not affect dependent
    WAITS_FOR,  // as for SOFT, but dependent waits until dependency starts/fails before starting
    MILESTONE,  // dependency must start successfully, but once started the dependency becomes soft
    BEFORE,     // "before" ordering constraint (specified via the "to" service)
    AFTER       // "after" ordering constraint (specified via the "from" service)
};

// Service set type identifiers:
constexpr int SSET_TYPE_NONE = 0;
constexpr int SSET_TYPE_DIRLOAD = 1;

#endif
