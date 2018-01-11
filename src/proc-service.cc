#include <sys/un.h>
#include <sys/socket.h>

#include "dinit-socket.h"
#include "dinit-util.h"
#include "proc-service.h"

extern eventloop_t event_loop;

using clock_type = dasynq::clock_type;
using rearm = dasynq::rearm;
using time_val = dasynq::time_val;

// Given a string and a list of pairs of (start,end) indices for each argument in that string,
// store a null terminator for the argument. Return a `char *` vector containing the beginning
// of each argument and a trailing nullptr. (The returned array is invalidated if the string is later modified).
std::vector<const char *> separate_args(std::string &s, std::list<std::pair<unsigned,unsigned>> &arg_indices)
{
    std::vector<const char *> r;
    r.reserve(arg_indices.size() + 1);

    // First store nul terminator for each part:
    for (auto index_pair : arg_indices) {
        if (index_pair.second < s.length()) {
            s[index_pair.second] = 0;
        }
    }

    // Now we can get the C string (c_str) and store offsets into it:
    const char * cstr = s.c_str();
    for (auto index_pair : arg_indices) {
        r.push_back(cstr + index_pair.first);
    }
    r.push_back(nullptr);
    return r;
}

void process_service::exec_succeeded() noexcept
{
    // This could be a smooth recovery (state already STARTED). Even more, the process
    // might be stopped (and killed via a signal) during smooth recovery.  We don't to
    // process startup again in either case, so we check for state STARTING:
    if (get_state() == service_state_t::STARTING) {
        started();
    }
    else if (get_state() == service_state_t::STOPPING) {
        // stopping, but smooth recovery was in process. That's now over so we can
        // commence normal stop. Note that if pid == -1 the process already stopped(!),
        // that's handled below.
        if (pid != -1 && stop_check_dependents()) {
            bring_down();
        }
    }
}

rearm exec_status_pipe_watcher::fd_event(eventloop_t &loop, int fd, int flags) noexcept
{
    base_process_service *sr = service;
    sr->waiting_for_execstat = false;

    int exec_status;
    int r = read(get_watched_fd(), &exec_status, sizeof(int));
    deregister(loop);
    close(get_watched_fd());

    if (r > 0) {
        // We read an errno code; exec() failed, and the service startup failed.
        if (sr->pid != -1) {
            sr->child_listener.deregister(event_loop, sr->pid);
            sr->reserved_child_watch = false;
            if (sr->stop_timer_armed) {
                sr->restart_timer.stop_timer(loop);
                sr->stop_timer_armed = false;
            }
        }
        sr->pid = -1;
        sr->exec_failed(exec_status);
    }
    else {
        sr->exec_succeeded();

        if (sr->pid == -1) {
            // Somehow the process managed to complete before we even saw the exec() status.
            sr->handle_exit_status(sr->exit_status);
        }
    }

    sr->services->process_queues();

    return rearm::REMOVED;
}

dasynq::rearm service_child_watcher::status_change(eventloop_t &loop, pid_t child, int status) noexcept
{
    base_process_service *sr = service;

    sr->pid = -1;
    sr->exit_status = status;

    // Ok, for a process service, any process death which we didn't rig
    // ourselves is a bit... unexpected. Probably, the child died because
    // we asked it to (sr->service_state == STOPPING). But even if
    // we didn't, there's not much we can do.

    if (sr->waiting_for_execstat) {
        // We still don't have an exec() status from the forked child, wait for that
        // before doing any further processing.
        return dasynq::rearm::NOOP; // hold watch reservation
    }

    // Must stop watch now since handle_exit_status might result in re-launch:
    // (stop_watch instead of deregister, so that we hold watch reservation).
    stop_watch(loop);

    if (sr->stop_timer_armed) {
        sr->restart_timer.stop_timer(loop);
        sr->stop_timer_armed = false;
    }

    sr->handle_exit_status(status);
    return dasynq::rearm::NOOP;
}

void process_service::handle_exit_status(int exit_status) noexcept
{
    bool did_exit = WIFEXITED(exit_status);
    bool was_signalled = WIFSIGNALED(exit_status);
    restarting = false;
    auto service_state = get_state();

    if (exit_status != 0 && service_state != service_state_t::STOPPING) {
        if (did_exit) {
            log(loglevel_t::ERROR, "Service ", get_name(), " process terminated with exit code ",
                    WEXITSTATUS(exit_status));
        }
        else if (was_signalled) {
            log(loglevel_t::ERROR, "Service ", get_name(), " terminated due to signal ",
                    WTERMSIG(exit_status));
        }
    }

    if (service_state == service_state_t::STARTING) {
        if (did_exit && WEXITSTATUS(exit_status) == 0) {
            started();
        }
        else {
            failed_to_start();
        }
    }
    else if (service_state == service_state_t::STOPPING) {
        // We won't log a non-zero exit status or termination due to signal here -
        // we assume that the process died because we signalled it.
        stopped();
    }
    else if (smooth_recovery && service_state == service_state_t::STARTED
            && get_target_state() == service_state_t::STARTED) {
        do_smooth_recovery();
        return;
    }
    else {
        emergency_stop();
    }
    services->process_queues();
}

void process_service::exec_failed(int errcode) noexcept
{
    log(loglevel_t::ERROR, get_name(), ": execution failed: ", strerror(errcode));
    if (get_state() == service_state_t::STARTING) {
        failed_to_start();
    }
    else {
        // Process service in smooth recovery:
        emergency_stop();
    }
}

void bgproc_service::handle_exit_status(int exit_status) noexcept
{
    begin:
    bool did_exit = WIFEXITED(exit_status);
    bool was_signalled = WIFSIGNALED(exit_status);
    auto service_state = get_state();

    if (exit_status != 0 && service_state != service_state_t::STOPPING) {
        if (did_exit) {
            log(loglevel_t::ERROR, "Service ", get_name(), " process terminated with exit code ",
                    WEXITSTATUS(exit_status));
        }
        else if (was_signalled) {
            log(loglevel_t::ERROR, "Service ", get_name(), " terminated due to signal ",
                    WTERMSIG(exit_status));
        }
    }

    // This may be a "smooth recovery" where we are restarting the process while leaving the
    // service in the STARTED state.
    if (restarting && service_state == service_state_t::STARTED) {
        restarting = false;
        bool need_stop = false;
        if ((did_exit && WEXITSTATUS(exit_status) != 0) || was_signalled) {
            need_stop = true;
        }
        else {
            // We need to re-read the PID, since it has now changed.
            if (pid_file.length() != 0) {
                auto pid_result = read_pid_file(&exit_status);
                switch (pid_result) {
                    case pid_result_t::FAILED:
                        // Failed startup: no auto-restart.
                        need_stop = true;
                        break;
                    case pid_result_t::TERMINATED:
                        goto begin;
                    case pid_result_t::OK:
                        break;
                }
            }
        }

        if (need_stop) {
            // Failed startup: no auto-restart.
            emergency_stop();
            services->process_queues();
        }

        return;
    }

    restarting = false;
    if (service_state == service_state_t::STARTING) {
        // POSIX requires that if the process exited clearly with a status code of 0,
        // the exit status value will be 0:
        if (exit_status == 0) {
            auto pid_result = read_pid_file(&exit_status);
            switch (pid_result) {
                case pid_result_t::FAILED:
                    // Failed startup: no auto-restart.
                    failed_to_start();
                    break;
                case pid_result_t::TERMINATED:
                    // started, but immediately terminated
                    started();
                    goto begin;
                case pid_result_t::OK:
                    started();
                    break;
            }
        }
        else {
            failed_to_start();
        }
    }
    else if (service_state == service_state_t::STOPPING) {
        // We won't log a non-zero exit status or termination due to signal here -
        // we assume that the process died because we signalled it.
        stopped();
    }
    else {
        // we must be STARTED
        if (smooth_recovery && get_target_state() == service_state_t::STARTED) {
            do_smooth_recovery();
            return;
        }
        if (! do_auto_restart() && start_explicit) {
            start_explicit = false;
            release();
        }
        forced_stop();
        stop_dependents();
        stopped();
    }
    services->process_queues();
}

void bgproc_service::exec_failed(int errcode) noexcept
{
    log(loglevel_t::ERROR, get_name(), ": execution failed: ", strerror(errcode));
    // Only time we execute is for startup:
    failed_to_start();
}

void scripted_service::handle_exit_status(int exit_status) noexcept
{
    bool did_exit = WIFEXITED(exit_status);
    bool was_signalled = WIFSIGNALED(exit_status);
    auto service_state = get_state();

    // For a scripted service, a termination occurs in one of three main cases:
    // - the start script completed (or failed), when service was STARTING
    // - the start script was interrupted to cancel startup; state is STOPPING
    // - the stop script complete (or failed), state is STOPPING

    if (service_state == service_state_t::STOPPING) {
        // We might be running the stop script, or we might be running the start script and have issued
        // a cancel order via SIGINT:
        if (did_exit && WEXITSTATUS(exit_status) == 0) {
            if (interrupting_start) {
                interrupting_start = false;
                // launch stop script:
                bring_down();
            }
            else {
                // We were running the stop script and finished successfully
                stopped();
            }
        }
        else {
            if (interrupting_start) {
                // We issued a start interrupt, so we expected this failure:
                if (did_exit) {
                    log(loglevel_t::INFO, "Service ", get_name(), " start cancelled; exit code ",
                            WEXITSTATUS(exit_status));
                }
                else if (was_signalled) {
                    log(loglevel_t::INFO, "Service ", get_name(), " start cancelled from signal ",
                            WTERMSIG(exit_status));
                }
            }
            else {
                // ??? failed to stop! Let's log it as warning:
                if (did_exit) {
                    log(loglevel_t::WARN, "Service ", get_name(), " stop command failed with exit code ",
                            WEXITSTATUS(exit_status));
                }
                else if (was_signalled) {
                    log(loglevel_t::WARN, "Service ", get_name(), " stop command terminated due to signal ",
                            WTERMSIG(exit_status));
                }
            }
            // Even if the stop script failed, assume that service is now stopped, so that any dependencies
            // can be stopped. There's not really any other useful course of action here.
            interrupting_start = false;
            stopped();
        }
        services->process_queues();
    }
    else { // STARTING
        if (exit_status == 0) {
            started();
        }
        else {
            // failed to start
            if (did_exit) {
                log(loglevel_t::ERROR, "Service ", get_name(), " command failed with exit code ",
                        WEXITSTATUS(exit_status));
            }
            else if (was_signalled) {
                log(loglevel_t::ERROR, "Service ", get_name(), " command terminated due to signal ",
                        WTERMSIG(exit_status));
            }
            failed_to_start();
        }
        services->process_queues();
    }
}

void scripted_service::exec_failed(int errcode) noexcept
{
    log(loglevel_t::ERROR, get_name(), ": execution failed: ", strerror(errcode));
    auto service_state = get_state();
    if (service_state == service_state_t::STARTING) {
        failed_to_start();
    }
    else if (service_state == service_state_t::STOPPING) {
        // We've logged the failure, but it's probably better not to leave the service in
        // STOPPING state:
        stopped();
    }
}

bgproc_service::pid_result_t
bgproc_service::read_pid_file(int *exit_status) noexcept
{
    const char *pid_file_c = pid_file.c_str();
    int fd = open(pid_file_c, O_CLOEXEC);
    if (fd == -1) {
        log(loglevel_t::ERROR, get_name(), ": read pid file: ", strerror(errno));
        return pid_result_t::FAILED;
    }

    char pidbuf[21]; // just enough to hold any 64-bit integer
    int r = ss_read(fd, pidbuf, 20);
    if (r < 0) {
        // Could not read from PID file
        log(loglevel_t::ERROR, get_name(), ": could not read from pidfile; ", strerror(errno));
        close(fd);
        return pid_result_t::FAILED;
    }

    close(fd);
    pidbuf[r] = 0; // store nul terminator

    bool valid_pid = false;
    try {
        unsigned long long v = std::stoull(pidbuf, nullptr, 0);
        if (v <= std::numeric_limits<pid_t>::max()) {
            pid = (pid_t) v;
            valid_pid = true;
        }
    }
    catch (std::out_of_range &exc) {
        // Too large?
    }
    catch (std::invalid_argument &exc) {
        // Ok, so it doesn't look like a number: proceed...
    }

    if (valid_pid) {
        pid_t wait_r = waitpid(pid, exit_status, WNOHANG);
        if (wait_r == -1 && errno == ECHILD) {
            // We can't track this child - check process exists:
            if (kill(pid, 0) == 0 || errno != ESRCH) {
                tracking_child = false;
                return pid_result_t::OK;
            }
            else {
                log(loglevel_t::ERROR, get_name(), ": pid read from pidfile (", pid, ") is not valid");
                pid = -1;
                return pid_result_t::FAILED;
            }
        }
        else if (wait_r == pid) {
            pid = -1;
            return pid_result_t::TERMINATED;
        }
        else if (wait_r == 0) {
            // We can track the child
            child_listener.add_reserved(event_loop, pid, dasynq::DEFAULT_PRIORITY - 10);
            tracking_child = true;
            reserved_child_watch = true;
            return pid_result_t::OK;
        }
    }

    log(loglevel_t::ERROR, get_name(), ": pid read from pidfile (", pid, ") is not valid");
    pid = -1;
    return pid_result_t::FAILED;
}

void process_service::bring_down() noexcept
{
    waiting_for_deps = false;
    if (waiting_for_execstat) {
        // The process is still starting. This should be uncommon, but can occur during
        // smooth recovery. We can't do much now; we have to wait until we get the
        // status, and then act appropriately.
        return;
    }
    else if (pid != -1) {
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

void scripted_service::bring_down() noexcept
{
    waiting_for_deps = false;
    if (stop_command.length() == 0) {
        stopped();
    }
    else if (! start_ps_process(stop_arg_parts, false)) {
        // Couldn't execute stop script, but there's not much we can do:
        stopped();
    }
    else {
        // successfully started stop script: start kill timer:
        if (stop_timeout != time_val(0,0)) {
            restart_timer.arm_timer_rel(event_loop, stop_timeout);
            stop_timer_armed = true;
        }
    }
}

dasynq::rearm process_restart_timer::timer_expiry(eventloop_t &, int expiry_count)
{
    service->stop_timer_armed = false;

    // Timer expires if:
    // We are stopping, including after having startup cancelled (stop timeout, state is STOPPING); We are
    // starting (start timeout, state is STARTING); We are waiting for restart timer before restarting,
    // including smooth recovery (restart timeout, state is STARTING or STARTED).
    if (service->get_state() == service_state_t::STOPPING) {
        service->kill_with_fire();
    }
    else if (service->pid != -1) {
        // Starting, start timed out.
        service->stop_dependents();
        service->interrupt_start();
    }
    else {
        // STARTING / STARTED, and we have a pid: must be restarting (smooth recovery if STARTED)
        service->do_restart();
    }

    // Leave the timer disabled, or, if it has been reset by any processing above, leave it armed:
    return dasynq::rearm::NOOP;
}
