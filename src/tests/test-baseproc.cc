#include "dinit.h"
#include "proc-service.h"

// This is a mock implementation of the base_process_service class, for testing purposes.

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
    //restart_timer.add_timer(event_loop);

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

bool base_process_service::bring_up() noexcept
{
    if (restarting) {
        if (pid == -1) {
            return restart_ps_process();
        }
        return true;
    }
    else {
        //event_loop.get_time(restart_interval_time, clock_type::MONOTONIC);
        restart_interval_count = 0;
        if (start_ps_process(exec_arg_parts, onstart_flags.starts_on_console)) {
            if (start_timeout != time_val(0,0)) {
                //restart_timer.arm_timer_rel(event_loop, start_timeout);
                stop_timer_armed = true;
            }
            else if (stop_timer_armed) {
                //restart_timer.stop_timer(event_loop);
                stop_timer_armed = false;
            }
            return true;
        }
        return false;
    }
}

void base_process_service::bring_down() noexcept
{
    waiting_for_deps = false;
    if (pid != -1) {
        // The process is still kicking on - must actually kill it. We signal the process
        // group (-pid) rather than just the process as there's less risk then of creating
        // an orphaned process group:
        if (! onstart_flags.no_sigterm) {
            //kill_pg(SIGTERM);
        }
        if (term_signal != -1) {
            //kill_pg(term_signal);
        }

        // In most cases, the rest is done in handle_exit_status.
        // If we are a BGPROCESS and the process is not our immediate child, however, that
        // won't work - check for this now:
        if (get_type() == service_type_t::BGPROCESS && ! tracking_child) {
            stopped();
        }
        else if (stop_timeout != time_val(0,0)) {
            //restart_timer.arm_timer_rel(event_loop, stop_timeout);
            stop_timer_armed = true;
        }
    }
    else {
        // The process is already dead.
        stopped();
    }
}

void base_process_service::do_smooth_recovery() noexcept
{
    if (! restart_ps_process()) {
        emergency_stop();
        services->process_queues();
    }
}

bool base_process_service::start_ps_process(const std::vector<const char *> &cmd, bool on_console) noexcept
{
    return false;
}

void base_process_service::kill_with_fire() noexcept
{
    if (pid != -1) {
        //log(loglevel_t::WARN, "Service ", get_name(), " with pid ", pid, " exceeded allowed stop time; killing.");
        //kill_pg(SIGKILL);
    }
}

void base_process_service::kill_pg(int signo) noexcept
{
    //pid_t pgid = getpgid(pid);
    //if (pgid == -1) {
        // only should happen if pid is invalid, which should never happen...
        //log(loglevel_t::ERROR, get_name(), ": can't signal process: ", strerror(errno));
        //return;
    //}
    //kill(-pgid, signo);
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
                //log(loglevel_t::ERROR, "Service ", get_name(), " restarting too quickly; stopping.");
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
        //restart_timer.arm_timer_rel(event_loop, timeout);
        waiting_restart_timer = true;
    }
    return true;
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

bool base_process_service::interrupt_start() noexcept
{
    if (waiting_restart_timer) {
        //restart_timer.stop_timer(event_loop);
        waiting_restart_timer = false;
        return service_record::interrupt_start();
    }
    else {
        //log(loglevel_t::WARN, "Interrupting start of service ", get_name(), " with pid ", pid, " (with SIGINT).");
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
        interrupt_start();
    }
    else {
        // STARTING / STARTED, and we have a pid: must be restarting (smooth recovery if STARTED)
        do_restart();
    }
}
