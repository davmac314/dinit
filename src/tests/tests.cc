#include <cassert>
#include <iostream>

#include "service.h"

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
}
