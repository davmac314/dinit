#include "service.h"

// A test service.
//
// This service can be induced to successfully start or fail (once it is STARTING) by calling either the
// started() or failed_to_start() functions.
//
class test_service : public service_record
{
    public:
    bool bring_up_reqd = false;
    bool start_interruptible = false;

    test_service(service_set *set, std::string name, service_type_t type_p,
            const std::list<prelim_dep> &deplist_p)
            : service_record(set, name, type_p, deplist_p)
    {

    }

    bool auto_stop = true;  // whether to call stopped() immediately from bring_down()

    // Do any post-dependency startup; return false on failure
    virtual bool bring_up() noexcept override
    {
        // return service_record::bring_up();
        bring_up_reqd = true;
        return true;
    }

    // All dependents have stopped.
    virtual void bring_down() noexcept override
    {
        waiting_for_deps = false;
        if (auto_stop) {
            stopped();
        }
    }

    void stopped() noexcept
    {
        assert(get_state() != service_state_t::STOPPED);
        service_record::stopped();
    }

    virtual bool can_interrupt_start() noexcept override
    {
        return start_interruptible;
    }

    virtual bool issue_start_interrupt() noexcept override
    {
        return true;
    }

    void started() noexcept
    {
        assert(bring_up_reqd);
        service_record::started();
    }

    void failed_to_start() noexcept
    {
        service_record::failed_to_start();
    }
};

// Service listener for use in tests
class test_listener : public service_listener
{
    public:
    bool got_started = false;
    bool got_stopped = false;
    bool start_cancelled = false;
    bool stop_cancelled = false;

    void service_event(service_record * service, service_event_t event) noexcept override
    {
        switch (event) {
        case service_event_t::STARTED:
            got_started = true;
            break;
        case service_event_t::STOPPED:
            got_stopped = true;
            break;
        case service_event_t::STARTCANCELLED:
            start_cancelled = true;
            break;
        case service_event_t::STOPCANCELLED:
            stop_cancelled = true;
            break;
        case service_event_t::FAILEDSTART:
            break;
        }
    }
};
