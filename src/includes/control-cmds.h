#ifndef DINIT_CONTROL_CMDS_H_INCLUDED
#define DINIT_CONTROL_CMDS_H_INCLUDED 1

// Dinit control command packet types

// Requests:

// Query protocol version:
constexpr static int DINIT_CP_QUERYVERSION = 0;

// Find (but don't load) a service:
constexpr static int DINIT_CP_FINDSERVICE = 1;

// Find or load a service:
constexpr static int DINIT_CP_LOADSERVICE = 2;

// Start or stop a service:
constexpr static int DINIT_CP_STARTSERVICE = 3;
constexpr static int DINIT_CP_STOPSERVICE  = 4;
constexpr static int DINIT_CP_WAKESERVICE = 5;
constexpr static int DINIT_CP_RELEASESERVICE = 6;

constexpr static int DINIT_CP_UNPINSERVICE = 7;

// List services:
constexpr static int DINIT_CP_LISTSERVICES = 8;

// Unload a service:
constexpr static int DINIT_CP_UNLOADSERVICE = 9;

// Shutdown:
constexpr static int DINIT_CP_SHUTDOWN = 10;
 // followed by 1-byte shutdown type

// Add/remove dependency to existing service:
constexpr static int DINIT_CP_ADD_DEP = 11;
constexpr static int DINIT_CP_REM_DEP = 12;

// Query service load path / mechanism:
constexpr static int DINIT_CP_QUERY_LOAD_MECH = 13;

// Add a waits for dependency from one service to another, and start the dependency:
constexpr static int DINIT_CP_ENABLESERVICE = 14;

// Find the name of a service (from a handle)
constexpr static int DINIT_CP_QUERYSERVICENAME = 15;

// Reload a service:
constexpr static int DINIT_CP_RELOADSERVICE = 16;

// Export a set of environment variables into activation environment:
constexpr static int DINIT_CP_SETENV = 17;

// Query status of an individual service
constexpr static int DINIT_CP_SERVICESTATUS = 18;

// Set trigger value for triggered services
constexpr static int DINIT_CP_SETTRIGGER = 19;

// Retrieve buffered output
constexpr static int DINIT_CP_CATLOG = 20;

// Send Signal to process
constexpr static int DINIT_CP_SIGNAL = 21;


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

// Information (out-of-band):

// Service event occurred (4-byte service handle, 1 byte event code)
constexpr static int DINIT_IP_SERVICEEVENT = 100;

#endif
