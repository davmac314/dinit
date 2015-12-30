#ifndef SERVICE_LISTENER_H
#define SERVICE_LISTENER_H

#include <service-constants.h>

class ServiceRecord;

// Interface for listening to services
class ServiceListener
{
    public:
    
    // An event occurred on the service being observed.
    // Listeners must not be added or removed during event notification.
    virtual void serviceEvent(ServiceRecord * service, ServiceEvent event) noexcept = 0;
};

#endif
