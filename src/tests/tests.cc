#include <cassert>
#include <iostream>

#include "service.h"
#include "test_service.h"

constexpr static auto REG = dependency_type::REGULAR;
constexpr static auto WAITS = dependency_type::WAITS_FOR;
constexpr static auto MS = dependency_type::MILESTONE;

// Test 1: starting a service starts dependencies; stopping the service releases and
// stops dependencies.
void test1()
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
}

// Test 2: Multiple dependents will hold a dependency active if one of the dependents is
// stopped/released.
void test2()
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
}

// Test 3: stopping a dependency causes its dependents to stop.
void test3()
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
}

// Test 4: an explicitly activated service with automatic restart will restart if it
// stops due to a dependency stopping, therefore also causing the dependency to restart.
void test4()
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

    // Now stop s1, which should also force s2 and s3 to stop.
    // s2 (and therefore s1) should restart:
    sset.stop_service(s1);

    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);
}

// Test 5: test that services which do not start immediately correctly chain start of
// dependent services.
void test5()
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
    assert(s1->get_state() == service_state_t::STOPPING);

    // If we now unpin, s3 should stop:
    s3->unpin();
    sset.process_queues();
    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s1->get_state() == service_state_t::STOPPED);
}

// Test that STOPPING dependency of pinned service returns to STARTED when the pinned service has
// a start issued.
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

    // s3 should remain started due to pin, but s1 and s2 are released and go STOPPING:
    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STOPPING);
    assert(s1->get_state() == service_state_t::STOPPING);

    // If we now issue start, STOPPING should revert to STARTED:
    s3->start(true);
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s1->get_state() == service_state_t::STARTED);
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

    // s3 should remain started due to pin, but s1 and s2 are released and go STOPPING:
    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STOPPING);
    assert(s1->get_state() == service_state_t::STOPPING);

    // If we now issue start, s2 still needs to stop (due to force stop):
    s3->start(true);
    sset.process_queues();

    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STOPPING);
    assert(s1->get_state() == service_state_t::STARTED);

    // When we unpin, s2 should STOP; s3 must stop as a result; s1 is released and so also stops:
    s3->unpin();

    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s1->get_state() == service_state_t::STOPPED);
}

// Test that service pinned started is released when stop issued and stops when unpinned
void test_pin4()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    sset.add_service(s1);

    // Pin s3:
    s1->pin_start();

    // Start the service:
    sset.start_service(s1);

    assert(s1->get_state() == service_state_t::STARTED);

    // Issue stop:
    s1->stop(true);
    sset.process_queues();

    // s3 should remain started:
    assert(s1->get_state() == service_state_t::STARTED);

    // If we now unpin, s1 should stop:
    s1->unpin();
    sset.process_queues();
    assert(s1->get_state() == service_state_t::STOPPED);
}

// Test 7: stopping a soft dependency doesn't cause the dependent to stop.
void test7()
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
}

// Test 8: stopping a milestone dependency doesn't cause the dependent to stop
void test8()
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
}

// Test 9: a failing milestone dependency causes the dependent to fail
void test9()
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

    assert(s2->get_state() == service_state_t::STOPPED);
}

// Test 10: if start cancelled, remove from console queue
void test10()
{
    service_set sset;

    // Create s1 and s2. s2 depends on s1, and starts on the console.
    test_service *s1 = new test_service(&sset, "test-service-1", service_type_t::INTERNAL, {});
    test_service *s2 = new test_service(&sset, "test-service-2", service_type_t::INTERNAL, {{s1, REG}});
    onstart_flags_t s2_flags;
    s2_flags.starts_on_console = true;
    s2->set_flags(s2_flags);
    sset.add_service(s1);
    sset.add_service(s2);

    // Create s3, which starts and runs on console:
    test_service *s3 = new test_service(&sset, "test-service-3", service_type_t::INTERNAL, {});
    onstart_flags_t s3_flags;
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
}

// Test 11: if a milestone dependency doesn't start, dependent doesn't start.
void test11()
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
}

// Test that active service count reaches 0 when stopping a service with different types of dependency
void test12()
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

#define RUN_TEST(name) \
    std::cout << #name "... "; \
    name(); \
    std::cout << "PASSED" << std::endl;

int main(int argc, char **argv)
{
    RUN_TEST(test1);
    RUN_TEST(test2);
    RUN_TEST(test3);
    RUN_TEST(test4);
    RUN_TEST(test5);
    RUN_TEST(test_pin1);
    RUN_TEST(test_pin2);
    RUN_TEST(test_pin3);
    RUN_TEST(test_pin4);
    RUN_TEST(test7);
    RUN_TEST(test8);
    RUN_TEST(test9);
    RUN_TEST(test10);
    RUN_TEST(test11);
    RUN_TEST(test12);
}
