#ifndef TEST_PROCSERVICE
#define TEST_PROCSERVICE

#include <dinit.h>

// Friend interface to access base_process_service private/protected members.
class base_process_service_test
{
    public:
    static void exec_succeeded(base_process_service *sr)
    {
        // When exec succeeds the child status pipe write end will automatically be closed (it is
        // close-on-exec). Simulate that by presenting an end-of-file to the read end:
        std::vector<char> status_data;
        int exec_fd = sr->child_status_listener.get_watched_fd();
        bp_sys::supply_read_data(exec_fd, std::move(status_data));

        event_loop.send_fd_event(exec_fd, dasynq::IN_EVENTS);
    }

    static void stop_exec_succeeded(process_service *sr)
    {
        std::vector<char> status_data;
        int exec_fd = sr->stop_pipe_watcher.get_watched_fd();
        bp_sys::supply_read_data(exec_fd, std::move(status_data));

        event_loop.send_fd_event(exec_fd, dasynq::IN_EVENTS);
    }

    static void exec_failed(base_process_service *sr, int errcode)
    {
        // When exec fails, the child process writes a stage code and errno value to the child
        // status pipe. Simulate that by presenting equivalent data to the read end:
        std::vector<char> status_data;
        status_data.resize(sizeof(run_proc_err));
        run_proc_err exec_status;
        exec_status.stage = exec_stage::DO_EXEC;
        exec_status.st_errno = errcode;
        memcpy(status_data.data(), &exec_status, sizeof(exec_status));
        int exec_fd = sr->child_status_listener.get_watched_fd();
        bp_sys::supply_read_data(exec_fd, std::move(status_data));

        event_loop.send_fd_event(exec_fd, dasynq::IN_EVENTS);
        event_loop.send_proc_event(sr->pid, {CLD_EXITED, 0});
    }

    static void handle_exit(base_process_service *sr, int exit_status)
    {
        event_loop.send_proc_event(sr->pid, {CLD_EXITED, exit_status});
    }

    static void handle_signal_exit(base_process_service *sr, int signo)
    {
        event_loop.send_proc_event(sr->pid, {CLD_KILLED, signo});
    }

    static void handle_stop_exit(process_service *ps, int exit_status)
    {
        event_loop.send_proc_event(ps->stop_pid, {CLD_EXITED, exit_status});
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

static time_val default_restart_interval = time_val(0, 200000000); // 200 milliseconds

static void init_service_defaults(base_process_service &ps)
{
    ps.set_restart_interval(time_val(10,0), 3);
    ps.set_restart_delay(default_restart_interval); // 200 milliseconds
    ps.set_stop_timeout(time_val(10,0));
}

#endif /* TEST_PROCSERVICE */
