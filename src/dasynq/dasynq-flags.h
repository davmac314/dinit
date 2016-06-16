namespace dasynq {

// Event type bits
constexpr unsigned int IN_EVENTS = 1;
constexpr unsigned int OUT_EVENTS = 2;
constexpr unsigned int ERR_EVENTS = 4;

constexpr unsigned int ONE_SHOT = 8;

// Masks:
constexpr unsigned int IO_EVENTS = IN_EVENTS | OUT_EVENTS;

}
