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

// Reply: ACK/NAK to request
constexpr static int DINIT_RP_ACK = 50;
constexpr static int DINIT_RP_NAK = 51;

// Request was bad (connection will be closed)
constexpr static int DINIT_RP_BADREQ = 52;

// Connection being closed due to out-of-memory condition
constexpr static int DINIT_RP_OOM = 53;

// Start service replies:
constexpr static int DINIT_RP_SERVICELOADERR = 54;
constexpr static int DINIT_RP_SERVICEOOM = 55; // couldn't start due to out-of-memory

// Not (any longer?) used
//constexpr static int DINIT_RP_SSISSUED = 56;  // service start/stop was issued (includes 4-byte service handle)
//constexpr static int DINIT_RP_SSREDUNDANT = 57;  // service was already started/stopped (or for stop, not loaded)

// Query version response:
constexpr static int DINIT_RP_CPVERSION = 58;

// Service record loaded/found
constexpr static int DINIT_RP_SERVICERECORD = 59;
//     followed by 4-byte service handle, 1-byte service state

// Couldn't find/load service
constexpr static int DINIT_RP_NOSERVICE = 60;

// Service is already started/stopped
constexpr static int DINIT_RP_ALREADYSS = 61;

// Information on a service / list complete:
constexpr static int DINIT_RP_SVCINFO = 62;
constexpr static int DINIT_RP_LISTDONE = 63;

// Service loader information:
constexpr static int DINIT_RP_LOADER_MECH = 64;

// Dependent services prevent stopping/restarting. Includes size_t count, handle_t * N handles.
constexpr static int DINIT_RP_DEPENDENTS = 65;

// Service name:
constexpr static int DINIT_RP_SERVICENAME = 66;

// Service is pinned stopped/started:
constexpr static int DINIT_RP_PINNEDSTOPPED = 67;
constexpr static int DINIT_RP_PINNEDSTARTED = 68;

// Shutdown is in progress, can't start/restart/wake service:
constexpr static int DINIT_RP_SHUTTINGDOWN = 69;

// Service status:
constexpr static int DINIT_RP_SERVICESTATUS = 70;

// Service description error:
constexpr static int DINIT_RP_SERVICE_DESC_ERR = 71;
// Service load error (general):
constexpr static int DINIT_RP_SERVICE_LOAD_ERR = 72;

// Service log:
constexpr static int DINIT_RP_SERVICE_LOG = 73;

// Signal replies:
constexpr static int DINIT_RP_SIGNAL_NOPID = 74;
constexpr static int DINIT_RP_SIGNAL_BADSIG = 75;
constexpr static int DINIT_RP_SIGNAL_KILLERR = 76;

// Service description directory:
constexpr static int DINIT_RP_SVCDSCDIR = 77;


// Information (out-of-band):

// Service event occurred (4-byte service handle, 1 byte event code)
constexpr static int DINIT_IP_SERVICEEVENT = 100;

#endif
