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

    // Whether a STARTING service can immediately transition to STOPPED (as opposed to
    // having to wait for it reach STARTED and then go through STOPPING).
    virtual bool can_interrupt_start() noexcept override
    {
        return waiting_for_deps;
    }

    virtual bool interrupt_start() noexcept override
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
