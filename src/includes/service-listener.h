#ifndef SERVICE_LISTENER_H
#define SERVICE_LISTENER_H

#include "service-constants.h"

class service_record;

// Interface for listening to services
class service_listener
{
    public:
    
    // An event occurred on the service being observed.
    // Listeners must not be added or removed during event notification.
    virtual void service_event(service_record * service, service_event_t event) noexcept = 0;
};

#endif
