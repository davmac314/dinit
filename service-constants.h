#ifndef SERVICE_CONSTANTS_H
#define SERVICE_CONSTANTS_H

/* Service states */
enum class ServiceState {
    STOPPED,    // service is not running.
    STARTING,   // service is starting, and will start (or fail to start) in time.
    STARTED,    // service is running,
    STOPPING    // service script is stopping and will stop.
};

/* Service types */
enum class ServiceType {
    DUMMY,      // dummy service, used to detect cyclice dependencies
    PROCESS,    // service runs as a process, and can be stopped by
                // sending the process a signal (SIGTERM)
    SCRIPTED,   // service requires an external command to start,
                // and a second command to stop
    INTERNAL    // internal service, runs no external process
};

/* Service events */
enum class ServiceEvent {
    STARTED,           // Service was started (reached STARTED state)
    STOPPED,           // Service was stopped (reached STOPPED state)
    FAILEDSTART,       // Service failed to start (possibly due to dependency failing)
    STARTCANCELLED,    // Service was set to be started but a stop was requested
    STOPCANCELLED      // Service was set to be stopped but a start was requested
};


#endif
