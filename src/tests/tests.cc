#include <iostream>

#include <cerrno>
#include <cassert>

#include "service.h"
#include "test_service.h"
#include "baseproc-sys.h"

constexpr static auto REG = dependency_type::REGULAR;
constexpr static auto WAITS = dependency_type::WAITS_FOR;
constexpr static auto MS = dependency_type::MILESTONE;
constexpr static auto BEFORE = dependency_type::BEFORE;
constexpr static auto AFTER = dependency_type::AFTER;

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

// Starting a service starts dependencies; stopping the service releases and
// stops dependencies.
void basic_test1()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, REG}});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL, {{s2, REG}});
    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);

    assert(sset.find_service("test-service-1") == s1);
    assert(sset.find_service("test-service-2") == s2);
    assert(sset.find_service("test-service-3") == s3);

    // s3 depends on s2, which depends on s1. So starting s3 should start all three services:
    sset.start_service(s3);

    assert(s1->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s3->get_state() == service_state_t::STARTED);

    // stopping s3 should release the other two services:
    sset.stop_service(s3);

    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s1->get_state() == service_state_t::STOPPED);
    assert(sset.count_active_services() == 0);
}

// Multiple dependents will hold a dependency active if one of the dependents is
// stopped/released.
void basic_test2()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, REG}});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL, {{s2, REG}});
    service_record *s4 = new service_record(&sset, "test-service-4", service_type_t::INTERNAL, {{s2, REG}});
    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);
    sset.add_service(s4);

    // s3 depends on s2, which depends on s1. Similarly with s4. After starting both, all services
    // should be started:
    sset.start_service(s3);
    sset.start_service(s4);

    assert(s1->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s3->get_state() == service_state_t::STARTED);
    assert(s4->get_state() == service_state_t::STARTED);

    // after stopping s3, s4 should hold the other two services:
    sset.stop_service(s3);

    assert(s4->get_state() == service_state_t::STARTED);
    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);

    // Now if we stop s4, s2 and s1 should also be released:
    sset.stop_service(s4);

    assert(s4->get_state() == service_state_t::STOPPED);
    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s1->get_state() == service_state_t::STOPPED);
    assert(sset.count_active_services() == 0);
}

// Stopping a dependency causes its dependents to stop.
void basic_test3()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, REG}});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL, {{s2, REG}});
    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);

    assert(sset.find_service("test-service-1") == s1);
    assert(sset.find_service("test-service-2") == s2);
    assert(sset.find_service("test-service-3") == s3);

    // Start all three services:
    sset.start_service(s3);

    // Now stop s1, which should also force s2 and s3 to stop:
    sset.stop_service(s1);

    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s1->get_state() == service_state_t::STOPPED);
    assert(sset.count_active_services() == 0);
}

// An explicitly activated service with automatic restart will restart if it
// stops due to a dependency stopping, therefore also causing the dependency to restart.
void basic_test4()
{
    service_set sset;

    test_service *s1 = new test_service(&sset, "test-service-1", service_type_t::INTERNAL, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, REG}});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL, {{s2, REG}});
    s2->set_auto_restart(true);
    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);

    assert(sset.find_service("test-service-1") == s1);
    assert(sset.find_service("test-service-2") == s2);
    assert(sset.find_service("test-service-3") == s3);

    // Start all three services:
    sset.start_service(s3);

    // Also explicitly activate s2:
    sset.start_service(s2);

    s1->started();
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);

    // Now stop s1, which should also force s2 and s3 to stop.
    // s2 (and therefore s1) should restart:
    s1->forced_stop();
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STARTING);
    assert(s1->get_state() == service_state_t::STARTING);

    s1->started();
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);
    assert(sset.count_active_services() == 2);
}

// Test that services which do not start immediately correctly chain start of
// dependent services.
void basic_test5()
{
    service_set sset;

    test_service *s1 = new test_service(&sset, "test-service-1", service_type_t::INTERNAL, {});
    test_service *s2 = new test_service(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, REG}});
    test_service *s3 = new test_service(&sset, "test-service-3", service_type_t::INTERNAL, {{s2, REG}});

    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);

    sset.start_service(s3);

    // All three should transition to STARTING state:
    assert(s3->get_state() == service_state_t::STARTING);
    assert(s2->get_state() == service_state_t::STARTING);
    assert(s1->get_state() == service_state_t::STARTING);

    s1->started();
    sset.process_queues();
    assert(s3->get_state() == service_state_t::STARTING);
    assert(s2->get_state() == service_state_t::STARTING);
    assert(s1->get_state() == service_state_t::STARTED);

    s2->started();
    sset.process_queues();
    assert(s3->get_state() == service_state_t::STARTING);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);

    s3->started();
    sset.process_queues();
    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);
    assert(sset.count_active_services() == 3);
}

// Test that issuing a stop-without-bring-down to a service that is held up by a dependent does not
// cause the service to stop.
void basic_test6()
{
    service_set sset;

    test_service *s1 = new test_service(&sset, "test-service-1", service_type_t::INTERNAL, {});
    test_service *s2 = new test_service(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, REG}});
    test_service *s3 = new test_service(&sset, "test-service-3", service_type_t::INTERNAL, {{s2, REG}});

    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);

    sset.start_service(s3);

    // All three should transition to STARTING state:
    assert(s3->get_state() == service_state_t::STARTING);
    s1->started();
    sset.process_queues();
    assert(s2->get_state() == service_state_t::STARTING);
    s2->started();
    sset.process_queues();
    assert(s3->get_state() == service_state_t::STARTING);
    s3->started();
    sset.process_queues();
    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);

    assert(s3->get_target_state() == service_state_t::STARTED);
    assert(s2->get_target_state() == service_state_t::STARTED);
    assert(s1->get_target_state() == service_state_t::STARTED);

    // Mark s2 active (it's already started):
    s2->start();
    sset.process_queues();

    // Issue stop, *without* bring-down, to s1 and s2.
    s1->stop(false);
    sset.process_queues();
    assert(s1->get_state() == service_state_t::STARTED);

    s2->stop(false);
    sset.process_queues();
    assert(s2->get_state() == service_state_t::STARTED);

    assert(s3->get_target_state() == service_state_t::STARTED);
    assert(s2->get_target_state() == service_state_t::STARTED);
    assert(s1->get_target_state() == service_state_t::STARTED);
    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);

    assert(sset.count_active_services() == 3);
}

// An explicitly activated service with automatic restart will not restart if it
// stops due to a dependency stopping from a manual stop (which inhibits restart).
void basic_test7()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, REG}});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL, {{s2, REG}});
    s2->set_auto_restart(true);
    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);

    assert(sset.find_service("test-service-1") == s1);
    assert(sset.find_service("test-service-2") == s2);
    assert(sset.find_service("test-service-3") == s3);

    // Start all three services:
    sset.start_service(s3);

    // Also explicitly activate s2:
    sset.start_service(s2);

    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);

    // Now stop s1, which should also force s2 and s3 to stop.
    // s2 should not restart, since this was an explicit forced stop which
    // inhibits restart, and so s1 should also remain stopped.
    sset.stop_service(s1);

    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s1->get_state() == service_state_t::STOPPED);
    assert(sset.count_active_services() == 0);
}

// A dependent service which stops due to a dependency should have target state STOPPED
// if it won't restart.
void basic_test8()
{
    service_set sset;

    test_service *s1 = new test_service(&sset, "test-service-1", service_type_t::INTERNAL, {});
    test_service *s2 = new test_service(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, REG}});
    test_service *s3 = new test_service(&sset, "test-service-3", service_type_t::INTERNAL, {{s2, REG}});
    s2->auto_stop = false;
    s3->auto_stop = false;
    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);

    assert(sset.find_service("test-service-1") == s1);
    assert(sset.find_service("test-service-2") == s2);
    assert(sset.find_service("test-service-3") == s3);

    // Start all three services:
    sset.start_service(s3);

    // Also explicitly activate s2:
    sset.start_service(s2);

    s1->started();
    sset.process_queues();
    s2->started();
    sset.process_queues();
    s3->started();
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);

    // Now stop s1, which should also force s2 and s3 to stop.
    // s2 (and therefore s1) should restart:
    s1->forced_stop();
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STOPPING);
    assert(s2->get_state() == service_state_t::STOPPING);
    assert(s1->get_state() == service_state_t::STOPPING);

    assert(s3->get_target_state() == service_state_t::STOPPED);
    assert(s2->get_target_state() == service_state_t::STOPPED);

    s3->stopped();
    sset.process_queues();
    s2->stopped();
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s1->get_state() == service_state_t::STOPPED);
    assert(sset.count_active_services() == 0);
}

// Test that service pinned in start state is not stopped when its dependency stops.
void test_pin1()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, REG}});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL, {{s2, REG}});
    s2->set_auto_restart(true);
    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);

    // Pin s3:
    s3->pin_start();

    // Start all three services:
    sset.start_service(s3);

    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);

    // Stop s2:
    s2->forced_stop();
    s2->stop(true);
    sset.process_queues();

    // s3 should remain started due to pin:
    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STOPPING);
    assert(s1->get_state() == service_state_t::STARTED);

    // If we now unpin, s3 should stop:
    s3->unpin();
    sset.process_queues();
    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s1->get_state() == service_state_t::STOPPED);
    assert(sset.count_active_services() == 0);
}

// Test that issuing a stop to a pinned-started service does not stop the service or its dependencies.
void test_pin2()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, REG}});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL, {{s2, REG}});
    s2->set_auto_restart(true);
    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);

    // Pin s3:
    s3->pin_start();

    // Start all three services:
    sset.start_service(s3);

    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);

    // Issue stop to s3:
    s3->stop(true);
    sset.process_queues();

    // s3 should remain started due to pin, s1 and s2 not released:
    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);
    assert(sset.count_active_services() == 3);
}

// Test that a STOPPING dependency of a pinned service stops when pin is released, even if pinned
// service is activated:
void test_pin3()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, REG}});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL, {{s2, REG}});
    s2->set_auto_restart(true);
    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);

    // Pin s3:
    s3->pin_start();

    // Start all three services:
    sset.start_service(s3);

    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);

    // Issue force stop to s2:
    s2->stop(true);
    s2->forced_stop();
    sset.process_queues();

    // s3 should remain started due to pin, but s2 should now be STOPPING:
    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STOPPING);
    assert(s1->get_state() == service_state_t::STARTED);

    // If we now issue start, s2 still needs to stop (due to force stop):
    s3->start();
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STOPPING);
    assert(s1->get_state() == service_state_t::STARTED);

    // When we unpin, s2 should STOP; s3 must stop as a result; s1 is released and so also stops:
    s3->unpin();

    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s1->get_state() == service_state_t::STOPPED);
    assert(sset.count_active_services() == 0);
}

// Test that service pinned started is released when stop issued and stops when unpinned
void test_pin4()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    sset.add_service(s1);

    // Pin s1:
    s1->pin_start();

    // Start the service:
    sset.start_service(s1);

    assert(s1->get_state() == service_state_t::STARTED);

    // Issue forced stop:
    s1->stop(true);
    s1->forced_stop();
    sset.process_queues();

    // s3 should remain started:
    assert(s1->get_state() == service_state_t::STARTED);

    // If we now unpin, s1 should stop:
    s1->unpin();
    sset.process_queues();
    assert(s1->get_state() == service_state_t::STOPPED);
    assert(sset.count_active_services() == 0);
}

// Test that a pinned-started service doesn't stop when released by a dependent
void test_pin5()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, REG}});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL, {{s2, REG}});
    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);

    // Pin s2:
    s2->pin_start();

    // Start all three services:
    sset.start_service(s3);

    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);

    // Issue stop to s3:
    s3->stop(true);
    sset.process_queues();

    // s2 should remain started due to pin (and s1 via dependency), s3 should stop
    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);

    // If we unpin s2, it should stop:
    s2->unpin();
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s1->get_state() == service_state_t::STOPPED);
    assert(sset.count_active_services() == 0);
}

// Test that unpinning a service has no effect on soft dependencies
void test_pin6()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, WAITS}});
    sset.add_service(s1);
    sset.add_service(s2);

    // Pin s2:
    s2->pin_start();

    // Start both three services:
    sset.start_service(s2);

    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);

    // Unpin:
    s2->unpin();

    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);
    assert(sset.count_active_services() == 2);
}

// Test that service pinned stopped does not start when unpinned (does not get marked active)
void test_pin7()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    sset.add_service(s1);

    // Pin stopped:
    s1->pin_stop();

    // Request service start:
    sset.start_service(s1);

    assert(s1->get_state() == service_state_t::STOPPED);

    // Unpin:
    s1->unpin();

    // Service should remain stopped
    assert(s1->get_state() == service_state_t::STOPPED);
    assert(sset.count_active_services() == 0);
}

// Test that dependents of a pinned-started service are unaffected by a transitive stop issued to the
// pinned service
void test_pin8()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, REG}});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL, {{s2, REG}});
    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);

    // Pin s3:
    s2->pin_start();

    // Start all three services:
    sset.start_service(s3);

    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);

    // Issue stop to s1:
    s1->stop(true);
    sset.process_queues();

    // s2 should remain started due to pin, s1 stopping, s3 remains started:
    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STOPPING);
    assert(sset.count_active_services() == 3);
}

// test that a pinned-stopped service is not started via a soft dependency once unpinned
void test_pin9()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, WAITS}});
    sset.add_service(s1);
    sset.add_service(s2);

    // Pin s1:
    s1->pin_stop();

    // Start s2:
    sset.start_service(s2);

    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STOPPED);

    // release pin, service should not start:
    s1->unpin();

    // s2 should remain started, s1 should remain stopped:
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STOPPED);
    assert(sset.count_active_services() == 1);
}

// test that starting a service with a stop-pinned dependency fails
void test_pin10()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, REG}});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL, {{s2, REG}});
    s2->set_auto_restart(true);
    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);

    // Pin-stop s2:
    s2->pin_stop();

    // Try to start all three services:
    sset.start_service(s3);

    // Start of s3 should fail due to s2
    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s1->get_state() == service_state_t::STOPPED);

    assert(sset.count_active_services() == 0);
}

// Stopping a soft dependency doesn't cause the dependent to stop.
void test_softdep1()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, REG}});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL, {{s2, WAITS}});
    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);

    assert(sset.find_service("test-service-1") == s1);
    assert(sset.find_service("test-service-2") == s2);
    assert(sset.find_service("test-service-3") == s3);

    // Start all three services:
    sset.start_service(s3);

    // Now stop s1, which should also force s2 but not s3 to stop:
    sset.stop_service(s1);

    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s1->get_state() == service_state_t::STOPPED);
    assert(sset.count_active_services() == 1);
}

// Stopping a milestone dependency doesn't cause the dependent to stop
void test_softdep2()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, MS}});
    sset.add_service(s1);
    sset.add_service(s2);

    assert(sset.find_service("test-service-1") == s1);
    assert(sset.find_service("test-service-2") == s2);

    // Start the services:
    sset.start_service(s2);

    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);

    // Now stop s1, which should not stop s2:
    sset.stop_service(s1);

    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STOPPED);
    assert(sset.count_active_services() == 1);
}

// A failing milestone dependency causes the dependent to fail
void test_softdep3()
{
    service_set sset;

    test_service *s1 = new test_service(&sset, "test-service-1", service_type_t::INTERNAL, {});
    test_service *s2 = new test_service(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, MS}});
    sset.add_service(s1);
    sset.add_service(s2);

    assert(sset.find_service("test-service-1") == s1);
    assert(sset.find_service("test-service-2") == s2);

    // Start the services, but fail s1:
    sset.start_service(s2);

    assert(s1->get_state() == service_state_t::STARTING);
    s1->failed_to_start();
    sset.process_queues();

    assert(s1->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(sset.count_active_services() == 0);
}

// If a milestone dependency start is cancelled, dependent doesn't start.
void test_softdep4()
{
    service_set sset;

    test_service *s1 = new test_service(&sset, "test-service-1", service_type_t::INTERNAL, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, MS}});

    sset.add_service(s1);
    sset.add_service(s2);

    assert(sset.find_service("test-service-1") == s1);
    assert(sset.find_service("test-service-2") == s2);

    // Request start of the s2 service:
    sset.start_service(s2);
    sset.process_queues();

    assert(s1->get_state() == service_state_t::STARTING);
    assert(s2->get_state() == service_state_t::STARTING);

    s1->stop();
    sset.process_queues();
    s1->bring_down();
    sset.process_queues();

    assert(s1->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(sset.count_active_services() == 0);
}

// Test that soft dependents of a service reattach when service starts
void test_softdep5()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, WAITS}});
    sset.add_service(s1);
    sset.add_service(s2);

    assert(sset.find_service("test-service-1") == s1);
    assert(sset.find_service("test-service-2") == s2);

    // Start both services:
    sset.start_service(s2);

    // Stop s1:
    sset.stop_service(s1);

    assert(s1->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STARTED);

    // Start s1:
    sset.start_service(s1);

    assert(s1->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);

    // De-activate but don't force bring down of s1
    // It should remain running, as the dependency from s2 should be reattached
    s1->stop(false);
    sset.process_queues();

    assert(s1->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);

    assert(sset.count_active_services() == 2);
}

// Test that already-started dependencies are correctly attached when starting
void test_softdep6()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, WAITS}});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL, {{s1, WAITS}});
    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);

    assert(sset.find_service("test-service-1") == s1);
    assert(sset.find_service("test-service-2") == s2);
    assert(sset.find_service("test-service-3") == s3);

    // Start s1+s2 services:
    sset.start_service(s2);

    // Start s3:
    sset.start_service(s3);

    // Stop s2:
    sset.stop_service(s2);

    // s1 should remain started, due to dependency from s3.
    assert(s1->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s3->get_state() == service_state_t::STARTED);

    // Stop s3, all should stop:
    sset.stop_service(s3);

    assert(s1->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s3->get_state() == service_state_t::STOPPED);

    assert(sset.count_active_services() == 0);
}

// Test that a soft dependency restarts if set to auto-restart and dependent still running
void test_softdep7()
{
    service_set sset;

    test_service *s1 = new test_service(&sset, "test-service-1", service_type_t::INTERNAL, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, WAITS}});
    s1->set_auto_restart(true);
    sset.add_service(s1);
    sset.add_service(s2);

    assert(sset.find_service("test-service-1") == s1);
    assert(sset.find_service("test-service-2") == s2);

    // Start both services:
    sset.start_service(s2);

    assert(s1->get_target_state() == service_state_t::STARTED);
    s1->started();
    sset.process_queues();

    assert(s1->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);

    // Unexpected stop:
    s1->forced_stop();
    sset.process_queues();

    // since s1 will restart, target state should be STARTED:
    assert(s1->get_target_state() == service_state_t::STARTED);

    s1->stopped();
    sset.process_queues();

    // We should see s1 restarting:
    assert(s1->get_state() == service_state_t::STARTING);

    s1->started();
    sset.process_queues();

    assert(s1->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);

    assert(sset.count_active_services() == 2);
}

// Test that a soft dependency doesn't restart if not set to auto-restart and dependent still running
void test_softdep8()
{
    service_set sset;

    test_service *s1 = new test_service(&sset, "test-service-1", service_type_t::INTERNAL, {});
    s1->auto_stop = false;
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, WAITS}});
    sset.add_service(s1);
    sset.add_service(s2);

    assert(sset.find_service("test-service-1") == s1);
    assert(sset.find_service("test-service-2") == s2);

    // Start both services:
    sset.start_service(s2);

    assert(s1->get_target_state() == service_state_t::STARTED);
    s1->started();
    sset.process_queues();

    assert(s1->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(sset.count_active_services() == 2);

    // Unexpected stop:
    s1->forced_stop();
    sset.process_queues();

    // since s1 will not restart, target state should be STOPPED:
    assert(s1->get_target_state() == service_state_t::STOPPED);

    s1->stopped();
    sset.process_queues();

    // We should see s1 remain stopped:
    assert(s1->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STARTED);

    assert(sset.count_active_services() == 1);
}

// If start cancelled, service is removed from console queue
void test_other1()
{
    service_set sset;

    // Create s1 and s2. s2 depends on s1, and starts on the console.
    test_service *s1 = new test_service(&sset, "test-service-1", service_type_t::INTERNAL, {});
    test_service *s2 = new test_service(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, REG}});
    service_flags_t s2_flags;
    s2_flags.starts_on_console = true;
    s2->set_flags(s2_flags);
    sset.add_service(s1);
    sset.add_service(s2);

    // Create s3, which starts and runs on console:
    test_service *s3 = new test_service(&sset, "test-service-3", service_type_t::INTERNAL, {});
    service_flags_t s3_flags;
    s3_flags.starts_on_console = true;
    s3_flags.runs_on_console = true;
    sset.add_service(s3);

    assert(sset.find_service("test-service-1") == s1);
    assert(sset.find_service("test-service-2") == s2);
    assert(sset.find_service("test-service-3") == s3);

    // Start the s3 service, so it gets console:
    sset.start_service(s3);
    sset.process_queues();
    s3->started();
    sset.process_queues();

    assert(! sset.is_queued_for_console(s3)); // should not be queued, because already has acquired
    assert(sset.is_console_queue_empty());

    // Start s2, which starts s1 as a dependency:

    sset.start_service(s2);
    sset.process_queues();

    assert(s1->get_state() == service_state_t::STARTING);
    assert(s2->get_state() == service_state_t::STARTING);

    s1->started();
    sset.process_queues();

    // s2 should now be waiting for console:
    assert(s1->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTING);
    assert(sset.is_queued_for_console(s2));

    // stop s1, should stop s2, s2 should unqueue:
    s1->stop();
    sset.process_queues();

    assert(s1->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(! sset.is_queued_for_console(s2));
    assert(sset.count_active_services() == 1); // s3 is still started
}

// Test that active service count reaches 0 when stopping a service with different types of dependency
void test_other2()
{
    service_set sset;

    service_record *s4 = new service_record(&sset, "test-service-4", service_type_t::INTERNAL, {});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {});

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL,
            {{s2, WAITS}, {s3, REG}, {s4, MS}});

    sset.add_service(s4);
    sset.add_service(s3);
    sset.add_service(s2);
    sset.add_service(s1);

    assert(sset.find_service("test-service-1") == s1);
    assert(sset.find_service("test-service-2") == s2);
    assert(sset.find_service("test-service-3") == s3);
    assert(sset.find_service("test-service-4") == s4);

    // Request start of the s2 service, should also start s1:
    sset.start_service(s1);
    sset.process_queues();

    assert(s1->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s3->get_state() == service_state_t::STARTED);
    assert(s4->get_state() == service_state_t::STARTED);

    s1->stop();
    sset.process_queues();

    assert(s1->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s4->get_state() == service_state_t::STOPPED);

    assert(sset.count_active_services() == 0);
}

// Tests for "restart" functionality.
void test_other3()
{
    service_set sset;

    test_service *s1 = new test_service(&sset, "test-service-1", service_type_t::INTERNAL, {});
    test_service *s2 = new test_service(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, WAITS}});
    test_service *s3 = new test_service(&sset, "test-service-3", service_type_t::INTERNAL, {{s2, REG}});

    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);

    // Start all services via s3
    sset.start_service(s3);
    s1->started();
    sset.process_queues();
    s2->started();
    sset.process_queues();
    s3->started();
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);

    test_listener tl;

    s1->add_listener(&tl);

    s1->restart();
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTING);
    assert(! tl.got_started);

    s1->started();
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);
    assert(tl.got_started);
}

// Make sure a service only restarts once (i.e. restart flag doesn't get stuck)
void test_other4()
{
    service_set sset;

    test_service *s1 = new test_service(&sset, "test-service-1", service_type_t::INTERNAL, {});
    test_service *s2 = new test_service(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, WAITS}});

    sset.add_service(s1);
    sset.add_service(s2);

    // Start all services via s2
    sset.start_service(s2);
    s1->started();
    sset.process_queues();
    s2->started();
    sset.process_queues();

    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);

    s1->restart();
    sset.process_queues();

    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTING);

    s1->started();
    sset.process_queues();

    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);

    // Ok, we restarted s1. Now stop it:

    s1->stop(true);
    sset.process_queues();

    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STOPPED); // didn't restart
}

// Test that restart can be cancelled if dependents stop
void test_other5()
{
    service_set sset;

    test_service *s1 = new test_service(&sset, "test-service-1", service_type_t::INTERNAL, {});
    test_service *s2 = new test_service(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, WAITS}});

    sset.add_service(s1);
    sset.add_service(s2);

    // Start all services via s2
    sset.start_service(s2);
    s1->started();
    sset.process_queues();
    s2->started();
    sset.process_queues();

    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);

    test_listener tl;
    s1->add_listener(&tl);

    s1->auto_stop = false;

    s1->restart();
    sset.process_queues();

    assert(s1->get_state() == service_state_t::STOPPING);
    assert(s1->get_target_state() == service_state_t::STARTED);

    // Stop s2, s1 will be released and will therefore not restart
    s2->stop();
    sset.process_queues();
    assert(s1->get_target_state() == service_state_t::STOPPED);
    assert(tl.start_cancelled);

    s1->stopped();
    sset.process_queues();

    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s1->get_state() == service_state_t::STOPPED);

    assert(! tl.got_started);
}

// Test interrupted startup.
void test_other6()
{
    service_set sset;

    test_service *s1 = new test_service(&sset, "test-service-1", service_type_t::INTERNAL, {});
    test_service *s2 = new test_service(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, WAITS}});
    test_service *s3 = new test_service(&sset, "test-service-3", service_type_t::INTERNAL, {{s2, MS}});

    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);

    // Begin to start all services via s3
    sset.start_service(s3);

    assert(s1->get_state() == service_state_t::STARTING);
    assert(s2->get_state() == service_state_t::STARTING);
    assert(s3->get_state() == service_state_t::STARTING);

    // Stop s2, s3 should stop, s1 is no longer required but still STARTING
    // Note s2 is waiting for dependencies (s1) so start should be interruptible.
    s2->stop();
    sset.process_queues();

    assert(s1->get_state() == service_state_t::STARTING);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s3->get_state() == service_state_t::STOPPED);

    // Once s1 starts, it is no longer required and so should stop.
    s1->started();
    sset.process_queues();

    assert(s1->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s3->get_state() == service_state_t::STOPPED);

    assert(sset.count_active_services() == 0);
}

static void flush_log(int fd)
{
    while (! is_log_flushed()) {
        event_loop.send_fd_event(fd, dasynq::OUT_EVENTS);
        event_loop.send_fd_event(STDOUT_FILENO, dasynq::OUT_EVENTS);
    }

    std::vector<char> wdata;
    bp_sys::extract_written_data(fd, wdata);
    bp_sys::extract_written_data(0, wdata);
}

void test_log1()
{
    // Basic test that output to log is written to log file
    service_set sset;
    init_log(true /* syslog format */);
    setup_log_console_handoff(&sset);

    int logfd = bp_sys::allocfd();
    setup_main_log(logfd);

    flush_log(logfd);

    log(loglevel_t::ERROR, "test one");

    // flush
    //event_loop.
    event_loop.send_fd_event(logfd, dasynq::OUT_EVENTS);

    std::vector<char> wdata;
    bp_sys::extract_written_data(logfd, wdata);

    std::string wstr {wdata.begin(), wdata.end()};

    assert(wstr == "<27>dinit: test one\n");
    close_log();
}

void test_log2()
{
    // test that log is closed on write failure.
    service_set sset;
    init_log(true /* syslog format */);
    setup_log_console_handoff(&sset);

    bool was_closed = false;

    class fail_writer : public bp_sys::write_handler {
    public:
        bool *was_closed = nullptr;

        ssize_t write(int fd, const void *buf, size_t count) override
        {
            errno = ENOSPC;
            return -1;
        }

        ~fail_writer() override
        {
            *was_closed = true;
        }
    };

    fail_writer *fw = new fail_writer();
    fw->was_closed = &was_closed;

    int logfd = bp_sys::allocfd(fw);
    setup_main_log(logfd);

    event_loop.send_fd_event(logfd, dasynq::OUT_EVENTS);
    event_loop.send_fd_event(STDOUT_FILENO, dasynq::OUT_EVENTS);

    log(loglevel_t::ERROR, "test two");

    // flush
    //event_loop.
    event_loop.send_fd_event(logfd, dasynq::OUT_EVENTS);

    assert(was_closed);
    close_log();
}

// Test ordering is honoured (when services would start anyway)
void test_order1()
{
    service_set sset;

    test_service *s1 = new test_service(&sset, "test-service-1", service_type_t::INTERNAL, {});
    test_service *s2 = new test_service(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, BEFORE}});
    test_service *s3 = new test_service(&sset, "test-service-3", service_type_t::INTERNAL, {{s2, REG}, {s1, REG}});
    s1->auto_stop = false;
    s2->auto_stop = false;
    s3->auto_stop = false;
    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);

    assert(sset.find_service("test-service-1") == s1);
    assert(sset.find_service("test-service-2") == s2);
    assert(sset.find_service("test-service-3") == s3);

    // Start all three services:
    sset.start_service(s3);

    assert(s1->bring_up_reqd == true);
    assert(s2->bring_up_reqd == false);
    assert(s3->bring_up_reqd == false);

    s1->started();
    sset.process_queues();

    assert(s2->bring_up_reqd == true);
    assert(s3->bring_up_reqd == false);

    s2->started();
    sset.process_queues();

    assert(s3->bring_up_reqd == true);

    s3->started();
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);

    // Now stop s3, which should also cause s1 and s2 to stop.
    s3->stop();
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STOPPING);
    assert(s2->get_state() == service_state_t::STOPPING);
    assert(s1->get_state() == service_state_t::STOPPING);

    assert(s3->get_target_state() == service_state_t::STOPPED);
    assert(s2->get_target_state() == service_state_t::STOPPED);
    assert(s1->get_target_state() == service_state_t::STOPPED);

    s3->stopped();
    sset.process_queues();
    s2->stopped();
    sset.process_queues();
    s1->stopped();
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s1->get_state() == service_state_t::STOPPED);
    assert(sset.count_active_services() == 0);
}

// Test "before" ordering does not by itself imply bring-up requirement
void test_order2()
{
    service_set sset;

    test_service *s1 = new test_service(&sset, "test-service-1", service_type_t::INTERNAL, {});
    test_service *s2 = new test_service(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, BEFORE}});
    test_service *s3 = new test_service(&sset, "test-service-3", service_type_t::INTERNAL, {{s2, REG}});
    s1->auto_stop = false;
    s2->auto_stop = false;
    s3->auto_stop = false;
    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);

    assert(sset.find_service("test-service-1") == s1);
    assert(sset.find_service("test-service-2") == s2);
    assert(sset.find_service("test-service-3") == s3);

    // Start s3 and s2; s1 should not be started
    sset.start_service(s3);

    assert(s1->bring_up_reqd == false);
    assert(s2->bring_up_reqd == true);
    assert(s3->bring_up_reqd == false);

    s2->started();
    sset.process_queues();

    s3->started();
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STOPPED);

    // Now stop s3, which should also cause ss2 to stop.
    s3->stop();
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STOPPING);
    assert(s2->get_state() == service_state_t::STOPPING);
    assert(s1->get_state() == service_state_t::STOPPED);

    assert(s3->get_target_state() == service_state_t::STOPPED);
    assert(s2->get_target_state() == service_state_t::STOPPED);
    assert(s1->get_target_state() == service_state_t::STOPPED);

    s3->stopped();
    sset.process_queues();
    s2->stopped();
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s1->get_state() == service_state_t::STOPPED);
    assert(sset.count_active_services() == 0);
}

// Test "after" ordering does not by itself imply bring-up requirement
void test_order3()
{
    service_set sset;

    test_service *s1 = new test_service(&sset, "test-service-1", service_type_t::INTERNAL, {});
    test_service *s2 = new test_service(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, AFTER}});
    test_service *s3 = new test_service(&sset, "test-service-3", service_type_t::INTERNAL, {{s2, REG}});
    s1->auto_stop = false;
    s2->auto_stop = false;
    s3->auto_stop = false;
    sset.add_service(s1);
    sset.add_service(s2);
    sset.add_service(s3);

    assert(sset.find_service("test-service-1") == s1);
    assert(sset.find_service("test-service-2") == s2);
    assert(sset.find_service("test-service-3") == s3);

    // Start s3 and s2; s1 should not be started
    sset.start_service(s3);

    assert(s1->bring_up_reqd == false);
    assert(s2->bring_up_reqd == true);
    assert(s3->bring_up_reqd == false);

    s2->started();
    sset.process_queues();

    s3->started();
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STOPPED);

    // Now stop s3, which should also cause ss2 to stop.
    s3->stop();
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STOPPING);
    assert(s2->get_state() == service_state_t::STOPPING);
    assert(s1->get_state() == service_state_t::STOPPED);

    assert(s3->get_target_state() == service_state_t::STOPPED);
    assert(s2->get_target_state() == service_state_t::STOPPED);
    assert(s1->get_target_state() == service_state_t::STOPPED);

    s3->stopped();
    sset.process_queues();
    s2->stopped();
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s1->get_state() == service_state_t::STOPPED);
    assert(sset.count_active_services() == 0);
}

#define RUN_TEST(name, spacing) \
    std::cout << #name "..." spacing << std::flush; \
    name(); \
    std::cout << "PASSED" << std::endl;

int main(int argc, char **argv)
{
    bp_sys::init_bpsys();

    RUN_TEST(basic_test1, "               ");
    RUN_TEST(basic_test2, "               ");
    RUN_TEST(basic_test3, "               ");
    RUN_TEST(basic_test4, "               ");
    RUN_TEST(basic_test5, "               ");
    RUN_TEST(basic_test6, "               ");
    RUN_TEST(basic_test7, "               ");
    RUN_TEST(basic_test8, "               ");
    RUN_TEST(test_pin1, "                 ");
    RUN_TEST(test_pin2, "                 ");
    RUN_TEST(test_pin3, "                 ");
    RUN_TEST(test_pin4, "                 ");
    RUN_TEST(test_pin5, "                 ");
    RUN_TEST(test_pin6, "                 ");
    RUN_TEST(test_pin7, "                 ");
    RUN_TEST(test_pin8, "                 ");
    RUN_TEST(test_pin9, "                 ");
    RUN_TEST(test_pin10, "                ");
    RUN_TEST(test_softdep1, "             ");
    RUN_TEST(test_softdep2, "             ");
    RUN_TEST(test_softdep3, "             ");
    RUN_TEST(test_softdep4, "             ");
    RUN_TEST(test_softdep5, "             ");
    RUN_TEST(test_softdep6, "             ");
    RUN_TEST(test_softdep7, "             ");
    RUN_TEST(test_softdep8, "             ");
    RUN_TEST(test_other1, "               ");
    RUN_TEST(test_other2, "               ");
    RUN_TEST(test_other3, "               ");
    RUN_TEST(test_other4, "               ");
    RUN_TEST(test_other5, "               ");
    RUN_TEST(test_other6, "               ");
    RUN_TEST(test_log1, "                 ");
    RUN_TEST(test_log2, "                 ");
    RUN_TEST(test_order1, "               ");
    RUN_TEST(test_order2, "               ");
    RUN_TEST(test_order3, "               ");
}
