// Dinit control command packet types

// Start or stop a named service:
constexpr static int DINIT_CP_STARTSERVICE = 0;
constexpr static int DINIT_CP_STOPSERVICE  = 1;

// Roll-back all services:
constexpr static int DINIT_CP_ROLLBACKALL = 2;

// Reply: request completed
constexpr static int DINIT_RP_COMPLETED = 50;
