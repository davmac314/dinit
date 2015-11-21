// Dinit control command packet types

// Start or stop a named service:
constexpr static int DINIT_CP_STARTSERVICE = 0;
constexpr static int DINIT_CP_STOPSERVICE  = 1;

// Roll-back all services:
constexpr static int DINIT_CP_ROLLBACKALL = 2;

// Reply: ACK/NAK to request
constexpr static int DINIT_RP_ACK = 50;
constexpr static int DINIT_RP_NAK = 51;

// Reply: rollback completed
constexpr static int DINIT_ROLLBACK_COMPLETED = 52;
