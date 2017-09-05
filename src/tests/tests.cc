#include <cassert>
#include <iostream>

#include "service.h"
#include "test_service.h"

// Test 1: starting a service starts dependencies; stopping the service releases and
// stops dependencies.
void test1()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type::INTERNAL, {}, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type::INTERNAL, {s1}, {});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type::INTERNAL, {s2}, {});
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

    service_record *s1 = new service_record(&sset, "test-service-1", service_type::INTERNAL, {}, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type::INTERNAL, {s1}, {});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type::INTERNAL, {s2}, {});
    service_record *s4 = new service_record(&sset, "test-service-4", service_type::INTERNAL, {s2}, {});
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

    service_record *s1 = new service_record(&sset, "test-service-1", service_type::INTERNAL, {}, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type::INTERNAL, {s1}, {});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type::INTERNAL, {s2}, {});
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

    service_record *s1 = new service_record(&sset, "test-service-1", service_type::INTERNAL, {}, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type::INTERNAL, {s1}, {});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type::INTERNAL, {s2}, {});
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

    test_service *s1 = new test_service(&sset, "test-service-1", service_type::INTERNAL, {}, {});
    test_service *s2 = new test_service(&sset, "test-service-2", service_type::INTERNAL, {s1}, {});
    test_service *s3 = new test_service(&sset, "test-service-3", service_type::INTERNAL, {s2}, {});

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

// Test 6: service pinned in start state is not stopped when its dependency stops.
void test6()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type::INTERNAL, {}, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type::INTERNAL, {s1}, {});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type::INTERNAL, {s2}, {});
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

    // s3 should remain started:
    assert(s3->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STOPPING);
    assert(s1->get_state() == service_state_t::STARTED);
}

// Test 7: stopping a soft dependency doesn't cause the dependent to stop.
void test7()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type::INTERNAL, {}, {});
    service_record *s2 = new service_record(&sset, "test-service-2", service_type::INTERNAL, {s1}, {});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type::INTERNAL, {}, {s2});
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


int main(int argc, char **argv)
{
    std::cout << "test1... ";
    test1();
    std::cout << "PASSED" << std::endl;

    std::cout << "test2... ";
    test2();
    std::cout << "PASSED" << std::endl;

    std::cout << "test3... ";
    test3();
    std::cout << "PASSED" << std::endl;

    std::cout << "test4... ";
    test4();
    std::cout << "PASSED" << std::endl;

    std::cout << "test5... ";
    test5();
    std::cout << "PASSED" << std::endl;

    std::cout << "test6... ";
    test6();
    std::cout << "PASSED" << std::endl;

    std::cout << "test7... ";
    test7();
    std::cout << "PASSED" << std::endl;
}
