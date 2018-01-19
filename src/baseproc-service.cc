#include <cstring>

#include <sys/un.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dinit.h"
#include "dinit-log.h"
#include "dinit-socket.h"
#include "proc-service.h"

#include "baseproc-sys.h"

/*
 * Base process implementation (base_process_service).
 *
 * See proc-service.h for interface documentation.
 */

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
        if (! open_socket()) {
            return false;
        }

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
    if (bp_sys::pipe2(pipefd, O_CLOEXEC)) {
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
        int fdflags = bp_sys::fcntl(control_socket[0], F_GETFD);
        bp_sys::fcntl(control_socket[0], F_SETFD, fdflags | FD_CLOEXEC);

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
        run_child_proc(cmd.data(), logfile, on_console, pipefd[1], control_socket[1], socket_fd);
    }
    else {
        // Parent process
        bp_sys::close(pipefd[1]); // close the 'other end' fd
        if (control_socket[1] != -1) {
            bp_sys::close(control_socket[1]);
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
        bp_sys::close(control_socket[0]);
        bp_sys::close(control_socket[1]);
    }

    out_p:
    bp_sys::close(pipefd[0]);
    bp_sys::close(pipefd[1]);

    return false;
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

    if (service_state == service_state_t::STARTING) {
        // for a smooth recovery, we want to check dependencies are available before actually
        // starting:
        if (! check_deps_started()) {
            waiting_for_deps = true;
            return;
        }
    }

    if (! start_ps_process(exec_arg_parts, have_console)) {
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

    // Check if enough time has lapsed since the previous restart. If not, start a timer:
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
    pid_t pgid = bp_sys::getpgid(pid);
    if (pgid == -1) {
        // only should happen if pid is invalid, which should never happen...
        log(loglevel_t::ERROR, get_name(), ": can't signal process: ", strerror(errno));
        return;
    }
    bp_sys::kill(-pgid, signo);
}

void base_process_service::timer_expired() noexcept
{
    stop_timer_armed = false;

    // Timer expires if:
    // We are stopping, including after having startup cancelled (stop timeout, state is STOPPING); We are
    // starting (start timeout, state is STARTING); We are waiting for restart timer before restarting,
    // including smooth recovery (restart timeout, state is STARTING or STARTED).
    if (get_state() == service_state_t::STOPPING) {
        kill_with_fire();
    }
    else if (pid != -1) {
        // Starting, start timed out.
        stop_dependents();
        if (start_explicit) {
            start_explicit = false;
            release();
        }
        interrupt_start();
    }
    else {
        // STARTING / STARTED, and we have a pid: must be restarting (smooth recovery if STARTED)
        do_restart();
    }
}

void base_process_service::emergency_stop() noexcept
{
    if (! do_auto_restart() && start_explicit) {
        start_explicit = false;
        release(false);
    }
    forced_stop();
    stop_dependents();
    stopped();
}

void base_process_service::becoming_inactive() noexcept
{
    if (socket_fd != -1) {
        close(socket_fd);
        socket_fd = -1;
    }
}

bool base_process_service::open_socket() noexcept
{
    if (socket_path.empty() || socket_fd != -1) {
        // No socket, or already open
        return true;
    }

    const char * saddrname = socket_path.c_str();

    // Check the specified socket path
    struct stat stat_buf;
    if (stat(saddrname, &stat_buf) == 0) {
        if ((stat_buf.st_mode & S_IFSOCK) == 0) {
            // Not a socket
            log(loglevel_t::ERROR, get_name(), ": Activation socket file exists (and is not a socket)");
            return false;
        }
    }
    else if (errno != ENOENT) {
        // Other error
        log(loglevel_t::ERROR, get_name(), ": Error checking activation socket: ", strerror(errno));
        return false;
    }

    // Remove stale socket file (if it exists).
    // We won't test the return from unlink - if it fails other than due to ENOENT, we should get an
    // error when we try to create the socket anyway.
    unlink(saddrname);

    uint sockaddr_size = offsetof(struct sockaddr_un, sun_path) + socket_path.length() + 1;
    struct sockaddr_un * name = static_cast<sockaddr_un *>(malloc(sockaddr_size));
    if (name == nullptr) {
        log(loglevel_t::ERROR, get_name(), ": Opening activation socket: out of memory");
        return false;
    }

    name->sun_family = AF_UNIX;
    strcpy(name->sun_path, saddrname);

    int sockfd = dinit_socket(AF_UNIX, SOCK_STREAM, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (sockfd == -1) {
        log(loglevel_t::ERROR, get_name(), ": Error creating activation socket: ", strerror(errno));
        free(name);
        return false;
    }

    if (bind(sockfd, (struct sockaddr *) name, sockaddr_size) == -1) {
        log(loglevel_t::ERROR, get_name(), ": Error binding activation socket: ", strerror(errno));
        close(sockfd);
        free(name);
        return false;
    }

    free(name);

    // POSIX (1003.1, 2013) says that fchown and fchmod don't necessarily work on sockets. We have to
    // use chown and chmod instead.
    if (chown(saddrname, socket_uid, socket_gid)) {
        log(loglevel_t::ERROR, get_name(), ": Error setting activation socket owner/group: ", strerror(errno));
        close(sockfd);
        return false;
    }

    if (chmod(saddrname, socket_perms) == -1) {
        log(loglevel_t::ERROR, get_name(), ": Error setting activation socket permissions: ", strerror(errno));
        close(sockfd);
        return false;
    }

    if (listen(sockfd, 128) == -1) { // 128 "seems reasonable".
        log(loglevel_t::ERROR, ": Error listening on activation socket: ", strerror(errno));
        close(sockfd);
        return false;
    }

    socket_fd = sockfd;
    return true;
}
