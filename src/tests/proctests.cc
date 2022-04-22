#include <cassert>
#include <iostream>
#include <list>
#include <utility>
#include <string>
#include <sstream>

#include "service.h"
#include "proc-service.h"

// Tests of process-service related functionality.
//
// These tests work mostly by completely mocking out the base_process_service class. The mock
// implementations can be found in test-baseproc.cc.

extern eventloop_t event_loop;

constexpr static auto REG = dependency_type::REGULAR;
constexpr static auto WAITS = dependency_type::WAITS_FOR;

// Friend interface to access base_process_service private/protected members.
class base_process_service_test
{
    public:
    static void exec_succeeded(base_process_service *bsp)
    {
        bsp->waiting_for_execstat = false;
        bsp->exec_succeeded();
    }

    static void exec_failed(base_process_service *bsp, int errcode)
    {
        run_proc_err err;
        err.stage = exec_stage::DO_EXEC;
        err.st_errno = errcode;
    	bsp->waiting_for_execstat = false;
    	bsp->pid = -1;
    	bsp->exec_failed(err);
    }

    static void handle_exit(base_process_service *bsp, int exit_status)
    {
        bsp->pid = -1;
        bsp->handle_exit_status(bp_sys::exit_status(true, false, exit_status));
    }

    static void handle_signal_exit(base_process_service *bsp, int signo)
    {
        bsp->pid = -1;
        bsp->handle_exit_status(bp_sys::exit_status(false, true, signo));
    }

    static void handle_stop_exit(process_service *ps, int exit_status)
    {
        ps->stop_pid = -1;
        ps->waiting_for_execstat = false;
        ps->stop_status = bp_sys::exit_status(true, false, exit_status);
        ps->handle_stop_exit();
    }

    static int get_notification_fd(base_process_service *bsp)
    {
        return bsp->notification_fd;
    }
};

namespace bp_sys {
    // last signal sent:
    extern int last_sig_sent;
    extern pid_t last_forked_pid;
}

static void init_service_defaults(base_process_service &ps)
{
    ps.set_restart_interval(time_val(10,0), 3);
    ps.set_restart_delay(time_val(0, 200000000)); // 200 milliseconds
    ps.set_stop_timeout(time_val(10,0));
}

// Regular service start
void test_proc_service_start()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// Test start with readiness notification
void test_proc_notify_start()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_notification_fd(3);
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    int nfd = base_process_service_test::get_notification_fd(&p);
    assert(nfd > 0);

    char notifystr[] = "ok started\n";
    std::vector<char> rnotifystr;
    rnotifystr.insert(rnotifystr.end(), notifystr, notifystr + sizeof(notifystr));
    bp_sys::supply_read_data(nfd, std::move(rnotifystr));

    event_loop.regd_fd_watchers[nfd]->fd_event(event_loop, nfd, dasynq::IN_EVENTS);

    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// Unexpected termination
void test_proc_unexpected_term()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    sset.add_service(&p);

    service_record d1 {&sset, "test-service-2", service_type_t::INTERNAL, {{&p, REG}}};
    sset.add_service(&d1);

    d1.start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTED);
    assert(p.get_target_state() == service_state_t::STARTED);
    assert(d1.get_state() == service_state_t::STARTED);
    assert(d1.get_target_state() == service_state_t::STARTED);

    base_process_service_test::handle_exit(&p, 0);

    assert(p.get_target_state() == service_state_t::STOPPED);
    assert(p.get_state() == service_state_t::STOPPED);
    assert(p.get_stop_reason() == stopped_reason_t::TERMINATED);
    assert(event_loop.active_timers.size() == 0);

    assert(d1.get_state() == service_state_t::STOPPED);
    assert(d1.get_target_state() == service_state_t::STOPPED);

    sset.remove_service(&d1);
    sset.remove_service(&p);
}

// Unexpected termination with restart
void test_proc_term_restart()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_auto_restart(true);
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    pid_t first_pid = bp_sys::last_forked_pid;

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    // Starting, restart timer should be armed:
    assert(p.get_state() == service_state_t::STARTING);
    assert(event_loop.active_timers.size() == 1);
    assert(bp_sys::last_forked_pid == first_pid);

    event_loop.advance_time(time_val(0, 200000000));
    assert(event_loop.active_timers.size() == 0);

    sset.process_queues();

    assert(bp_sys::last_forked_pid == (first_pid + 1));

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// Unexpected termination with restart, with dependent
void test_proc_term_restart2()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    service_record b {&sset, "boot"};
    sset.add_service(&b);

    process_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_auto_restart(true);
    sset.add_service(&p);

    b.add_dep(&p, WAITS);

    pid_t first_pid = bp_sys::last_forked_pid;

    b.start();
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);
    assert(bp_sys::last_forked_pid == first_pid + 1);

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);
    assert(bp_sys::last_forked_pid == first_pid + 1);

    // simulate process terminating, should then be restarted:
    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    // Starting, restart timer should be armed:
    assert(p.get_state() == service_state_t::STARTING);
    assert(event_loop.active_timers.size() == 1);
    assert(bp_sys::last_forked_pid == first_pid + 1);

    event_loop.advance_time(time_val(0, 200000000));
    assert(event_loop.active_timers.size() == 0);

    sset.process_queues();
    assert(bp_sys::last_forked_pid == first_pid + 2);

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    assert(sset.count_active_services() == 2);

    // Request stop, this time it should not restart:
    p.stop(true);
    sset.process_queues();
    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPED);
    assert(event_loop.active_timers.size() == 0);
    assert(sset.count_active_services() == 1);
    assert(bp_sys::last_forked_pid == first_pid + 2);

    // simulate terminate dinit
    sset.stop_all_services();

    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPED);
    assert(event_loop.active_timers.size() == 0);
    assert(sset.count_active_services() == 0);

    sset.remove_service(&p);
    sset.remove_service(&b);
}

// Restart due to dependent, after unexpected termination
void test_proc_term_restart3()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    sset.add_service(&p);

    service_record d1 {&sset, "test-service-2", service_type_t::INTERNAL, {{&p, REG}}};
    d1.set_auto_restart(true);
    sset.add_service(&d1);

    d1.start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTED);
    assert(p.get_target_state() == service_state_t::STARTED);
    assert(d1.get_state() == service_state_t::STARTED);
    assert(d1.get_target_state() == service_state_t::STARTED);

    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    assert(p.get_target_state() == service_state_t::STARTED);
    assert(p.get_state() == service_state_t::STARTING);
    assert(p.get_stop_reason() == stopped_reason_t::TERMINATED);

    assert(d1.get_state() == service_state_t::STARTING);
    assert(d1.get_target_state() == service_state_t::STARTED);

    sset.remove_service(&d1);
    sset.remove_service(&p);
    event_loop.active_timers.clear();
}

// Termination via stop request
void test_term_via_stop()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    p.stop(true);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPING);
    assert(event_loop.active_timers.size() == 1);

    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPED);
    assert(p.get_stop_reason() == stopped_reason_t::NORMAL);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// Termination via stop request, ensure reason is reset:
void test_term_via_stop2()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    // first set it up with failure reason:

    base_process_service_test::exec_failed(&p, ENOENT);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPED);
    assert(p.get_stop_reason() == stopped_reason_t::EXECFAILED);

    // now restart clean:

    p.start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    p.stop(true);
    sset.process_queues();
    assert(p.get_state() == service_state_t::STOPPING);

    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();
    assert(p.get_state() == service_state_t::STOPPED);
    assert(p.get_stop_reason() == stopped_reason_t::NORMAL);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// stop twice
void test_term_via_stop3()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    p.stop(true);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPING);
    assert(event_loop.active_timers.size() == 1);

    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPED);
    assert(p.get_stop_reason() == stopped_reason_t::NORMAL);
    assert(event_loop.active_timers.size() == 0);

    p.start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    bp_sys::last_sig_sent = 0; // make sure signal is re-sent

    p.stop(true);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPING);
    assert(event_loop.active_timers.size() == 1);

    assert(bp_sys::last_sig_sent == SIGTERM);

    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPED);
    assert(p.get_stop_reason() == stopped_reason_t::NORMAL);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// Time-out during start
void test_proc_start_timeout()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    scripted_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_start_timeout(time_val(10,0));
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    event_loop.advance_time(time_val(10,0));
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPING);

    base_process_service_test::handle_signal_exit(&p, SIGTERM);
    sset.process_queues();

    // We set no stop script, so state should now be STOPPED with no timer set
    assert(p.get_state() == service_state_t::STOPPED);
    assert(p.get_stop_reason() == stopped_reason_t::TIMEDOUT);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// Test that a timeout doesn't stop a "waits for" dependent to fail to start
void test_proc_start_timeout2()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    scripted_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    p.set_start_timeout(time_val {1,0});
    init_service_defaults(p);
    sset.add_service(&p);

    service_record ts {&sset, "test-service-1", service_type_t::INTERNAL,
        {{&p, dependency_type::WAITS_FOR}} };

    ts.start();
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);
    assert(ts.get_state() == service_state_t::STARTING);

    event_loop.advance_time(time_val {1,0}); // start timer should expire
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPING);

    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPED);
    assert(p.get_stop_reason() == stopped_reason_t::TIMEDOUT);
    assert(ts.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// Test exec() failure for process service start.
void test_proc_start_execfail()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::exec_failed(&p, ENOENT);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPED);
    assert(p.get_stop_reason() == stopped_reason_t::EXECFAILED);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// Test no ready notification before process terminates
void test_proc_notify_fail()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_notification_fd(3);
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    int nfd = base_process_service_test::get_notification_fd(&p);
    assert(nfd > 0);

    // Signal EOF on notify fd:
    event_loop.regd_fd_watchers[nfd]->fd_event(event_loop, nfd, dasynq::IN_EVENTS);

    assert(p.get_state() == service_state_t::STOPPING);

    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPED);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// Test stop timeout
void test_proc_stop_timeout()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_stop_timeout(time_val {10, 0});
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTED);

    p.stop(true);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPING);
    assert(bp_sys::last_sig_sent == SIGTERM);

    event_loop.advance_time(time_val {10, 0}); // expire stop timer
    sset.process_queues();

    // kill signal (SIGKILL) should have been sent; process not dead until it's dead, however
    assert(p.get_state() == service_state_t::STOPPING);
    assert(bp_sys::last_sig_sent == SIGKILL);

    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPED);
    assert(p.get_stop_reason() == stopped_reason_t::NORMAL);

    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// Smooth recovery
void test_proc_smooth_recovery1()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_smooth_recovery(true);
    p.set_restart_delay(time_val {0, 1000});
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    pid_t first_instance = bp_sys::last_forked_pid;

    assert(p.get_state() == service_state_t::STARTED);

    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    // since time hasn't been changed, we expect that the process has not yet been re-launched:
    assert(first_instance == bp_sys::last_forked_pid);
    assert(p.get_state() == service_state_t::STARTED);

    event_loop.advance_time(time_val {0, 1000});
    sset.process_queues();

    // Now a new process should've been launched:
    assert(first_instance + 1 == bp_sys::last_forked_pid);
    assert(p.get_state() == service_state_t::STARTED);

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// Smooth recovery without restart delay
void test_proc_smooth_recovery2()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_smooth_recovery(true);
    p.set_restart_delay(time_val(0, 0));
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    pid_t first_instance = bp_sys::last_forked_pid;

    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    // no restart delay, process should restart immediately:
    assert(first_instance + 1 == bp_sys::last_forked_pid);
    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// failure during smooth recovery is non-recoverable
void test_proc_smooth_recovery3()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_smooth_recovery(true);
    p.set_restart_delay(time_val(0, 0));
    sset.add_service(&p);

    service_record d1 {&sset, "test-service-2", service_type_t::INTERNAL, {{&p, REG}}};
    d1.set_auto_restart(true);
    sset.add_service(&d1);

    d1.start();
    //p.start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    pid_t first_instance = bp_sys::last_forked_pid;

    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    // no restart delay, process should attempt restart immediately:
    assert(first_instance + 1 == bp_sys::last_forked_pid);
    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    base_process_service_test::exec_failed(&p, ENOENT);

    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPED);
    assert(p.get_target_state() == service_state_t::STOPPED);
    assert(d1.get_state() == service_state_t::STOPPED);
    assert(d1.get_target_state() == service_state_t::STOPPED);

    sset.remove_service(&d1);
    sset.remove_service(&p);
}

void test_proc_smooth_recovery4()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_smooth_recovery(true);
    p.set_restart_delay(time_val(2, 0)); // 2 second restart delay
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    pid_t first_instance = bp_sys::last_forked_pid;

    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    // smooth recovery should have begun
    event_loop.advance_time(time_val(1, 0));

    assert(p.get_state() == service_state_t::STARTED);

    // If we stop now, timer should be cancelled
    p.stop(true);

    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPED);
    assert(first_instance == bp_sys::last_forked_pid);  // no more processes launched
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// stop during smooth recovery
void test_proc_smooth_recovery5()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_smooth_recovery(true);
    p.set_restart_delay(time_val {0, 1000});
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    pid_t first_instance = bp_sys::last_forked_pid;

    assert(p.get_state() == service_state_t::STARTED);

    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    // since time hasn't been changed, we expect that the process has not yet been re-launched:
    assert(first_instance == bp_sys::last_forked_pid);
    assert(p.get_state() == service_state_t::STARTED);

    event_loop.advance_time(time_val {0, 1000});
    sset.process_queues();

    // Now a new process should've been launched:
    assert(first_instance + 1 == bp_sys::last_forked_pid);
    assert(p.get_state() == service_state_t::STARTED);

    // However, at this stage the exec has not succeeded. If we issue a stop, we shouldn't see a signal sent yet,
    // since it's not clear what signal to send (term signal might not be SIGTERM, but if it's something else, the
    // process before exec() may not respond correctly)
    bp_sys::last_sig_sent = -1;
    p.stop(true);

    assert(bp_sys::last_sig_sent == -1);
    assert(p.get_state() == service_state_t::STOPPING);

    // Once the exec succeeds, then we should:
    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(bp_sys::last_sig_sent == SIGTERM);

    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPED);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// stop during smooth recovery (while waiting on restart timer)
void test_proc_smooth_recovery6()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_smooth_recovery(true);
    p.set_restart_delay(time_val {0, 1000});
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    pid_t first_instance = bp_sys::last_forked_pid;

    assert(p.get_state() == service_state_t::STARTED);

    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    // since time hasn't been changed, we expect that the process has not yet been re-launched:
    assert(first_instance == bp_sys::last_forked_pid);
    assert(p.get_state() == service_state_t::STARTED);

    bp_sys::last_sig_sent = -1;

    // Now issue a stop:
    p.stop(true);
    sset.process_queues();

    // since we were waiting on the restart timer, there should be no process signalled and the
    // state should now be stopped:
    assert(p.get_state() == service_state_t::STOPPED);
    assert(bp_sys::last_sig_sent == -1);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// simulate the launcher process forking a daemon process, and supply the process ID of that
// daemon process in a pid file.
static void supply_pid_contents(const char *pid_file, pid_t *daemon_instance_p = nullptr)
{
    using namespace std;

    pid_t daemon_instance = ++bp_sys::last_forked_pid;

    // Set up the pid file content with the pid of the daemon
    stringstream str;
    str << daemon_instance << std::flush;
    string pid_file_content = str.str();
    vector<char> pid_file_content_v(pid_file_content.begin(), pid_file_content.end());
    bp_sys::supply_file_content(pid_file, std::move(pid_file_content_v));

    if (daemon_instance_p != nullptr) {
        *daemon_instance_p = daemon_instance;
    }
}

void test_bgproc_start()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    bgproc_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_pid_file("/run/daemon.pid");
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    supply_pid_contents("/run/daemon.pid");

    base_process_service_test::handle_exit(&p, 0);

    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

void test_bgproc_start_fail()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    bgproc_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_pid_file("/run/daemon.pid");
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::handle_exit(&p, 0x1); // fail status

    assert(p.get_state() == service_state_t::STOPPED);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

void test_bgproc_start_fail_pid()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    bgproc_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_pid_file("/run/no-exist-daemon.pid");
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    // launcher returns success, but no pid file exists:
    base_process_service_test::handle_exit(&p, 0x0);

    assert(p.get_state() == service_state_t::STOPPED);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

void test_bgproc_unexpected_term()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    bgproc_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_pid_file("/run/daemon.pid");
    sset.add_service(&p);

    service_record d1 {&sset, "test-service-2", service_type_t::INTERNAL, {{&p, REG}}};
    sset.add_service(&d1);

    d1.start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);

    assert(p.get_state() == service_state_t::STARTING);
    assert(d1.get_state() == service_state_t::STARTING);

    supply_pid_contents("/run/daemon.pid");

    base_process_service_test::handle_exit(&p, 0);

    assert(p.get_target_state() == service_state_t::STARTED);
    assert(p.get_state() == service_state_t::STARTED);
    assert(d1.get_target_state() == service_state_t::STARTED);
    assert(d1.get_state() == service_state_t::STARTED);

    // Now the backgrounded daemon itself terminates:
    base_process_service_test::handle_exit(&p, 0);

    assert(p.get_state() == service_state_t::STOPPED);
    assert(p.get_target_state() == service_state_t::STOPPED);
    assert(p.get_stop_reason() == stopped_reason_t::TERMINATED);
    assert(event_loop.active_timers.size() == 0);

    assert(d1.get_state() == service_state_t::STOPPED);
    assert(d1.get_target_state() == service_state_t::STOPPED);

    sset.remove_service(&d1);
    sset.remove_service(&p);
}

void test_bgproc_smooth_recover()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    bgproc_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_smooth_recovery(true);
    p.set_restart_delay(time_val {0, 1000});
    p.set_pid_file("/run/daemon.pid");
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    pid_t daemon_instance;
    supply_pid_contents("/run/daemon.pid", &daemon_instance);

    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::handle_exit(&p, 0); // exit the launch process
    sset.process_queues();

    // daemon process has been started now, state should be STARTED
    assert(p.get_state() == service_state_t::STARTED);
    assert(daemon_instance == bp_sys::last_forked_pid);

    base_process_service_test::handle_exit(&p, 0); // exit the daemon process

    // since time hasn't been changed, we expect that the process has not yet been re-launched:
    assert(p.get_state() == service_state_t::STARTED);
    assert(daemon_instance == bp_sys::last_forked_pid);

    event_loop.advance_time(time_val {0, 1000});
    sset.process_queues();

    // Now a new process should've been launched:
    assert(event_loop.active_timers.size() == 0);
    assert(daemon_instance + 1 == bp_sys::last_forked_pid);
    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(event_loop.active_timers.size() == 0);

    supply_pid_contents("/run/daemon.pid", &daemon_instance);

    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTED);

    assert(event_loop.active_timers.size() == 0);

    // Now run through it again

    base_process_service_test::handle_exit(&p, 0); // exit the daemon process

    // since time hasn't been changed, we expect that the process has not yet been re-launched:
    assert(p.get_state() == service_state_t::STARTED);
    assert(p.get_pid() == -1);
    assert(daemon_instance == bp_sys::last_forked_pid);

    event_loop.advance_time(time_val {0, 1000});
    sset.process_queues();

    // Now a new process should've been launched:
    assert(event_loop.active_timers.size() == 0);
    assert(daemon_instance + 1 == bp_sys::last_forked_pid);
    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(event_loop.active_timers.size() == 0);

    supply_pid_contents("/run/daemon.pid", &daemon_instance);

    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTED);
    assert(p.get_pid() == daemon_instance);

    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// stop issued during smooth recovery, with dependent
void test_bgproc_smooth_recove2()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());

    bgproc_service p {&sset, "testproc", string(command), command_offsets, {}};
    init_service_defaults(p);
    p.set_smooth_recovery(true);
    p.set_restart_delay(time_val {0, 1000});
    p.set_pid_file("/run/daemon.pid");
    sset.add_service(&p);

    process_service d1 {&sset, "testproc-d1", std::move(command), command_offsets, {{&p, REG}}};
    init_service_defaults(d1);
    sset.add_service(&d1);

    d1.start();
    sset.process_queues();

    // process for p exec succeds, reads pid file, starts
    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();
    pid_t daemon_instance;
    supply_pid_contents("/run/daemon.pid", &daemon_instance);
    assert(p.get_state() == service_state_t::STARTING);
    base_process_service_test::handle_exit(&p, 0); // exit the launch process
    sset.process_queues();
    assert(p.get_state() == service_state_t::STARTED);

    // dependent has been forked already:
    assert(bp_sys::last_forked_pid == daemon_instance + 1);

    // dependent then starts
    base_process_service_test::exec_succeeded(&d1);
    assert(bp_sys::last_forked_pid == daemon_instance + 1);

    // exit daemon process unexpectedly:
    base_process_service_test::handle_exit(&p, 0);

    // since time hasn't been changed, we expect that the process has not yet been re-launched:
    assert(p.get_state() == service_state_t::STARTED);
    assert(bp_sys::last_forked_pid == daemon_instance + 1);

    event_loop.advance_time(time_val {0, 1000});

    // Now a new process should've been launched:
    assert(event_loop.active_timers.size() == 0);
    assert(bp_sys::last_forked_pid == daemon_instance + 2);
    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    // We tell the service to stop, before the smooth recovery completes (in fact
    // before the exec even succeeds):
    p.stop(true);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPING);
    assert(d1.get_state() == service_state_t::STOPPING);

    // Now the bgprocess launcher completes but with a bogus pid file contents:
    string pid_file_content_str = "";
    vector<char> pid_file_content_v(pid_file_content_str.begin(), pid_file_content_str.end());
    bp_sys::supply_file_content("/run/daemon.pid", std::move(pid_file_content_v));
    base_process_service_test::exec_succeeded(&p);
    base_process_service_test::handle_exit(&p, 0); // exit the launch process

    assert(p.get_state() == service_state_t::STOPPING);
    assert(d1.get_state() == service_state_t::STOPPING);

    // Now the dependent stops:
    base_process_service_test::handle_exit(&d1, 0);
    assert(d1.get_state() == service_state_t::STOPPED);
    assert(p.get_state() == service_state_t::STOPPED);

    sset.remove_service(&p);
    sset.remove_service(&d1);
}

// bgproc smooth recovery failure
void test_bgproc_smooth_recove3()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());

    bgproc_service p {&sset, "testproc", string(command), command_offsets, {}};
    init_service_defaults(p);
    p.set_smooth_recovery(true);
    p.set_restart_delay(time_val {0, 1000});
    p.set_pid_file("/run/daemon.pid");
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    // process for p exec succeds, reads pid file, starts
    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();
    pid_t daemon_instance;
    supply_pid_contents("/run/daemon.pid", &daemon_instance);
    assert(p.get_state() == service_state_t::STARTING);
    base_process_service_test::handle_exit(&p, 0); // exit the launch process
    sset.process_queues();
    assert(p.get_state() == service_state_t::STARTED);

    // exit daemon process unexpectedly:
    base_process_service_test::handle_exit(&p, 0);

    // since time hasn't been changed, we expect that the process has not yet been re-launched:
    assert(p.get_state() == service_state_t::STARTED);
    assert(bp_sys::last_forked_pid == daemon_instance);

    event_loop.advance_time(time_val {0, 1000});

    // Now a new process should've been launched:
    assert(event_loop.active_timers.size() == 0);
    assert(bp_sys::last_forked_pid == daemon_instance + 1);
    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    // Let the new process fail to executable:
    base_process_service_test::exec_failed(&p, ENOMEM);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPED);
    assert(p.get_stop_reason() == stopped_reason_t::TERMINATED);

    sset.remove_service(&p);

}

// stop while in smooth recovery - waiting for restart timer
void test_bgproc_smooth_recove4()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());

    bgproc_service p {&sset, "testproc", string(command), command_offsets, {}};
    init_service_defaults(p);
    p.set_smooth_recovery(true);
    p.set_restart_delay(time_val {0, 1000});
    p.set_pid_file("/run/daemon.pid");
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    // process for p exec succeds, reads pid file, starts
    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();
    pid_t daemon_instance;
    supply_pid_contents("/run/daemon.pid", &daemon_instance);
    assert(p.get_state() == service_state_t::STARTING);
    base_process_service_test::handle_exit(&p, 0); // exit the launch process
    sset.process_queues();
    assert(p.get_state() == service_state_t::STARTED);

    // exit daemon process unexpectedly:
    base_process_service_test::handle_exit(&p, 0);

    // since time hasn't been changed, we expect that the process has not yet been re-launched:
    assert(p.get_state() == service_state_t::STARTED);
    assert(bp_sys::last_forked_pid == daemon_instance);

    // Now issue stop:
    p.stop();

    sset.process_queues();

    // since process was not running, shouldn't need to wait for it to end:
    assert(p.get_state() == service_state_t::STOPPED);

    assert(event_loop.active_timers.size() == 0);
    assert(bp_sys::last_forked_pid == daemon_instance);

    // now start again:
    p.start();
    sset.process_queues();
    supply_pid_contents("/run/daemon.pid", &daemon_instance);
    assert(p.get_state() == service_state_t::STARTING);
    base_process_service_test::exec_succeeded(&p);
    base_process_service_test::handle_exit(&p, 0); // exit the launch process
    sset.process_queues();
    assert(p.get_state() == service_state_t::STARTED);

    // and terminate:
    p.stop();
    sset.process_queues();
    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPED);

    sset.remove_service(&p);
}

// Unexpected termination with restart
void test_bgproc_term_restart()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    bgproc_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_auto_restart(true);
    p.set_restart_delay(time_val {0, 1000});
    p.set_pid_file("/run/daemon.pid");
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    pid_t daemon_instance;
    supply_pid_contents("/run/daemon.pid", &daemon_instance);

    // Launch process completes:
    base_process_service_test::handle_exit(&p, 0); // exit the launch process
    assert(p.get_state() == service_state_t::STARTED);
    assert(p.get_pid() == daemon_instance);

    // Unexpected termination of daemon:
    base_process_service_test::handle_exit(&p, 1);

    // Should re-start:
    assert(p.get_state() == service_state_t::STARTING);

    // Starting, restart timer should be armed:
    assert(p.get_state() == service_state_t::STARTING);
    assert(p.get_pid() == -1);
    assert(event_loop.active_timers.size() == 1);

    event_loop.advance_time(time_val(0, 1000));
    assert(event_loop.active_timers.size() == 0);

    assert(p.get_pid() != -1);

    // Supply new pid
    supply_pid_contents("/run/daemon.pid", &daemon_instance);

    // Launch process completes again
    base_process_service_test::handle_exit(&p, 0);
    assert(p.get_state() == service_state_t::STARTED);
    assert(p.get_pid() == daemon_instance);

    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

void test_bgproc_stop()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    bgproc_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_auto_restart(true);
    p.set_pid_file("/run/daemon.pid");
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    pid_t daemon_instance;
    supply_pid_contents("/run/daemon.pid", &daemon_instance);

    // Launch process completes:
    base_process_service_test::handle_exit(&p, 0); // exit the launch process
    assert(p.get_state() == service_state_t::STARTED);
    assert(p.get_pid() == daemon_instance);

    // Issue stop:
    p.stop();
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPING);

    base_process_service_test::handle_exit(&p, 1);

    assert(p.get_state() == service_state_t::STOPPED);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// If launch process completes successfully (exit code 0) after "stop" issued during startup,
// make sure the pid file is read and the daemon process is signalled.
void test_bgproc_stop2()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    bgproc_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_auto_restart(true);
    p.set_pid_file("/run/daemon.pid");
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    pid_t daemon_instance;
    supply_pid_contents("/run/daemon.pid", &daemon_instance);

    // Now issue stop
    p.stop();
    // doesn't really matter if state is STARTING or STOPPING
    assert(p.get_state() == service_state_t::STARTING || p.get_state() == service_state_t::STOPPING);

    // Launch process completes:
    bp_sys::last_sig_sent = -1;
    base_process_service_test::handle_exit(&p, 0); // exit the launch process

    // What should happen now: read the pid file, immediately signal the daemon, and go STOPPING
    assert(p.get_pid() == daemon_instance);
    assert(bp_sys::last_sig_sent == SIGTERM);
    assert(p.get_state() == service_state_t::STOPPING);

    // daemon exits:
    base_process_service_test::handle_exit(&p, 1);

    assert(p.get_state() == service_state_t::STOPPED);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// Stop issued during smooth recovery
void test_bgproc_stop3()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    bgproc_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_smooth_recovery(true);
    p.set_restart_delay(time_val {0, 1000});
    p.set_pid_file("/run/daemon.pid");
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    pid_t daemon_instance;
    supply_pid_contents("/run/daemon.pid", &daemon_instance);

    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::handle_exit(&p, 0); // exit the launch process
    sset.process_queues();

    // daemon process has been started now, state should be STARTED
    assert(p.get_state() == service_state_t::STARTED);
    assert(daemon_instance == bp_sys::last_forked_pid);

    base_process_service_test::handle_exit(&p, 0); // exit the daemon process

    // since time hasn't been changed, we expect that the process has not yet been re-launched:
    assert(p.get_state() == service_state_t::STARTED);
    assert(daemon_instance == bp_sys::last_forked_pid);

    event_loop.advance_time(time_val {0, 1000});
    sset.process_queues();

    // Now a new process should've been launched:
    assert(event_loop.active_timers.size() == 0);
    assert(daemon_instance + 1 == bp_sys::last_forked_pid);
    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    base_process_service_test::exec_succeeded(&p);

    assert(event_loop.active_timers.size() == 0);

    supply_pid_contents("/run/daemon.pid", &daemon_instance);

    // Now, we are in smooth recovery (state = STARTED), launch process is running.

    p.stop();
    assert(p.get_state() == service_state_t::STOPPING);

    base_process_service_test::handle_exit(&p, 0);

    // The launch process terminated, but *successfully*. So the PID file should be read and the
    // daemon process killed also.

    assert(p.get_state() == service_state_t::STOPPING);
    assert(p.get_pid() == daemon_instance);

    base_process_service_test::handle_exit(&p, 0); // exit the daemon process

    assert(p.get_state() == service_state_t::STOPPED);
    assert(daemon_instance == bp_sys::last_forked_pid);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// stop issued via command (service with stop-command set)
void test_bgproc_stop4()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    string stop_command = "stop-command";
    list<pair<unsigned,unsigned>> stop_command_offsets;
    stop_command_offsets.emplace_back(0, stop_command.length());

    bgproc_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_smooth_recovery(true);
    p.set_restart_delay(time_val {0, 1000});
    p.set_pid_file("/run/daemon.pid");
    sset.add_service(&p);
    p.set_stop_command(stop_command, stop_command_offsets);

    p.start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    pid_t daemon_instance;
    supply_pid_contents("/run/daemon.pid", &daemon_instance);

    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::handle_exit(&p, 0); // exit the launch process
    sset.process_queues();

    // daemon process has been started now, state should be STARTED
    assert(p.get_state() == service_state_t::STARTED);
    assert(daemon_instance == bp_sys::last_forked_pid);

    // so stop:
    p.stop();
    sset.process_queues();

    base_process_service_test::handle_stop_exit(&p, 0); // exit the daemon process
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPING);

    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPED);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// stop issued via command (service with stop-command set); service process dies before stop command
void test_bgproc_stop5()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    string stop_command = "stop-command";
    list<pair<unsigned,unsigned>> stop_command_offsets;
    stop_command_offsets.emplace_back(0, stop_command.length());

    bgproc_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_smooth_recovery(true);
    p.set_restart_delay(time_val {0, 1000});
    p.set_pid_file("/run/daemon.pid");
    sset.add_service(&p);
    p.set_stop_command(stop_command, stop_command_offsets);

    p.start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    pid_t daemon_instance;
    supply_pid_contents("/run/daemon.pid", &daemon_instance);

    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::handle_exit(&p, 0); // exit the launch process
    sset.process_queues();

    // daemon process has been started now, state should be STARTED
    assert(p.get_state() == service_state_t::STARTED);
    assert(daemon_instance == bp_sys::last_forked_pid);

    // so stop:
    p.stop();
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPING);

    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPING);

    base_process_service_test::handle_stop_exit(&p, 0); // exit the daemon process
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPED);
    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

// Test stop timeout
void test_scripted_stop_timeout()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    string stopcommand = "stop-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    scripted_service p {&sset, "testscripted", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_stop_command(stopcommand, command_offsets);
    p.set_stop_timeout(time_val {10, 0});
    sset.add_service(&p);

    p.start();
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();
    base_process_service_test::handle_exit(&p, 0);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTED);

    p.stop(true);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPING);

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    // should still be stopping:
    assert(p.get_state() == service_state_t::STOPPING);

    event_loop.advance_time(time_val {10, 0}); // expire stop timer
    sset.process_queues();

    // kill signal (SIGKILL) should have been sent; process not dead until it's dead, however
    assert(p.get_state() == service_state_t::STOPPING);
    assert(bp_sys::last_sig_sent == SIGKILL);

    base_process_service_test::handle_exit(&p, SIGKILL);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPED);
    assert(p.get_stop_reason() == stopped_reason_t::NORMAL);

    assert(event_loop.active_timers.size() == 0);

    sset.remove_service(&p);
}

void test_scripted_start_fail()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    string stopcommand = "stop-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    scripted_service p {&sset, "testscripted", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_stop_command(stopcommand, command_offsets);
    sset.add_service(&p);

    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{&p, REG}});
    service_record *s3 = new service_record(&sset, "test-service-3",
            service_type_t::INTERNAL, {{&p, REG}, {s2, REG}});
    sset.add_service(s2);
    sset.add_service(s3);

    s3->start();
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();
    base_process_service_test::handle_exit(&p, 0x1);  // exit fail
    sset.process_queues();

    // failed to start:
    assert(p.get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s3->get_state() == service_state_t::STOPPED);
    assert(p.get_stop_reason() == stopped_reason_t::FAILED);
    assert(s2->get_stop_reason() == stopped_reason_t::DEPFAILED);
    assert(s3->get_stop_reason() == stopped_reason_t::DEPFAILED);

    event_loop.active_timers.clear();
    sset.remove_service(&p);

    assert(sset.count_active_services() == 0);
}

void test_scripted_stop_fail()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    string stopcommand = "stop-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    scripted_service p {&sset, "testscripted", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    p.set_stop_command(stopcommand, command_offsets);
    sset.add_service(&p);

    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {});
    service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL,
            {{s2, REG}, {&p, REG}});
    service_record *s4 = new service_record(&sset, "test-service-4", service_type_t::INTERNAL,
            {{&p, REG}, {s3, REG}});
    sset.add_service(s2);
    sset.add_service(s3);
    sset.add_service(s4);

    s4->start();
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();
    base_process_service_test::handle_exit(&p, 0x0);  // success
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(s3->get_state() == service_state_t::STARTED);
    assert(s4->get_state() == service_state_t::STARTED);

    pid_t last_forked = bp_sys::last_forked_pid;

    s4->stop(true);
    sset.process_queues();

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();
    base_process_service_test::handle_exit(&p, 0x1);  // failure
    sset.process_queues();

    // The stop command should be executed once:
    assert((bp_sys::last_forked_pid - last_forked) == 1);

    assert(p.get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(s3->get_state() == service_state_t::STOPPED);
    assert(s4->get_state() == service_state_t::STOPPED);

    event_loop.active_timers.clear();
    sset.remove_service(&p);
}

void test_scripted_start_skip()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    scripted_service p {&sset, "testscripted", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    service_flags_t sflags;
    sflags.skippable = true;
    p.set_flags(sflags);
    sset.add_service(&p);

    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{&p, REG}});
    sset.add_service(s2);

    s2->start();
    sset.process_queues();
    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();
    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::handle_signal_exit(&p, SIGINT); // interrupted
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);
    assert(p.was_start_skipped());
    assert(! s2->was_start_skipped());
    assert(sset.count_active_services() == 2);

    s2->stop(true);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(p.get_stop_reason() == stopped_reason_t::NORMAL);
    assert(s2->get_stop_reason() == stopped_reason_t::NORMAL);
    assert(sset.count_active_services() == 0);

    event_loop.active_timers.clear();
    sset.remove_service(&p);
}

// Test interrupting start of a service marked skippable
void test_scripted_start_skip2()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    scripted_service p {&sset, "testscripted", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    service_flags_t sflags;
    sflags.skippable = true;
    sflags.start_interruptible = true;
    p.set_flags(sflags);
    sset.add_service(&p);

    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {{&p, REG}});
    sset.add_service(s2);

    s2->start();
    sset.process_queues();
    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();
    assert(p.get_state() == service_state_t::STARTING);

    s2->stop(true);  // abort startup; p should be cancelled
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPING);

    base_process_service_test::handle_signal_exit(&p, SIGINT); // interrupted
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);
    assert(p.get_stop_reason() == stopped_reason_t::NORMAL);
    assert(s2->get_stop_reason() == stopped_reason_t::NORMAL);
    assert(sset.count_active_services() == 0);

    event_loop.active_timers.clear();
    sset.remove_service(&p);
}

// Test that starting a service with a waits-for dependency on another - currently stopping - service,
// causes that service to re-start.
void test_waitsfor_restart()
{
    using namespace std;

    service_set sset;

    string command = "test-command";
    list<pair<unsigned,unsigned>> command_offsets;
    command_offsets.emplace_back(0, command.length());
    std::list<prelim_dep> depends;

    process_service p {&sset, "testproc", std::move(command), command_offsets, depends};
    init_service_defaults(p);
    sset.add_service(&p);

    service_record tp {&sset, "test-service", service_type_t::INTERNAL, {{&p, WAITS}}};
    sset.add_service(&tp);

    // start p:

    p.start();
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STARTED);
    assert(event_loop.active_timers.size() == 0);

    // begin stopping p:

    p.stop(true);
    sset.process_queues();

    assert(p.get_state() == service_state_t::STOPPING);

    // start tp (which waits-for p):

    tp.start();
    sset.process_queues();

    assert(tp.get_state() == service_state_t::STARTING);
    assert(p.get_state() == service_state_t::STOPPING);

    // p terminates (finishes stopping). Then it should re-start...
    base_process_service_test::handle_signal_exit(&p, SIGTERM);
    sset.process_queues();

    assert(tp.get_state() == service_state_t::STARTING);
    assert(p.get_state() == service_state_t::STARTING);

    base_process_service_test::exec_succeeded(&p);
    sset.process_queues();

    assert(tp.get_state() == service_state_t::STARTED);
    assert(p.get_state() == service_state_t::STARTED);

    sset.remove_service(&tp);
    sset.remove_service(&p);
}


#define RUN_TEST(name, spacing) \
    std::cout << #name "..." spacing << std::flush; \
    name(); \
    std::cout << "PASSED" << std::endl;

int main(int argc, char **argv)
{
    RUN_TEST(test_proc_service_start, "    ");
    RUN_TEST(test_proc_notify_start, "     ");
    RUN_TEST(test_proc_unexpected_term, "  ");
    RUN_TEST(test_proc_term_restart, "     ");
    RUN_TEST(test_proc_term_restart2, "    ");
    RUN_TEST(test_proc_term_restart3, "    ");
    RUN_TEST(test_term_via_stop, "         ");
    RUN_TEST(test_term_via_stop2, "        ");
    RUN_TEST(test_term_via_stop3, "        ");
    RUN_TEST(test_proc_start_timeout, "    ");
    RUN_TEST(test_proc_start_timeout2, "   ");
    RUN_TEST(test_proc_start_execfail, "   ");
    RUN_TEST(test_proc_notify_fail, "      ");
    RUN_TEST(test_proc_stop_timeout, "     ");
    RUN_TEST(test_proc_smooth_recovery1, " ");
    RUN_TEST(test_proc_smooth_recovery2, " ");
    RUN_TEST(test_proc_smooth_recovery3, " ");
    RUN_TEST(test_proc_smooth_recovery4, " ");
    RUN_TEST(test_proc_smooth_recovery5, " ");
    RUN_TEST(test_proc_smooth_recovery6, " ");
    RUN_TEST(test_bgproc_start, "          ");
    RUN_TEST(test_bgproc_start_fail, "     ");
    RUN_TEST(test_bgproc_start_fail_pid, " ");
    RUN_TEST(test_bgproc_unexpected_term, "");
    RUN_TEST(test_bgproc_smooth_recover, " ");
    RUN_TEST(test_bgproc_smooth_recove2, " ");
    RUN_TEST(test_bgproc_smooth_recove3, " ");
    RUN_TEST(test_bgproc_smooth_recove4, " ");
    RUN_TEST(test_bgproc_term_restart, "   ");
    RUN_TEST(test_bgproc_stop, "           ");
    RUN_TEST(test_bgproc_stop2, "          ");
    RUN_TEST(test_bgproc_stop3, "          ");
    RUN_TEST(test_bgproc_stop4, "          ");
    RUN_TEST(test_bgproc_stop5, "          ");
    RUN_TEST(test_scripted_stop_timeout, " ");
    RUN_TEST(test_scripted_start_fail, "   ");
    RUN_TEST(test_scripted_stop_fail, "    ");
    RUN_TEST(test_scripted_start_skip, "   ");
    RUN_TEST(test_scripted_start_skip2, "  ");
    RUN_TEST(test_waitsfor_restart, "      ");
}
