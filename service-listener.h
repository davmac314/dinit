#ifndef SERVICE_LISTENER_H
#define SERVICE_LISTENER_H

class ServiceRecord;

enum class ServiceEvent {
    STARTED,           // Service was started (reached STARTED state)
    STOPPED,           // Service was stopped (reached STOPPED state)
    FAILEDSTART,       // Service failed to start (possibly due to dependency failing)
    STARTCANCELLED,    // Service was set to be started but a stop was requested
    STOPCANCELLED      // Service was set to be stopped but a start was requested
};

// Interface for listening to services
class ServiceListener
{
    public:
    
    // An event occurred on the service being observed.
    // Listeners must not be added or removed during event notification.
    virtual void serviceEvent(ServiceRecord * service, ServiceEvent event) noexcept = 0;
};

#endif
