#ifndef DINIT_H_INCLUDED
#define DINIT_H_INCLUDED 1

#include <dasynq.h>

/*
 * General Dinit definitions.
 */

using eventloop_t = dasynq::event_loop<dasynq::null_mutex>;

using clock_type = dasynq::clock_type;
using rearm = dasynq::rearm;
using time_val = dasynq::time_val;

class environment;

void rootfs_is_rw() noexcept;
void setup_external_log() noexcept;

extern eventloop_t event_loop;

#endif
