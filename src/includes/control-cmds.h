#ifndef DINIT_CONTROL_CMDS_H_INCLUDED
#define DINIT_CONTROL_CMDS_H_INCLUDED 1

#include <control-datatypes.h>

// Dinit control command packet types

// Control protocol versions:
// 1 - dinit 0.16 and prior
// 2 - dinit 0.17 (adds SETTRIGGER, CATLOG, SIGNAL)
// 3 - dinit 0.17.1 (adds QUERYSERVICEDSCDIR)
// 4 - dinit 0.18.0 (adds CLOSEHANDLE, GETALLENV)
// 5 - dinit 0.19.1 (process status now represented as ([int]si_code + [int]si_status) rather than
//                   a single integer; SERVICEEVENT5 sent alongside SERVICEEVENT; adds LISTENENV, ENVEVENT)
//    (dinit 0.19.2 removed support for LISTENENV/ENVEVENT, they were added again in 0.19.3)

// Requests:
enum class cp_cmd : dinit_cptypes::cp_cmd_t {
    // Query protocol version:
    QUERYVERSION = 0,

    // Find (but don't load) a service:
    FINDSERVICE = 1,

    // Find or load a service:
    LOADSERVICE = 2,

    // Start or stop a service:
    STARTSERVICE = 3,
    STOPSERVICE  = 4,
    WAKESERVICE = 5,
    RELEASESERVICE = 6,

    UNPINSERVICE = 7,

    // List services (deprecated):
    LISTSERVICES = 8,

    // Unload a service:
    UNLOADSERVICE = 9,

    // Shutdown:
    SHUTDOWN = 10,
     // followed by 1-byte shutdown type

    // Add/remove dependency to existing service:
    ADD_DEP = 11,
    REM_DEP = 12,

    // Query service load path / mechanism:
    QUERY_LOAD_MECH = 13,

    // Add a waits for dependency from one service to another, and start the dependency:
    ENABLESERVICE = 14,

    // Find the name of a service (from a handle)
    QUERYSERVICENAME = 15,

    // Reload a service:
    RELOADSERVICE = 16,

    // Export a set of environment variables into activation environment:
    SETENV = 17,

    // Query status of an individual service (deprecated)
    SERVICESTATUS = 18,

    // Set trigger value for triggered services
    SETTRIGGER = 19,

    // Retrieve buffered output
    CATLOG = 20,

    // Send Signal to process
    SIGNAL = 21,

    // Query service description directory
    QUERYSERVICEDSCDIR = 22,

    // "Close" a service handle
    CLOSEHANDLE = 23,

    // Retrieve complete environment
    GETALLENV = 24,

    // List services (protocol version 5+)
    LISTSERVICES5 = 25,

    // Query status of an individual service (5+)
    SERVICESTATUS5 = 26,

    // Start listening to environment events
    LISTENENV = 27,
};

// Replies:
enum class cp_rply : dinit_cptypes::cp_rply_t {
    // Reply: ACK/NAK to request
    ACK = 50,
    NAK = 51,

    // Request was bad (connection will be closed)
    BADREQ = 52,

    // Connection being closed due to out-of-memory condition
    OOM = 53,

    // Start service replies:
    SERVICELOADERR = 54,
    SERVICEOOM = 55, // couldn't start due to out-of-memory

    // Not (any longer?) used
    //SSISSUED = 56,  // service start/stop was issued (includes 4-byte service handle)
    //SSREDUNDANT = 57,  // service was already started/stopped (or for stop, not loaded)

    // Query version response:
    CPVERSION = 58,

    // Service record loaded/found
    SERVICERECORD = 59,
    // 1-byte service state, followed by 4-byte service handle, followed by 1-byte service target state

    // Couldn't find/load service
    NOSERVICE = 60,

    // Service is already started/stopped
    ALREADYSS = 61,

    // Information on a service / list complete:
    SVCINFO = 62,
    LISTDONE = 63,

    // Service loader information:
    LOADER_MECH = 64,

    // Dependent services prevent stopping/restarting. Includes size_t count, handle_t * N handles.
    DEPENDENTS = 65,

    // Service name:
    SERVICENAME = 66,

    // Service is pinned stopped/started:
    PINNEDSTOPPED = 67,
    PINNEDSTARTED = 68,

    // Shutdown is in progress, can't start/restart/wake service:
    SHUTTINGDOWN = 69,

    // Service status:
    SERVICESTATUS = 70,

    // Service description error:
    SERVICE_DESC_ERR = 71,
    // Service load error (general):
    SERVICE_LOAD_ERR = 72,

    // Service log:
    SERVICE_LOG = 73,

    // Signal replies:
    SIGNAL_NOPID = 74,
    SIGNAL_BADSIG = 75,
    SIGNAL_KILLERR = 76,

    // Service description directory:
    SVCDSCDIR = 77,

    // Retrieve complete environment
    ALLENV = 78,

    // "Pre-acknowledgement". Issued before main reply after restart command
    // (to avoid race condition for client tracking service status)
    PREACK = 79,
};

// Information (out-of-band):
enum class cp_info : dinit_cptypes::cp_info_t {
    // Service event occurred (4-byte service handle, 1 byte event code)
    SERVICEEVENT = 100,
    // Service event for protocol version 5+ - 4 byte handle, 1 byte event code, proc_status_t status
    SERVICEEVENT5 = 101,
    // Environment event; 2 bytes length + env string
    ENVEVENT = 102,
};

#endif
