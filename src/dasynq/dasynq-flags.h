#ifndef DASYNQ_FLAGS_H_INCLUDED
#define DASYNQ_FLAGS_H_INCLUDED

namespace dasynq {

// Event type bits
constexpr unsigned int IN_EVENTS = 1;
constexpr unsigned int OUT_EVENTS = 2;
constexpr unsigned int ERR_EVENTS = 4;

constexpr unsigned int ONE_SHOT = 8;

// Masks:
constexpr unsigned int IO_EVENTS = IN_EVENTS | OUT_EVENTS;

// Different timer clock types
enum class clock_type
{
    SYSTEM,
    MONOTONIC
};

}

#endif
