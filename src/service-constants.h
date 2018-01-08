#ifndef SERVICE_CONSTANTS_H
#define SERVICE_CONSTANTS_H

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
    CONTINUE,          // Continue normal boot sequence (used after single-user shell)
    HALT,              // Halt system without powering down
    POWEROFF,          // Power off system
    REBOOT             // Reboot system
};

#endif
