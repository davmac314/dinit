namespace dasync {

// Event type bits
constexpr unsigned int in_events = 1;
constexpr unsigned int out_events = 2;
constexpr unsigned int err_events = 4;

constexpr unsigned int one_shot = 8;

// Masks:
constexpr unsigned int IO_EVENTS = in_events | out_events;

}
