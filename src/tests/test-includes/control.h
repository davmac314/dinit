// dummy control.h

#include "dinit.h"

class service_set;

class control_conn_t
{
    public:
    control_conn_t(eventloop_t &loop, service_set * services_p, int fd)
    {
        // active_control_conns++;
    }
};
