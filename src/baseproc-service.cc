#include <cstring>
#include <cstdlib>

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
    if (!restart_ps_process()) {
        unrecoverable_stop();
        services->process_queues();
    }
}

bool base_process_service::bring_up() noexcept
{
    if (!open_socket()) {
        return false;
    }

    bool start_success;
    if (in_auto_restart) {
        start_success = restart_ps_process();
    }
    else {
        restart_interval_count = 0;
        start_success = start_ps_process(exec_arg_parts,
                onstart_flags.starts_on_console || onstart_flags.shares_console);
        // start_ps_process updates last_start_time, use it also for restart_interval_time:
        restart_interval_time = last_start_time;

        // Arm start timer. (For restarts, this is only done once the restart interval expires).
        if (start_success) {
            if (start_timeout != time_val(0,0)) {
                process_timer.arm_timer_rel(event_loop, start_timeout);
                waiting_stopstart_timer = true;
            }
            else if (waiting_stopstart_timer) {
                process_timer.stop_timer(event_loop);
                waiting_stopstart_timer = false;
            }
        }
    }

    return start_success;
}

void base_process_service::handle_unexpected_termination() noexcept
{
    // unexpected termination, with possible restart
    stop_reason = stopped_reason_t::TERMINATED;

    // We want to circumvent the normal process of waiting for dependents to stop before we
    // attempt to restart, for two reasons:
    // 1) we can restart more quickly
    // 2) we can use the restart rate-limiting logic from restart_ps_process rather than
    //    the usual start_ps_process (the usual bring-up).
    // But we need to issue a forced stop and process queues, to discover our eventual target
    // state (so we know whether we actually want to restart or not).
    // Note we can't call forced_stop() directly here, because we need to set in_auto_restart in
    // between do_stop() and processing queues (so that it is set correctly if restart occurs):

    force_stop = true;
    do_stop();
    services->process_queues();

    if (get_state() == service_state_t::STOPPING) {
        // We must be waiting for dependents;
        // If we're going to restart, we can kick that off now:
        if (get_target_state() == service_state_t::STARTED && !pinned_stopped) {
            initiate_start();
            services->process_queues();
        }
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

    int input_fd;
    if (!get_input_fd(&input_fd)) {
        return false;
    }

    const char * logfile = this->logfile.c_str();
    if (this->log_type == log_type_id::LOGFILE || this->log_type == log_type_id::NONE) {
        // Note: if log_type == NONE, logfile should be empty.
        if (*logfile == 0) {
            logfile = "/dev/null";
        }
    }
    else /* log_type_id::BUFFER or ::PIPE */ {
        if (this->log_output_fd == -1) {
            int logfd[2];
            // Note: we set CLOEXEC on the file descriptors here; when the output file descriptor is dup'd
            // to stdout, this will be effectively removed for the output end
            if (bp_sys::pipe2(logfd, O_CLOEXEC)) {
                log(loglevel_t::ERROR, get_name(), ": can't create output pipe: ", strerror(errno));
                goto out_p;
            }
            this->log_input_fd = logfd[0];
            this->log_output_fd = logfd[1];
            if (this->log_type == log_type_id::BUFFER) {
                try {
                    this->log_output_listener.add_watch(event_loop, logfd[0], dasynq::IN_EVENTS,
                            false /* not enabled */);
                }
                catch (...) {
                    log(loglevel_t::ERROR, get_name(), ": can't add output watch (insufficient resources)");
                    bp_sys::close(this->log_input_fd);
                    bp_sys::close(this->log_output_fd);
                    this->log_input_fd = -1;
                    this->log_output_fd = -1;
                    goto out_p;
                }
            }
        }
        // (More is done below, after we have performed additional setup)
    }

    {
        bool child_status_registered = false;
        control_conn_t *control_conn = nullptr;

        int control_socket[2] = {-1, -1};
        int notify_pipe[2] = {-1, -1};
        int notify_fd = -1;
        bool have_notify = !notification_var.empty() || notification_sock || force_notification_fd != -1;
        ready_notify_watcher * rwatcher = have_notify ? get_ready_watcher() : nullptr;
        bool ready_watcher_registered = false;

        if (onstart_flags.pass_cs_fd) {
            if (dinit_socketpair(AF_UNIX, SOCK_STREAM, /* protocol */ 0, control_socket, SOCK_NONBLOCK)) {
                log(loglevel_t::ERROR, get_name(), ": can't create control socket: ", strerror(errno));
                goto out_lfd;
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

        if (have_notify && !notification_sock) {
            // Create a notification pipe:
            if (bp_sys::pipe2(notify_pipe, 0) != 0) {
                log(loglevel_t::ERROR, get_name(), ": can't create notification pipe: ", strerror(errno));
                goto out_cs_h;
            }

            // Set the read side as close-on-exec:
            int fdflags = bp_sys::fcntl(notify_pipe[0], F_GETFD);
            bp_sys::fcntl(notify_pipe[0], F_SETFD, fdflags | FD_CLOEXEC);
            notify_fd = notify_pipe[1];
            rwatcher->sun.sun_family = AF_UNSPEC;
        } else if (have_notify) {
            notify_fd = socket(AF_UNIX, SOCK_DGRAM|SOCK_CLOEXEC, 0);
            if (notify_fd < 0) {
                log(loglevel_t::ERROR, get_name(), ": can't create notification socket: ", strerror(errno));
                goto out_cs_h;
            }
            memset(&rwatcher->sun, 0, sizeof(rwatcher->sun));
            rwatcher->sun.sun_family = AF_UNIX;
            snprintf(&rwatcher->sun.sun_path[1], sizeof(rwatcher->sun.sun_path) - 1, "/tmp/dinit-ready-notify");
            if (bind(notify_fd, reinterpret_cast<sockaddr *>(&rwatcher->sun), sizeof("/tmp/dinit-ready-notify") + sizeof(sa_family_t)) < 0) {
                log(loglevel_t::ERROR, get_name(), ": can't bind to notification socket: ", strerror(errno));
                goto out_cs_h;
            }
            notification_var = "NOTIFY_SOCKET=@/tmp/dinit-ready-notify";
        }

        if (have_notify) {
            // add, but don't yet enable, readiness watcher:
            try {
                rwatcher->add_watch(event_loop, notification_sock ? notify_fd : notify_pipe[0], dasynq::IN_EVENTS, false);
                ready_watcher_registered = true;
            }
            catch (std::exception &exc) {
                log(loglevel_t::ERROR, get_name(), ": can't add notification watch: ", exc.what());
                goto out_cs_h;
            }
        }

        if (log_type == log_type_id::BUFFER) {
            // Set watcher enabled if space in buffer
            if (log_buf_size > 0) {
                // Append a "restarted" message to buffer contents
                const char *restarting_msg = "\n(dinit: note: service restarted)\n";
                unsigned restarting_msg_len = strlen(restarting_msg);
                bool trailing_nl = log_buffer[log_buf_size - 1] == '\n';
                if (trailing_nl) {
                    ++restarting_msg; // trim leading newline
                    --restarting_msg_len;
                }
                if (log_buf_size + restarting_msg_len >= log_buf_max) {
                    goto skip_enable_log_watch;
                }
                if (!ensure_log_buffer_backing(log_buf_size + restarting_msg_len)) {
                    goto skip_enable_log_watch;
                }
                memcpy(log_buffer.data() + log_buf_size, restarting_msg, restarting_msg_len);
                log_buf_size += restarting_msg_len;
            }
            log_output_listener.set_enabled(event_loop, true);
        }
        skip_enable_log_watch: ;

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
            log(loglevel_t::ERROR, get_name(), ": could not fork: ", e.what());
            goto out_cs_h;
        }

        if (forkpid == 0) {
            const char * working_dir_c = service_dsc_dir;
            if (!working_dir.empty()) working_dir_c = working_dir.c_str();
            after_fork(getpid());
            run_proc_params run_params{cmd.data(), working_dir_c, logfile, pipefd[1], run_as_uid, run_as_gid, rlimits};
            run_params.on_console = on_console;
            run_params.in_foreground = !onstart_flags.shares_console;
            run_params.unmask_sigint = onstart_flags.unmask_intr;
            run_params.csfd = control_socket[1];
            run_params.socket_fd = socket_fd;
            run_params.notify_fd = notify_fd;
            run_params.force_notify_fd = force_notification_fd;
            run_params.notify_var = notification_var.c_str();
            run_params.env_file = env_file.c_str();
            run_params.output_fd = log_output_fd;
            run_params.input_fd = input_fd;
            #if SUPPORT_CGROUPS
            run_params.run_in_cgroup = run_in_cgroup.c_str();
            #endif
            run_child_proc(run_params);
        }
        else {
            // Parent process
            pid = forkpid;

            bp_sys::close(pipefd[1]); // close the 'other end' fd
            if (control_socket[1] != -1) bp_sys::close(control_socket[1]);
            if (notify_pipe[1] != -1) bp_sys::close(notify_pipe[1]);
            notification_fd = notify_pipe[0];
            waiting_for_execstat = true;
            return true;
        }

        // Failure exit:

        out_cs_h:
        if (child_status_registered) {
            child_status_listener.deregister(event_loop);
        }

        if (notify_pipe[0] != -1) bp_sys::close(notify_pipe[0]);
        if (notify_pipe[1] != -1) bp_sys::close(notify_pipe[1]);
        if (ready_watcher_registered) {
            rwatcher->deregister(event_loop);
        }

        if (onstart_flags.pass_cs_fd) {
            delete control_conn;

            out_cs:
            bp_sys::close(control_socket[0]);
            bp_sys::close(control_socket[1]);
        }
    }

    out_lfd:
    if (log_input_fd != -1) {
        log_output_listener.deregister(event_loop);
        bp_sys::close(log_input_fd);
        bp_sys::close(log_output_fd);
        log_input_fd = -1;
        log_output_fd = -1;
    }

    out_p:
    bp_sys::close(pipefd[0]);
    bp_sys::close(pipefd[1]);

    return false;
}

base_process_service::base_process_service(service_set *sset, string name,
        service_type_t service_type_p, ha_string &&command,
        const std::list<std::pair<unsigned,unsigned>> &command_offsets,
        const std::list<prelim_dep> &deplist_p)
     : service_record(sset, name, service_type_p, deplist_p), child_listener(this),
       child_status_listener(this), process_timer(this), log_output_listener(this)
{
    program_name = std::move(command);
    exec_arg_parts = separate_args(program_name, command_offsets);

    restart_interval_count = 0;
    restart_interval_time = {0, 0};
    process_timer.service = this;
    process_timer.add_timer(event_loop);

    // By default, allow a maximum of 3 restarts within 10.0 seconds:
    restart_interval.seconds() = 10;
    restart_interval.nseconds() = 0;
    max_restart_interval_count = 3;

    waiting_restart_timer = false;
    waiting_stopstart_timer = false;
    reserved_child_watch = false;
    tracking_child = false;
}

void base_process_service::do_restart() noexcept
{
    // Actually perform process restart. We may be in smooth recovery (state = STARTED) or this may
    // be a regular restart.

    waiting_restart_timer = false;
    auto service_state = get_state();

    if (!start_ps_process(exec_arg_parts, have_console || onstart_flags.shares_console)) {
        if (service_state == service_state_t::STARTING) {
            failed_to_start();
        }
        else {
            // smooth recovery failure
            unrecoverable_stop();
        }
        services->process_queues();
    }
    else {
        // started process successfully (at least as far as fork)
        if (start_timeout != time_val(0,0)) {
            process_timer.arm_timer_rel(event_loop, start_timeout);
            waiting_stopstart_timer = true;
        }
    }
}

bool base_process_service::restart_ps_process() noexcept
{
    using time_val = dasynq::time_val;

    time_val current_time;
    event_loop.get_time(current_time, clock_type::MONOTONIC);

    // Check if enough time has lapsed since the previous restart. If not, start a timer:
    time_val tdiff = current_time - last_start_time;
    if (restart_delay <= tdiff) {
        // > restart delay (normally 200ms)
        do_restart();
    }
    else {
        time_val timeout = restart_delay - tdiff;
        process_timer.arm_timer_rel(event_loop, timeout);
        waiting_restart_timer = true;
    }
    return true;
}

bool base_process_service::interrupt_start() noexcept
{
    if (waiting_restart_timer) {
        process_timer.stop_timer(event_loop);
        waiting_restart_timer = false;
        return service_record::interrupt_start();
    }
    else {
        log(loglevel_t::WARN, "Interrupting start of service ", get_name(), " with pid ", pid,
                " (with SIGINT).");
        kill_pg(SIGINT);

        if (stop_timeout != time_val(0,0)) {
            process_timer.arm_timer_rel(event_loop, stop_timeout);
            waiting_stopstart_timer = true;
        }
        else if (waiting_stopstart_timer) {
            process_timer.stop_timer(event_loop);
            waiting_stopstart_timer = false;
        }

        set_state(service_state_t::STOPPING);
        return false;
    }
}

void base_process_service::kill_with_fire() noexcept
{
    if (pid != -1) {
        log(loglevel_t::WARN, "Service ", get_name(), " with pid ", pid,
                " exceeded allowed stop time; killing.");
        kill_pg(SIGKILL);
    }
}

void base_process_service::kill_pg(int signo) noexcept
{
    if (onstart_flags.signal_process_only) {
        bp_sys::kill(pid, signo);
    }
    else {
        pid_t pgid = bp_sys::getpgid(pid);
        if (pgid == -1) {
            // On some OSes (eg OpenBSD) we aren't allowed to get the pgid of a process in a different
            // session. If the process is in a different session, however, it must be a process group
            // leader and the pgid must equal the process id.
            pgid = pid;
        }
        bp_sys::kill(-pgid, signo);
    }
}

void base_process_service::timer_expired() noexcept
{
    waiting_stopstart_timer = false;

    // Timer expires if:
    // We are stopping, including after having startup cancelled (stop timeout, state is STOPPING); We are
    // starting (start timeout, state is STARTING); We are waiting for restart timer before restarting,
    // including smooth recovery (restart timeout, state is STARTING or STARTED).
    if (get_state() == service_state_t::STOPPING) {
        kill_with_fire();
    }
    else if (pid != -1) {
        // Starting, start timed out.
        log(loglevel_t::WARN, "Service ", get_name(), " with pid ", pid,
                " exceeded allowed start time; cancelling.");
        interrupt_start();
        stop_reason = stopped_reason_t::TIMEDOUT;
        failed_to_start(false, false);
    }
    else {
        // STARTING / STARTED, and we have no pid: must be restarting (smooth recovery if STARTED)
        do_restart();
    }
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
            log(loglevel_t::ERROR, get_name(), ": activation socket file exists (and is not a socket)");
            return false;
        }
    }
    else if (errno != ENOENT) {
        // Other error
        log(loglevel_t::ERROR, get_name(), ": error checking activation socket: ", strerror(errno));
        return false;
    }

    // Remove stale socket file (if it exists).
    // We won't test the return from unlink - if it fails other than due to ENOENT, we should get an
    // error when we try to create the socket anyway.
    unlink(saddrname);

    uint sockaddr_size = offsetof(struct sockaddr_un, sun_path) + socket_path.length() + 1;
    struct sockaddr_un * name = static_cast<sockaddr_un *>(malloc(sockaddr_size));
    if (name == nullptr) {
        log(loglevel_t::ERROR, get_name(), ": opening activation socket: out of memory");
        return false;
    }

    name->sun_family = AF_UNIX;
    strcpy(name->sun_path, saddrname);

    int sockfd = dinit_socket(AF_UNIX, SOCK_STREAM, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (sockfd == -1) {
        log(loglevel_t::ERROR, get_name(), ": error creating activation socket: ", strerror(errno));
        free(name);
        return false;
    }

    if (bind(sockfd, (struct sockaddr *) name, sockaddr_size) == -1) {
        log(loglevel_t::ERROR, get_name(), ": error binding activation socket: ", strerror(errno));
        close(sockfd);
        free(name);
        return false;
    }

    free(name);

    // POSIX (1003.1, 2013) says that fchown and fchmod don't necessarily work on sockets. We have to
    // use chown and chmod instead.
    if (chown(saddrname, socket_uid, socket_gid)) {
        log(loglevel_t::ERROR, get_name(), ": error setting activation socket owner/group: ",
                strerror(errno));
        close(sockfd);
        return false;
    }

    if (chmod(saddrname, socket_perms) == -1) {
        log(loglevel_t::ERROR, get_name(), ": Error setting activation socket permissions: ",
                strerror(errno));
        close(sockfd);
        return false;
    }

    if (listen(sockfd, 128) == -1) { // 128 "seems reasonable".
        log(loglevel_t::ERROR, ": error listening on activation socket: ", strerror(errno));
        close(sockfd);
        return false;
    }

    socket_fd = sockfd;
    return true;
}

bool base_process_service::ensure_log_buffer_backing(unsigned new_size) noexcept
{
    //  Note: we manage capacity manually to avoid it exceeding maximum
    if (log_buffer.size() < new_size) {
        if (log_buffer.capacity() < new_size) {
            try {
                unsigned new_capacity = std::max((unsigned)log_buffer.capacity() * 2, new_size);
                new_capacity = std::min(new_capacity, log_buf_max);
                log_buffer.reserve(new_capacity);
                log_buffer.resize(new_capacity);
            }
            catch (std::bad_alloc &badalloc) {
                log(loglevel_t::WARN, get_name(), ": cannot increase log buffer; out-of-memory");
                return false;
            }
        }
        else {
            log_buffer.resize(new_size);
        }
    }
    return true;
}
