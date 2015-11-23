// Dinit control command packet types

// Requests:

// Start or stop a named service:
constexpr static int DINIT_CP_STARTSERVICE = 0;
constexpr static int DINIT_CP_STOPSERVICE  = 1;

// Roll-back all services:
constexpr static int DINIT_CP_ROLLBACKALL = 2;

// Query protocol version:
constexpr static int DINIT_CP_QUERYVERSION = 3;


// Replies:

// Reply: ACK/NAK to request
constexpr static int DINIT_RP_ACK = 50;
constexpr static int DINIT_RP_NAK = 51;

// Request was bad (connection will be closed)
constexpr static int DINIT_RP_BADREQ = 52;

// Connection being closed due to out-of-memory condition
constexpr static int DINIT_RP_OOM = 53;

// Reply: rollback completed
constexpr static int DINIT_ROLLBACK_COMPLETED = 60;

// Start service replies:
constexpr static int DINIT_RP_SERVICELOADERR = 61;
constexpr static int DINIT_RP_SERVICEOOM = 62; // couldn't start due to out-of-memory

// Query version response:
constexpr static int DINIT_RP_CPVERSION = 63;
