#include <cassert>
#include <iostream>
#include <list>
#include <utility>
#include <string>

#include "service.h"
#include "proc-service.h"

// Tests of process-service related functionality.
//
// These tests work mostly by completely mocking out the base_process_service class. The mock
// implementations can be found in test-baseproc.cc.

// Friend interface to access base_process_service private/protected members.
class base_process_service_test
{
    public:
    static void exec_succeeded(base_process_service *bsp)
    {
        bsp->exec_succeeded();
    }

    static void handle_exit(base_process_service *bsp, int exit_status)
    {
        bsp->handle_exit_status(exit_status);
    }
};

// Regular service start
void test1()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p = process_service(&sset, "testproc", std::move(command), command_offsets, depends);
    p.start(true);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTED);
}

// Unexpected termination
void test2()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p = process_service(&sset, "testproc", std::move(command), command_offsets, depends);
    p.start(true);

    base_process_service_test::exec_succeeded(&p);

    assert(p.get_state() == service_state_t::STARTED);

    base_process_service_test::handle_exit(&p, 0);

    assert(p.get_state() == service_state_t::STOPPED);
}

// Termination via stop request
void test3()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p = process_service(&sset, "testproc", std::move(command), command_offsets, depends);
    p.start(true);

    base_process_service_test::exec_succeeded(&p);

    assert(p.get_state() == service_state_t::STARTED);

    p.stop(true);

    assert(p.get_state() == service_state_t::STOPPING);

    base_process_service_test::handle_exit(&p, 0);

    assert(p.get_state() == service_state_t::STOPPED);
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
}
