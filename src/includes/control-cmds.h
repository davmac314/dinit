#ifndef DINIT_CONTROL_CMDS_H_INCLUDED
#define DINIT_CONTROL_CMDS_H_INCLUDED 1

#include <control-datatypes.h>

// Dinit control command packet types

// Requests:
enum class cp_cmd :dinit_cptypes::cp_cmd_t {
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

    // List services:
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

    // Query status of an individual service
    SERVICESTATUS = 18,

    // Set trigger value for triggered services
    SETTRIGGER = 19,

    // Retrieve buffered output
    CATLOG = 20,

    // Send Signal to process
    SIGNAL = 21,

    // Query service description directory
    QUERYSERVICEDSCDIR = 22,

};

// Replies:
enum class cp_rply :dinit_cptypes::cp_rply_t {
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
    //     followed by 4-byte service handle, 1-byte service state

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
};

// Information (out-of-band):
enum class cp_info :dinit_cptypes::cp_info_t {
    // Service event occurred (4-byte service handle, 1 byte event code)
    SERVICEEVENT = 100,
};

#endif
