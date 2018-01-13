#include <cstring>

#include "dinit-log.h"
#include "dinit-socket.h"
#include "proc-service.h"

/*
 * Base process implementation (base_process_service).
 *
 * See proc-service.h for interface documentation.
 */

extern eventloop_t event_loop;

using clock_type = dasynq::clock_type;
using rearm = dasynq::rearm;
using time_val = dasynq::time_val;

void base_process_service::do_smooth_recovery() noexcept
{
    if (! restart_ps_process()) {
        emergency_stop();
        services->process_queues();
    }
}

bool base_process_service::bring_up() noexcept
{
    if (restarting) {
        if (pid == -1) {
            return restart_ps_process();
        }
        return true;
    }
    else {
        event_loop.get_time(restart_interval_time, clock_type::MONOTONIC);
        restart_interval_count = 0;
        if (start_ps_process(exec_arg_parts, onstart_flags.starts_on_console)) {
            if (start_timeout != time_val(0,0)) {
                restart_timer.arm_timer_rel(event_loop, start_timeout);
                stop_timer_armed = true;
            }
            else if (stop_timer_armed) {
                restart_timer.stop_timer(event_loop);
                stop_timer_armed = false;
            }
            return true;
        }
        return false;
    }
}

bool base_process_service::start_ps_process(const std::vector<const char *> &cmd, bool on_console) noexcept
{
    // In general, you can't tell whether fork/exec is successful. We use a pipe to communicate
    // success/failure from the child to the parent. The pipe is set CLOEXEC so a successful
    // exec closes the pipe, and the parent sees EOF. If the exec is unsuccessful, the errno
    // is written to the pipe, and the parent can read it.

    event_loop.get_time(last_start_time, clock_type::MONOTONIC);

    int pipefd[2];
    if (dasynq::pipe2(pipefd, O_CLOEXEC)) {
        log(loglevel_t::ERROR, get_name(), ": can't create status check pipe: ", strerror(errno));
        return false;
    }

    const char * logfile = this->logfile.c_str();
    if (*logfile == 0) {
        logfile = "/dev/null";
    }

    bool child_status_registered = false;
    control_conn_t *control_conn = nullptr;

    int control_socket[2] = {-1, -1};
    if (onstart_flags.pass_cs_fd) {
        if (dinit_socketpair(AF_UNIX, SOCK_STREAM, /* protocol */ 0, control_socket, SOCK_NONBLOCK)) {
            log(loglevel_t::ERROR, get_name(), ": can't create control socket: ", strerror(errno));
            goto out_p;
        }

        // Make the server side socket close-on-exec:
        int fdflags = fcntl(control_socket[0], F_GETFD);
        fcntl(control_socket[0], F_SETFD, fdflags | FD_CLOEXEC);

        try {
            control_conn = new control_conn_t(event_loop, services, control_socket[0]);
        }
        catch (std::exception &exc) {
            log(loglevel_t::ERROR, get_name(), ": can't launch process; out of memory");
            goto out_cs;
        }
    }

    // Set up complete, now fork and exec:

    pid_t forkpid;

    try {
        child_status_listener.add_watch(event_loop, pipefd[0], dasynq::IN_EVENTS);
        child_status_registered = true;

        // We specify a high priority (i.e. low priority value) so that process termination is
        // handled early. This means we have always recorded that the process is terminated by the
        // time that we handle events that might otherwise cause us to signal the process, so we
        // avoid sending a signal to an invalid (and possibly recycled) process ID.
        forkpid = child_listener.fork(event_loop, reserved_child_watch, dasynq::DEFAULT_PRIORITY - 10);
        reserved_child_watch = true;
    }
    catch (std::exception &e) {
        log(loglevel_t::ERROR, get_name(), ": Could not fork: ", e.what());
        goto out_cs_h;
    }

    if (forkpid == 0) {
        run_child_proc(cmd.data(), logfile, on_console, pipefd[1], control_socket[1]);
    }
    else {
        // Parent process
        close(pipefd[1]); // close the 'other end' fd
        if (control_socket[1] != -1) {
            close(control_socket[1]);
        }
        pid = forkpid;

        waiting_for_execstat = true;
        return true;
    }

    // Failure exit:

    out_cs_h:
    if (child_status_registered) {
        child_status_listener.deregister(event_loop);
    }

    if (onstart_flags.pass_cs_fd) {
        delete control_conn;

        out_cs:
        close(control_socket[0]);
        close(control_socket[1]);
    }

    out_p:
    close(pipefd[0]);
    close(pipefd[1]);

    return false;
}

void base_process_service::bring_down() noexcept
{
    waiting_for_deps = false;
    if (pid != -1) {
        // The process is still kicking on - must actually kill it. We signal the process
        // group (-pid) rather than just the process as there's less risk then of creating
        // an orphaned process group:
        if (! onstart_flags.no_sigterm) {
            kill_pg(SIGTERM);
        }
        if (term_signal != -1) {
            kill_pg(term_signal);
        }

        // In most cases, the rest is done in handle_exit_status.
        // If we are a BGPROCESS and the process is not our immediate child, however, that
        // won't work - check for this now:
        if (get_type() == service_type_t::BGPROCESS && ! tracking_child) {
            stopped();
        }
        else if (stop_timeout != time_val(0,0)) {
            restart_timer.arm_timer_rel(event_loop, stop_timeout);
            stop_timer_armed = true;
        }
    }
    else {
        // The process is already dead.
        stopped();
    }
}

base_process_service::base_process_service(service_set *sset, string name,
        service_type_t service_type_p, string &&command,
        std::list<std::pair<unsigned,unsigned>> &command_offsets,
        const std::list<prelim_dep> &deplist_p)
     : service_record(sset, name, service_type_p, deplist_p), child_listener(this),
       child_status_listener(this), restart_timer(this)
{
    program_name = std::move(command);
    exec_arg_parts = separate_args(program_name, command_offsets);

    restart_interval_count = 0;
    restart_interval_time = {0, 0};
    restart_timer.service = this;
    restart_timer.add_timer(event_loop);

    // By default, allow a maximum of 3 restarts within 10.0 seconds:
    restart_interval.seconds() = 10;
    restart_interval.nseconds() = 0;
    max_restart_interval_count = 3;

    waiting_restart_timer = false;
    reserved_child_watch = false;
    tracking_child = false;
    stop_timer_armed = false;
    start_is_interruptible = false;
}

void base_process_service::do_restart() noexcept
{
    waiting_restart_timer = false;
    restart_interval_count++;
    auto service_state = get_state();

    // We may be STARTING (regular restart) or STARTED ("smooth recovery"). This affects whether
    // the process should be granted access to the console:
    bool on_console = service_state == service_state_t::STARTING
            ? onstart_flags.starts_on_console : onstart_flags.runs_on_console;

    if (service_state == service_state_t::STARTING) {
        // for a smooth recovery, we want to check dependencies are available before actually
        // starting:
        if (! check_deps_started()) {
            waiting_for_deps = true;
            return;
        }
    }

    if (! start_ps_process(exec_arg_parts, on_console)) {
        restarting = false;
        if (service_state == service_state_t::STARTING) {
            failed_to_start();
        }
        else {
            // desired_state = service_state_t::STOPPED;
            forced_stop();
        }
        services->process_queues();
    }
}

bool base_process_service::restart_ps_process() noexcept
{
    using time_val = dasynq::time_val;

    time_val current_time;
    event_loop.get_time(current_time, clock_type::MONOTONIC);

    if (max_restart_interval_count != 0) {
        // Check whether we're still in the most recent restart check interval:
        time_val int_diff = current_time - restart_interval_time;
        if (int_diff < restart_interval) {
            if (restart_interval_count >= max_restart_interval_count) {
                log(loglevel_t::ERROR, "Service ", get_name(), " restarting too quickly; stopping.");
                return false;
            }
        }
        else {
            restart_interval_time = current_time;
            restart_interval_count = 0;
        }
    }

    // Check if enough time has lapsed since the prevous restart. If not, start a timer:
    time_val tdiff = current_time - last_start_time;
    if (restart_delay <= tdiff) {
        // > restart delay (normally 200ms)
        do_restart();
    }
    else {
        time_val timeout = restart_delay - tdiff;
        restart_timer.arm_timer_rel(event_loop, timeout);
        waiting_restart_timer = true;
    }
    return true;
}

bool base_process_service::interrupt_start() noexcept
{
    if (waiting_restart_timer) {
        restart_timer.stop_timer(event_loop);
        waiting_restart_timer = false;
        return service_record::interrupt_start();
    }
    else {
        log(loglevel_t::WARN, "Interrupting start of service ", get_name(), " with pid ", pid, " (with SIGINT).");
        kill_pg(SIGINT);
        if (stop_timeout != time_val(0,0)) {
            restart_timer.arm_timer_rel(event_loop, stop_timeout);
            stop_timer_armed = true;
        }
        else if (stop_timer_armed) {
            restart_timer.stop_timer(event_loop);
            stop_timer_armed = false;
        }
        set_state(service_state_t::STOPPING);
        notify_listeners(service_event_t::STARTCANCELLED);
        return false;
    }
}

void base_process_service::kill_with_fire() noexcept
{
    if (pid != -1) {
        log(loglevel_t::WARN, "Service ", get_name(), " with pid ", pid, " exceeded allowed stop time; killing.");
        kill_pg(SIGKILL);
    }
}

void base_process_service::kill_pg(int signo) noexcept
{
    pid_t pgid = getpgid(pid);
    if (pgid == -1) {
        // only should happen if pid is invalid, which should never happen...
        log(loglevel_t::ERROR, get_name(), ": can't signal process: ", strerror(errno));
        return;
    }
    kill(-pgid, signo);
}
