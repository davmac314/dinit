#ifndef TEST_PROCSERVICE
#define TEST_PROCSERVICE

#include <dinit.h>

// Friend interface to access base_process_service private/protected members.
class base_process_service_test
{
    public:
    static void exec_succeeded(base_process_service *bsp)
    {
        // TODO: signal this directly via simulated pipe/event loop
        bsp->waiting_for_execstat = false;
        bsp->exec_succeeded();
    }

    static void exec_failed(base_process_service *sr, int errcode)
    {
        // TODO: signal this directly via simulated pipe/event loop
        run_proc_err exec_status;
        exec_status.stage = exec_stage::DO_EXEC;
        exec_status.st_errno = errcode;
        sr->waiting_for_execstat = false;
        if (sr->pid != -1) {
            sr->child_listener.deregister(event_loop, sr->pid);
            sr->reserved_child_watch = false;
            if (sr->waiting_stopstart_timer) {
                sr->process_timer.stop_timer(event_loop);
                sr->waiting_stopstart_timer = false;
            }
        }
        sr->pid = -1;
        sr->exec_err_info = exec_status;
        sr->exec_failed(exec_status);
    }

    static void handle_exit(base_process_service *bsp, int exit_status)
    {
        bsp->child_listener.status_change(event_loop, bsp->pid,
                eventloop_t::child_proc_watcher::proc_status_t(CLD_EXITED, exit_status));
    }

    static void handle_signal_exit(base_process_service *bsp, int signo)
    {
        bsp->child_listener.status_change(event_loop, bsp->pid,
                eventloop_t::child_proc_watcher::proc_status_t(CLD_KILLED, signo));
    }

    static void handle_stop_exit(process_service *ps, int exit_status)
    {
        // (Effectively signals that stop process exec succeeded also)
        // TODO: signal this directly via event loop watcher

        ps->waiting_for_execstat = false;
        ps->stop_pid = -1;
        ps->stop_status = eventloop_t::child_proc_watcher::proc_status_t(CLD_EXITED, exit_status);
        ps->stop_watcher.stop_watch(event_loop);
        ps->handle_stop_exit();

        ps->services->process_queues();
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
