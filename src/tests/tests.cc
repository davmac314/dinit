#include <cassert>
#include "service.h"

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

    assert(s1->getState() == service_state_t::STARTED);
    assert(s2->getState() == service_state_t::STARTED);
    assert(s3->getState() == service_state_t::STARTED);

    // stopping s3 should release the other two services:
    sset.stop_service(s3);

    assert(s3->getState() == service_state_t::STOPPED);
    assert(s2->getState() == service_state_t::STOPPED);
    assert(s1->getState() == service_state_t::STOPPED);
}

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

    assert(s1->getState() == service_state_t::STARTED);
    assert(s2->getState() == service_state_t::STARTED);
    assert(s3->getState() == service_state_t::STARTED);
    assert(s4->getState() == service_state_t::STARTED);

    // after stopping s3, s4 should hold the other two services:
    sset.stop_service(s3);

    assert(s4->getState() == service_state_t::STARTED);
    assert(s3->getState() == service_state_t::STOPPED);
    assert(s2->getState() == service_state_t::STARTED);
    assert(s1->getState() == service_state_t::STARTED);

    // Now if we stop s4, s2 and s1 should also be released:
    sset.stop_service(s4);

    assert(s4->getState() == service_state_t::STOPPED);
    assert(s3->getState() == service_state_t::STOPPED);
    assert(s2->getState() == service_state_t::STOPPED);
    assert(s1->getState() == service_state_t::STOPPED);
}

int main(int argc, char **argv)
{
    test1();
    test2();
}
