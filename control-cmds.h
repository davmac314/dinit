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

// Roll-back all services:
constexpr static int DINIT_CP_ROLLBACKALL = 5;



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

constexpr static int DINIT_RP_SSISSUED = 56;  // service start/stop was issued (includes 4-byte service handle)
constexpr static int DINIT_RP_SSREDUNDANT = 57;  // service was already started/stopped (or for stop, not loaded)

// Query version response:
constexpr static int DINIT_RP_CPVERSION = 58;

// Service record loaded/found
constexpr static int DINIT_RP_SERVICERECORD = 59;
//     followed by 4-byte service handle, 1-byte service state

// Couldn't find/load service
constexpr static int DINIT_RP_NOSERVICE = 60;



// Information:

// Service event occurred (4-byte service handle, 1 byte event code)
constexpr static int DINIT_IP_SERVICEEVENT = 100;

// rollback completed
constexpr static int DINIT_ROLLBACK_COMPLETED = 101;
