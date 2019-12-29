#include "dasynq.h"
#include "dinit.h"

// using eventloop_t = dasynq::event_loop<dasynq::null_mutex>;

eventloop_t event_loop;

int active_control_conns = 0;
bool external_log_open = false;

/*
These are provided in header instead:

void open_control_socket(bool report_ro_failure) noexcept
{
}

void setup_external_log() noexcept
{
}
*/
