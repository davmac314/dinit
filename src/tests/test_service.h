#include "service.h"

class test_service : public service_record
{
    public:
    test_service(service_set *set, std::string name, service_type type_p,
            const std::list<prelim_dep> &deplist_p)
            : service_record(set, name, type_p, deplist_p)
    {

    }

    // Do any post-dependency startup; return false on failure
    virtual bool start_ps_process() noexcept override
    {
        // return service_record::start_ps_process();
        return true;
    }

    // All dependents have stopped.
    virtual void all_deps_stopped() noexcept override
    {
        return service_record::all_deps_stopped();
    }

    // Whether a STARTING service can immediately transition to STOPPED (as opposed to
    // having to wait for it reach STARTED and then go through STOPPING).
    virtual bool can_interrupt_start() noexcept override
    {
        return waiting_for_deps;
    }

    virtual void interrupt_start() noexcept override
    {

    }

    void started() noexcept
    {
        service_record::started();
    }

    void failed_to_start() noexcept
    {
        service_record::failed_to_start();
    }
};
