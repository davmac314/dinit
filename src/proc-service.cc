#include <cstring>
#include <type_traits>

#include <sys/un.h>
#include <sys/socket.h>

#include "dinit.h"
#include "dinit-socket.h"
#include "dinit-util.h"
#include "dinit-log.h"
#include "proc-service.h"

/*
 * Most of the implementation for process-based services (process, scripted, bgprocess) is here.
 *
 * See proc-service.h header for interface details.
 */

// Given a string and a list of pairs of (start,end) indices for each argument in that string,
// store a null terminator for the argument. Return a `char *` vector containing the beginning
// of each argument and a trailing nullptr. (The returned array is invalidated if the string is
// later modified).
std::vector<const char *> separate_args(std::string &s,
        const std::list<std::pair<unsigned,unsigned>> &arg_indices)
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
        if (force_notification_fd != -1 || !notification_var.empty()) {
            // Wait for readiness notification:
            readiness_watcher.set_enabled(event_loop, true);
        }
        else {
            if (waiting_stopstart_timer) {
                process_timer.stop_timer(event_loop);
                waiting_stopstart_timer = false;
            }
            started();
        }
    }
    else if (get_state() == service_state_t::STOPPING) {
        // stopping, but smooth recovery was in process. That's now over so we can
        // commence normal stop. Note that if pid == -1 the process already stopped,
        // that is correctly handled by bring_down().
        if (stop_check_dependents()) {
            bring_down();
        }
    }
}

void scripted_service::exec_succeeded() noexcept
{
	// For a scripted service, this means nothing other than that the start/stop
	// script will now begin.
}

rearm exec_status_pipe_watcher::fd_event(eventloop_t &loop, int fd, int flags) noexcept
{
    base_process_service *sr = service;
    sr->waiting_for_execstat = false;

    run_proc_err exec_status;
    int r = read(get_watched_fd(), &exec_status, sizeof(exec_status));
    deregister(loop);
    close(get_watched_fd());

    if (r > 0) {
        // We read an errno code; exec() failed, and the service startup failed.
        if (sr->pid != -1) {
            sr->child_listener.deregister(event_loop, sr->pid);
            sr->reserved_child_watch = false;
            if (sr->waiting_stopstart_timer) {
                sr->process_timer.stop_timer(loop);
                sr->waiting_stopstart_timer = false;
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

rearm ready_notify_watcher::fd_event(eventloop_t &, int fd, int flags) noexcept
{
    char buf[128];
    if (service->get_state() == service_state_t::STARTING) {
        // can we actually read anything from the notification pipe?
        int r = bp_sys::read(fd, buf, sizeof(buf));
        if (r > 0) {
            if (service->waiting_stopstart_timer) {
                service->process_timer.stop_timer(event_loop);
                service->waiting_stopstart_timer = false;
            }
            service->started();
        }
        else if (r == 0 || errno != EAGAIN) {
            if (service->waiting_stopstart_timer) {
                service->process_timer.stop_timer(event_loop);
                service->waiting_stopstart_timer = false;
            }
            service->failed_to_start(false, false);
            service->set_state(service_state_t::STOPPING);
            service->bring_down();
        }
    }
    else {
        // Just keep consuming data from the pipe:
        int r = bp_sys::read(fd, buf, sizeof(buf));
        if (r == 0) {
            // Process closed write end or terminated
            close(fd);
            service->notification_fd = -1;
            return rearm::DISARM;
        }
    }

    service->services->process_queues();
    return rearm::REARM;
}

dasynq::rearm service_child_watcher::status_change(eventloop_t &loop, pid_t child, int status) noexcept
{
    base_process_service *sr = service;

    sr->pid = -1;
    sr->exit_status = bp_sys::exit_status(status);

    // Ok, for a process service, any process death which we didn't rig ourselves is a bit... unexpected.
    // Probably, the child died because we asked it to (sr->service_state == STOPPING). But even if we
    // didn't, there's not much we can do.

    if (sr->waiting_for_execstat) {
        // We still don't have an exec() status from the forked child, wait for that
        // before doing any further processing.
        return dasynq::rearm::NOOP; // hold watch reservation
    }

    // Must stop watch now since handle_exit_status might result in re-launch:
    // (stop_watch instead of deregister, so that we hold watch reservation).
    stop_watch(loop);

    if (sr->waiting_stopstart_timer) {
        sr->process_timer.stop_timer(loop);
        sr->waiting_stopstart_timer = false;
    }

    sr->handle_exit_status(bp_sys::exit_status(status));
    return dasynq::rearm::NOOP;
}

void process_service::handle_exit_status(bp_sys::exit_status exit_status) noexcept
{
    bool did_exit = exit_status.did_exit();
    bool was_signalled = exit_status.was_signalled();
    auto service_state = get_state();

    if (notification_fd != -1) {
        readiness_watcher.deregister(event_loop);
        bp_sys::close(notification_fd);
        notification_fd = -1;
    }

    if (!exit_status.did_exit_clean() && service_state != service_state_t::STOPPING) {
        if (did_exit) {
            log(loglevel_t::ERROR, "Service ", get_name(), " process terminated with exit code ",
                    exit_status.get_exit_status());
        }
        else if (was_signalled) {
            log(loglevel_t::ERROR, "Service ", get_name(), " terminated due to signal ",
                    exit_status.get_term_sig());
        }
    }

    if (waiting_stopstart_timer) {
        process_timer.stop_timer(event_loop);
        waiting_stopstart_timer = false;
    }

#if USE_UTMPX
    if (*inittab_id || *inittab_line) {
        clear_utmp_entry(inittab_id, inittab_line);
    }
#endif

    if (service_state == service_state_t::STARTING) {
        // If state is STARTING, we must be waiting for readiness notification; the process has
        // terminated before becoming ready.
        stop_reason = stopped_reason_t::FAILED;
        failed_to_start();
    }
    else if (service_state == service_state_t::STOPPING) {
        // We won't log a non-zero exit status or termination due to signal here -
        // we assume that the process died because we signalled it.
        if (waiting_stopstart_timer) {
            process_timer.stop_timer(event_loop);
        }
        stopped();
    }
    else if (smooth_recovery && service_state == service_state_t::STARTED) {
        // unexpected termination, with smooth recovery
        do_smooth_recovery();
        return;
    }
    else {
        handle_unexpected_termination();
    }
    services->process_queues();
}

void process_service::exec_failed(run_proc_err errcode) noexcept
{
    log(loglevel_t::ERROR, get_name(), ": execution failed - ",
            exec_stage_descriptions[static_cast<int>(errcode.stage)], ": ", strerror(errcode.st_errno));

    if (waiting_stopstart_timer) {
        process_timer.stop_timer(event_loop);
        waiting_stopstart_timer = false;
    }

    if (notification_fd != -1) {
        readiness_watcher.deregister(event_loop);
        bp_sys::close(notification_fd);
        notification_fd = -1;
    }

    if (get_state() == service_state_t::STARTING) {
        stop_reason = stopped_reason_t::EXECFAILED;
        failed_to_start();
    }
    else {
        // Process service in smooth recovery:
        stop_reason = stopped_reason_t::TERMINATED;
        unrecoverable_stop();
    }
}

void bgproc_service::handle_exit_status(bp_sys::exit_status exit_status) noexcept
{
    // For bgproc services, receiving exit status can mean one of two things:
    // 1. We were launching the process, and it finished (possibly after forking). If it did fork
    //    we want to obtain the process id of the process that we should now monitor, the actual
    //    daemon.
    // 2. The above has already happened, and we are monitoring the daemon process, which has now
    //    terminated for some reason.

    begin:
    bool did_exit = exit_status.did_exit();
    bool was_signalled = exit_status.was_signalled();
    auto service_state = get_state();

    if (!exit_status.did_exit_clean() && service_state != service_state_t::STOPPING) {
        if (did_exit) {
            log(loglevel_t::ERROR, "Service ", get_name(), " process terminated with exit code ",
                    exit_status.get_exit_status());
        }
        else if (was_signalled) {
            log(loglevel_t::ERROR, "Service ", get_name(), " terminated due to signal ",
                    exit_status.get_term_sig());
        }
    }

    if (waiting_stopstart_timer) {
        process_timer.stop_timer(event_loop);
        waiting_stopstart_timer = false;
    }

    if (doing_smooth_recovery) {
        doing_smooth_recovery = false;

        // We're either started, or stopping (i.e. we were requested to stop during smooth recovery).

        if (service_state == service_state_t::STOPPING) {
            // Stop was issued during smooth recovery
            if ((did_exit && exit_status.get_exit_status() != 0) || was_signalled) {
                stopped();
            }
            else {
                // We need to re-read the PID, since it has now changed.
                if (pid_file.length() != 0) {
                    auto pid_result = read_pid_file(&exit_status);

                    switch (pid_result) {
                    case pid_result_t::FAILED:
                    case pid_result_t::TERMINATED:
                        // Failed startup: no auto-restart.
                        stopped();
                        break;
                    case pid_result_t::OK:
                        // We now need to bring down the daemon process
                        bring_down();
                        return;
                    }
                }
            }
            services->process_queues();
            return;
        }
        else /* if (service_state == service_state_t::STARTED) */ {
            bool need_stop = false;
            if ((did_exit && exit_status.get_exit_status() != 0) || was_signalled) {
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
                stop_reason = stopped_reason_t::TERMINATED;
                unrecoverable_stop();
                services->process_queues();
            }

            return;
        }
    }

    if (service_state == service_state_t::STARTING) {
        // POSIX requires that if the process exited clearly with a status code of 0,
        // the exit status value will be 0:
        if (exit_status.did_exit_clean()) {
            auto pid_result = read_pid_file(&exit_status);
            switch (pid_result) {
                case pid_result_t::FAILED:
                    // Failed startup: no auto-restart.
                    stop_reason = stopped_reason_t::FAILED;
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
            stop_reason = stopped_reason_t::FAILED;
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
            doing_smooth_recovery = true;
            do_smooth_recovery();
            if (get_state() != service_state_t::STARTED) {
                doing_smooth_recovery = false;
            }
            return;
        }
        handle_unexpected_termination();
    }
    services->process_queues();
}

void bgproc_service::exec_failed(run_proc_err errcode) noexcept
{
    log(loglevel_t::ERROR, get_name(), ": execution failed - ",
            exec_stage_descriptions[static_cast<int>(errcode.stage)], ": ", strerror(errcode.st_errno));

    // Only time we execute is for startup:
    stop_reason = stopped_reason_t::EXECFAILED;
    failed_to_start();
}

void scripted_service::handle_exit_status(bp_sys::exit_status exit_status) noexcept
{
    bool did_exit = exit_status.did_exit();
    bool was_signalled = exit_status.was_signalled();
    auto service_state = get_state();

    // For a scripted service, a termination occurs in one of three main cases:
    // - the start script completed (or failed), when service was STARTING
    // - the start script was interrupted to cancel startup; state is STOPPING
    // - the stop script complete (or failed), state is STOPPING

    if (service_state == service_state_t::STOPPING) {
        // We might be running the stop script, or we might be running the start script and have issued
        // a cancel order via SIGINT:
        if (interrupting_start) {
            if (waiting_stopstart_timer) {
                process_timer.stop_timer(event_loop);
                waiting_stopstart_timer = false;
            }
            // We issued a start interrupt, so we expected this failure:
            if (did_exit && exit_status.get_exit_status() != 0) {
                log(loglevel_t::NOTICE, "Service ", get_name(), " start cancelled; exit code ",
                        exit_status.get_exit_status());
                // Assume that a command terminating normally (with failure status) requires no cleanup:
                stopped();
            }
            else {
                if (was_signalled) {
                    log(loglevel_t::NOTICE, "Service ", get_name(), " start cancelled from signal ",
                            exit_status.get_term_sig());
                }
                // If the start script completed successfully, or was interrupted via our signal,
                // we want to run the stop script to clean up:
                bring_down();
            }
            interrupting_start = false;
        }
        else if (exit_status.did_exit_clean()) {
            // We were running the stop script and finished successfully
            stopped();
        }
        else {
            // ??? failed to stop! Let's log it as warning:
            if (did_exit) {
                log(loglevel_t::WARN, "Service ", get_name(), " stop command failed with exit code ",
                        exit_status.get_exit_status());
            }
            else if (was_signalled) {
                log(loglevel_t::WARN, "Service ", get_name(), " stop command terminated due to signal ",
                        exit_status.get_term_sig());
            }
            // Even if the stop script failed, assume that service is now stopped, so that any dependencies
            // can be stopped. There's not really any other useful course of action here.
            stopped();
        }
        services->process_queues();
    }
    else { // STARTING
        if (exit_status.did_exit_clean()) {
            started();
        }
        else if (was_signalled && exit_status.get_term_sig() == SIGINT && onstart_flags.skippable) {
            // A skippable service can be skipped by interrupting (eg by ^C if the service
            // starts on the console).
            start_skipped = true;
            started();
        }
        else {
            // failed to start
            if (did_exit) {
                log(loglevel_t::ERROR, "Service ", get_name(), " command failed with exit code ",
                        exit_status.get_exit_status());
            }
            else if (was_signalled) {
                log(loglevel_t::ERROR, "Service ", get_name(), " command terminated due to signal ",
                        exit_status.get_term_sig());
            }
            stop_reason = stopped_reason_t::FAILED;
            failed_to_start();
        }
        services->process_queues();
    }
}

void scripted_service::exec_failed(run_proc_err errcode) noexcept
{
    log(loglevel_t::ERROR, get_name(), ": execution failed - ",
            exec_stage_descriptions[static_cast<int>(errcode.stage)], ": ", strerror(errcode.st_errno));
    auto service_state = get_state();
    if (service_state == service_state_t::STARTING) {
        stop_reason = stopped_reason_t::EXECFAILED;
        failed_to_start();
    }
    else if (service_state == service_state_t::STOPPING) {
        // We've logged the failure, but it's probably better not to leave the service in
        // STOPPING state:
        stopped();
    }
}

// Return a value as an unsigned-type value.
template <typename T> typename std::make_unsigned<T>::type make_unsigned_val(T val)
{
    return static_cast<typename std::make_unsigned<T>::type>(val);
}

bgproc_service::pid_result_t
bgproc_service::read_pid_file(bp_sys::exit_status *exit_status) noexcept
{
    const char *pid_file_c = pid_file.c_str();
    int fd = bp_sys::open(pid_file_c, O_CLOEXEC);
    if (fd == -1) {
        log(loglevel_t::ERROR, get_name(), ": read pid file: ", strerror(errno));
        return pid_result_t::FAILED;
    }

    char pidbuf[21]; // just enough to hold any 64-bit integer
    int r = complete_read(fd, pidbuf, 20);
    if (r < 0) {
        // Could not read from PID file
        log(loglevel_t::ERROR, get_name(), ": could not read from pidfile; ", strerror(errno));
        bp_sys::close(fd);
        return pid_result_t::FAILED;
    }

    bp_sys::close(fd);
    pidbuf[r] = 0; // store nul terminator

    bool valid_pid = false;
    try {
        unsigned long long v = std::stoull(pidbuf, nullptr, 0);
        if (v <= make_unsigned_val(std::numeric_limits<pid_t>::max())) {
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
            if (bp_sys::kill(pid, 0) == 0 || errno != ESRCH) {
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
        if (term_signal != 0) {
            kill_pg(term_signal);
        }

        // If there's a stop timeout, arm the timer now:
        if (stop_timeout != time_val(0,0)) {
            process_timer.arm_timer_rel(event_loop, stop_timeout);
            waiting_stopstart_timer = true;
        }

        // The rest is done in handle_exit_status.
    }
    else {
        // The process is already dead.
        if (waiting_restart_timer) {
            process_timer.stop_timer(event_loop);
            waiting_restart_timer = false;
        }
        stopped();
    }
}

void bgproc_service::bring_down() noexcept
{
    if (pid != -1) {
        // The process is still kicking on - must actually kill it. We signal the process
        // group (-pid) rather than just the process as there's less risk then of creating
        // an orphaned process group:
        if (term_signal != 0) {
            kill_pg(SIGTERM);
        }

        // In most cases, the rest is done in handle_exit_status.
        // If we are a BGPROCESS and the process is not our immediate child, however, that
        // won't work - check for this now:
        if (! tracking_child) {
            stopped();
        }
        else if (stop_timeout != time_val(0,0)) {
            process_timer.arm_timer_rel(event_loop, stop_timeout);
            waiting_stopstart_timer = true;
        }
    }
    else {
        // The process is already dead.
        stopped();
    }
}

void scripted_service::bring_down() noexcept
{
	if (pid != -1) {
		// We're already running the stop script; nothing to do.
		return;
	}

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
            process_timer.arm_timer_rel(event_loop, stop_timeout);
            waiting_stopstart_timer = true;
        }
    }
}

dasynq::rearm process_restart_timer::timer_expiry(eventloop_t &, int expiry_count)
{
    service->timer_expired();

    // Leave the timer disabled, or, if it has been reset by any processing above, leave it armed:
    return dasynq::rearm::NOOP;
}
