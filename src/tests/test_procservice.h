#ifndef TEST_PROCSERVICE
#define TEST_PROCSERVICE

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
        bsp->exit_status = eventloop_t::child_proc_watcher::proc_status_t(CLD_EXITED, exit_status);
        bsp->handle_exit_status();
    }

    static void handle_signal_exit(base_process_service *bsp, int signo)
    {
        bsp->pid = -1;
        bsp->exit_status = eventloop_t::child_proc_watcher::proc_status_t(CLD_KILLED, signo);
        bsp->handle_exit_status();
    }

    static void handle_stop_exit(process_service *ps, int exit_status)
    {
        ps->stop_pid = -1;
        ps->waiting_for_execstat = false;
        ps->stop_status = eventloop_t::child_proc_watcher::proc_status_t(CLD_EXITED, exit_status);
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

static time_val default_restart_interval = time_val(0, 200000000); // 200 milliseconds

static void init_service_defaults(base_process_service &ps)
{
    ps.set_restart_interval(time_val(10,0), 3);
    ps.set_restart_delay(default_restart_interval); // 200 milliseconds
    ps.set_stop_timeout(time_val(10,0));
}

#endif /* TEST_PROCSERVICE */
