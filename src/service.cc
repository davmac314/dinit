#include <cstring>
#include <cerrno>
#include <iterator>
#include <memory>
#include <cstddef>

#include <sys/ioctl.h>
#include <fcntl.h>
#include <termios.h>

#include "dinit.h"
#include "service.h"
#include "dinit-log.h"
#include "dinit-socket.h"
#include "dinit-util.h"
#include "baseproc-sys.h"

/*
 * service.cc - Service management.
 * See service.h for details.
 */

// Find the requested service by name
static service_record * find_service(const std::list<service_record *> & records,
                                    const char *name) noexcept
{
    using std::list;
    list<service_record *>::const_iterator i = records.begin();
    for ( ; i != records.end(); ++i ) {
        if (strcmp((*i)->get_name().c_str(), name) == 0) {
            return *i;
        }
    }
    return nullptr;
}

service_record * service_set::find_service(const std::string &name) noexcept
{
    return ::find_service(records, name.c_str());
}

// Called when a service has actually stopped; dependents have stopped already, unless this stop
// is due to an unexpected process termination.
void service_record::stopped() noexcept
{
    if (have_console) {
        bp_sys::tcsetpgrp(0, bp_sys::getpgrp());
        release_console();
    }

    force_stop = false;

    // If we are to re-start, restarting should have been set true and desired_state should be STARTED.
    // (A restart could be cancelled via a separately issued stop, including via a shutdown).
    bool will_restart = desired_state == service_state_t::STARTED && !pinned_stopped;

    // If we won't restart, break soft dependencies now
    if (! will_restart) {
        for (auto dept : dependents) {
            if (!dept->is_hard()) {
                // waits-for or soft dependency:
                if (dept->waiting_on) {
                    dept->waiting_on = false;
                    dept->get_from()->dependency_started();
                }
                if (dept->holding_acq) {
                    dept->holding_acq = false;
                    // release without issuing stop, since we're called only when this
                    // service is already stopped/stopping:
                    release(false);
                }
            }
        }
    }

    for (auto & dependency : depends_on) {
        // we signal dependencies in case they are waiting for us to stop:
        dependency.get_to()->dependent_stopped();
    }

    service_state = service_state_t::STOPPED;

    if (will_restart) {
        // Desired state is "started".
        initiate_start();
    }
    else {
        becoming_inactive();

        if (start_explicit) {
            // If we were explicitly started, our required_by count must be at least 1. Use
            // release() to correctly release, mark inactive and release dependencies.
            start_explicit = false;
            release(false);
        }
        else if (required_by == 0) {
            // This can only be the case if we didn't have start_explicit, since required_by would
            // otherwise by non-zero. Since our release(s) above were with state != STOPPED, we now
            // must mark inactive (i.e. it won't have been done as part of the release).
            services->service_inactive(this);
        }
    }

    // Start failure will have been logged already, only log if we are stopped for other reasons:
    if (! start_failed) {
        log_service_stopped(service_name);

        // If this service chains to another, start the chained service now, if:
        // - this service self-terminated (rather than being stopped),
        // - ... successfully (i.e. exit code 0)
        // - this service won't restart, and
        // - a shutdown isn't in progress
        if ((onstart_flags.always_chain || (did_finish(stop_reason) && get_exit_status() == 0 && ! will_restart))
                && ! start_on_completion.empty() && ! services->is_shutting_down()) {
            try {
                auto chain_to = services->load_service(start_on_completion.c_str());
                chain_to->start();
            }
            catch (service_load_exc &sle) {
                log(loglevel_t::ERROR, "Couldn't chain to service ", start_on_completion, ": ",
                        "couldn't load ", sle.service_name, ": ", sle.exc_description);
            }
            catch (std::bad_alloc &bae) {
                log(loglevel_t::ERROR, "Couldn't chain to service ", start_on_completion,
                        ": Out of memory");
            }
        }
    }
    notify_listeners(service_event_t::STOPPED);
}

void service_record::require() noexcept
{
    if (required_by++ == 0) {
        if (service_state != service_state_t::STARTING && service_state != service_state_t::STARTED) {
            prop_start = true;
            services->add_prop_queue(this);
            // Note: pin is checked in start().
            // Require will be propagated to dependencies if/when the service actually starts.
        }
    }
}

void service_record::release(bool issue_stop) noexcept
{
    if (--required_by == 0) {
        if (service_state == service_state_t::STOPPING) {
            // If we are stopping but would have restarted, we now need to notify that the restart
            // has been cancelled. Other start-cancelled cases are handled by do_stop() (called
            // below).
            if (desired_state == service_state_t::STARTED && !pinned_started) {
                notify_listeners(service_event_t::STARTCANCELLED);
            }
        }
        desired_state = service_state_t::STOPPED;

        if (pinned_started) return;

        // Can stop, and can release dependencies now. We don't need to issue a release if
        // a require was pending though:
        prop_release = !prop_require;
        prop_require = false;
        if (prop_release && service_state != service_state_t::STOPPED) {
            services->add_prop_queue(this);
        }

        if (service_state != service_state_t::STOPPED && service_state != service_state_t::STOPPING
                && issue_stop) {
        	stop_reason = stopped_reason_t::NORMAL;
            do_stop();
        }
    }
}

void service_record::release_dependencies() noexcept
{
    for (auto & dependency : depends_on) {
        service_record * dep_to = dependency.get_to();
        if (dependency.holding_acq) {
            // We must clear holding_acq before calling release, otherwise the dependency
            // may decide to stop, check this link and release itself a second time.
            dependency.holding_acq = false;
            dep_to->release();
        }
    }
}

void service_record::start() noexcept
{
    if (pinned_stopped) {
        // bail out early for this case, we don't want to set start_explicit
        return;
    }

    if (!start_explicit) {
        ++required_by;
        start_explicit = true;
    }

    do_start();
}

void service_record::initiate_start() noexcept
{
    start_failed = false;
    start_skipped = false;
    service_state = service_state_t::STARTING;
    waiting_for_deps = true;

    if (start_check_dependencies()) {
        waiting_for_deps = false;
        services->add_transition_queue(this);
    }
}

void service_record::do_propagation() noexcept
{
    if (prop_require) {
        // Need to require all our dependencies
        for (auto & dep : depends_on) {
            if (!dep.is_only_ordering()) {
                dep.get_to()->require();
                dep.holding_acq = true;
            }
        }
        prop_require = false;
    }

    if (prop_release) {
        release_dependencies();
        prop_release = false;
    }

    if (prop_failure) {
        prop_failure = false;
        stop_reason = stopped_reason_t::DEPFAILED;
        service_state = service_state_t::STOPPED;
        failed_to_start(true);
    }

    if (prop_start) {
        prop_start = false;
        do_start();
    }

    if (prop_stop) {
        prop_stop = false;
        do_stop();
    }
}

void service_record::execute_transition() noexcept
{
    if (service_state == service_state_t::STARTING) {
        if (check_deps_started()) {
            all_deps_started();
        }
    }
    else if (service_state == service_state_t::STOPPING) {
        if (stop_check_dependents()) {
            waiting_for_deps = false;
            bring_down();
        }
    }
}

void service_record::do_start() noexcept
{
    bool was_active = service_state != service_state_t::STOPPED;

    desired_state = service_state_t::STARTED;

    if (pinned_stopped) {
        if (!was_active) {
            failed_to_start(false, false);
        }
        return;
    }

    // re-attach any soft dependents, now that we are starting again
    if (!was_active) {
        for (auto dept : dependents) {
            if (!dept->is_hard()) {
                service_state_t dept_state = dept->get_from()->service_state;
                if (!dept->holding_acq
                        && (dept_state == service_state_t::STARTED || dept_state == service_state_t::STARTING)) {
                    dept->holding_acq = true;
                    ++required_by;
                }
            }
        }
    }

    if (was_active) {
        // We're already starting/started, or we are stopping and need to wait for
        // that the complete.
        if (service_state != service_state_t::STOPPING) {
            return;
        }

        if (! can_interrupt_stop()) {
            return;
        }

        // We're STOPPING, and that can be interrupted. Our dependencies might be STOPPING,
        // but if so they are waiting (for us), so they too can be instantly returned to
        // STARTING state.
        notify_listeners(service_event_t::STOPCANCELLED);
    }
    else { // !was_active
        services->service_active(this);
        prop_require = !prop_release;
        prop_release = false;
        if (prop_require) {
            services->add_prop_queue(this);
        }
    }

    initiate_start();
}

void service_record::dependency_started() noexcept
{
    // Note that we check for STARTED state here in case the service is in smooth recovery while pinned.
    // In that case it will wait for dependencies to start before restarting the process.
    if ((service_state == service_state_t::STARTING || service_state == service_state_t::STARTED)
            && waiting_for_deps) {
        services->add_transition_queue(this);
    }
}

bool service_record::start_check_dependencies() noexcept
{
    bool all_deps_started = true;

    for (auto & dep : depends_on) {
        service_record * to = dep.get_to();
        if (dep.is_only_ordering()
                && to->service_state != service_state_t::STARTING) continue;
        if (to->service_state != service_state_t::STARTED) {
            dep.waiting_on = true;
            all_deps_started = false;
        }
    }

    return all_deps_started;
}

bool service_record::check_deps_started() noexcept
{
    for (auto & dep : depends_on) {
        if (dep.waiting_on) {
            return false;
        }
    }

    return true;
}

void service_record::all_deps_started() noexcept
{
    if (onstart_flags.starts_on_console && ! have_console) {
        queue_for_console();
        return;
    }

    waiting_for_deps = false;

    if (!bring_up()) {
        service_state = service_state_t::STOPPING;
        failed_to_start();
    }
}

void service_record::acquired_console() noexcept
{
    waiting_for_console = false;
    have_console = true;

    if (service_state != service_state_t::STARTING) {
        // We got the console but no longer want it.
        release_console();
    }
    else if (check_deps_started()) {
        all_deps_started();
    }
    else {
        // We got the console but can't use it yet.
        release_console();
    }
}

void service_record::started() noexcept
{
    // If we start on console but don't keep it, release it now:
    if (have_console && ! onstart_flags.runs_on_console) {
        bp_sys::tcsetpgrp(0, bp_sys::getpgrp());
        release_console();
    }

    log_service_started(get_name());
    service_state = service_state_t::STARTED;
    notify_listeners(service_event_t::STARTED);

    if (onstart_flags.rw_ready) {
        rootfs_is_rw();
    }
    if (onstart_flags.log_ready) {
        setup_external_log();
    }

    if (force_stop || desired_state == service_state_t::STOPPED) {
        // We must now stop.
        do_stop();
        return;
    }

    // Notify any dependents whose desired state is STARTED:
    for (auto dept : dependents) {
        if (dept->waiting_on) {
            dept->get_from()->dependency_started();
            dept->waiting_on = false;
        }
    }
}

void service_record::failed_to_start(bool depfailed, bool immediate_stop) noexcept
{
    desired_state = service_state_t::STOPPED;

    if (waiting_for_console) {
        services->unqueue_console(this);
        waiting_for_console = false;
    }

    if (start_explicit) {
        start_explicit = false;
        release(false);
    }

    // Cancel start of dependents:
    for (auto & dept : dependents) {
        switch (dept->dep_type) {
        case dependency_type::REGULAR:
        case dependency_type::MILESTONE:
            // If REGULAR and STARTED, we can't have failed to start i.e. we must be started, so
            // we don't worry about that case. If MILESTONE and started the dependency is already
            // satisfied so again we don't need to do anything.
            if (dept->get_from()->service_state == service_state_t::STARTING) {
                dept->get_from()->prop_failure = true;
                services->add_prop_queue(dept->get_from());
            }
            break;
        case dependency_type::WAITS_FOR:
        case dependency_type::SOFT:
        case dependency_type::BEFORE:
        case dependency_type::AFTER:
            if (dept->waiting_on) {
                dept->waiting_on = false;
                dept->get_from()->dependency_started();
            }
        }

        // Always release now, so that our desired state will be STOPPED before we call
        // stopped() below (if we do so). Otherwise it may decide to restart us.
        if (dept->holding_acq) {
            dept->holding_acq = false;
            release(false);
        }
    }

    start_failed = true;
    log_service_failed(get_name());
    notify_listeners(service_event_t::FAILEDSTART);
    pinned_started = false;

    if (immediate_stop) {
        stopped();
    }
}

void service_record::unrecoverable_stop() noexcept
{
    desired_state = service_state_t::STOPPED;
    forced_stop();
}

bool service_record::bring_up() noexcept
{
    // default implementation: there is no process, so we are started.
    started();
    return true;
}

// Mark this and all dependent services to be force-stopped.
void service_record::forced_stop() noexcept
{
    if (service_state != service_state_t::STOPPED) {
        force_stop = true;
        if (! pinned_started) {
            prop_stop = true;
            services->add_prop_queue(this);
        }
    }
}

void service_record::dependent_stopped() noexcept
{
    if (service_state == service_state_t::STOPPING && waiting_for_deps) {
        services->add_transition_queue(this);
    }
}

void service_record::stop(bool bring_down) noexcept
{
    // Stop; remove activation, and don't self-restart.

    if (start_explicit) {
        start_explicit = false;
        required_by--;
    }

    if (bring_down || required_by == 0) {
        // Set desired state to STOPPED, this will inhibit automatic restart (and will be
        // propagated to dependents)
        desired_state = service_state_t::STOPPED;
    }

    if (pinned_started) {
        return;
    }

    // If our required_by count is 0, we should treat this as a full manual stop regardless
    if (required_by == 0) {
        bring_down = true;
        prop_release = !prop_require;
        if (prop_release) {
            services->add_prop_queue(this);
        }
    }

    if (bring_down && service_state != service_state_t::STOPPED
    		&& service_state != service_state_t::STOPPING) {
    	stop_reason = stopped_reason_t::NORMAL;
        do_stop();
    }
}

bool service_record::restart() noexcept
{
    // Re-start without affecting dependency links/activation.

    if (service_state == service_state_t::STARTED) {
        stop_reason = stopped_reason_t::NORMAL;
        force_stop = true;
        do_stop(true);
        return true;
    }

    // Wrong state
    return false;
}

void service_record::do_stop(bool with_restart) noexcept
{
    // Called when we should definitely stop. We may need to restart afterwards, but we
    // won't know that for sure until the execution transition.

    if (pinned_started) return;

    in_auto_restart = false;

    // Will we restart? desired state of STOPPED inhibits auto-restart
    bool for_restart = with_restart || (auto_restart && desired_state == service_state_t::STARTED);

    // If we won't restart, release explicit activation:
    if (!for_restart) {
        if (start_explicit) {
            start_explicit = false;
            release(false);
        }
    }

    bool all_deps_stopped = stop_dependents(for_restart);

    if (service_state != service_state_t::STARTED) {
        if (service_state == service_state_t::STARTING) {
            // If waiting for a dependency, or waiting for the console, we can interrupt start. Otherwise,
            // we need to delegate to can_interrupt_start() (which can be overridden).
            if (! waiting_for_deps && ! waiting_for_console) {
                if (! can_interrupt_start()) {
                    // Well this is awkward: we're going to have to continue starting. We can stop once
                    // we've reached the started state.
                    return;
                }

                if (! interrupt_start()) {
                    // Now wait for service startup to actually end; we don't need to handle it here.
                    notify_listeners(service_event_t::STARTCANCELLED);
                    return;
                }
            }
            else if (waiting_for_console) {
                services->unqueue_console(this);
                waiting_for_console = false;
            }

            // We must have had desired_state == STARTED.
            notify_listeners(service_event_t::STARTCANCELLED);

            // Reaching this point, we are starting interruptibly - so we
            // stop now (by falling through to below).
        }
        else {
            // If we're starting we need to wait for that to complete.
            // If we're already stopping/stopped there's nothing to do.
            return;
        }
    }

    service_state = service_state_t::STOPPING;
    waiting_for_deps = !all_deps_stopped;
    if (all_deps_stopped) {
        services->add_transition_queue(this);
    }
}

bool service_record::stop_check_dependents() noexcept
{
    bool all_deps_stopped = true;
    for (auto dept : dependents) {
        // Note if the dependent is waiting on us, it must be restarting (since the
        // waiting_on flag gets cleared when we stop, and would only be set if the
        // service tries to restart). We can treat that as "stopped" for purposes of
        // checking whether we can transition to stopped state.
        if (dept->is_hard() && dept->holding_acq && !dept->waiting_on) {
            all_deps_stopped = false;
            break;
        }
    }

    return all_deps_stopped;
}

bool service_record::stop_dependents(bool for_restart) noexcept
{
    // We are in either STARTED or STARTING states.
    bool all_deps_stopped = true;
    for (auto dept : dependents) {
        if (!dept->holding_acq) {
            continue;
        }
        if (dept->is_hard()) {
            service_record *dep_from = dept->get_from();

            if (!dep_from->is_fundamentally_stopped()) {
                // Note we check *first* since if the dependent service is not stopped,
                // 1. We will issue a stop to it shortly and
                // 2. It will notify us when stopped, at which point the stop_check_dependents()
                //    check is run anyway.
                all_deps_stopped = false;
            }

            if (force_stop) {
                // If this service is to be forcefully stopped, dependents must also be.
                dep_from->forced_stop();
            }

            if (dep_from->get_state() != service_state_t::STOPPED
                    && dep_from->get_state() != service_state_t::STOPPING) {
                dep_from->prop_stop = true;
                if (desired_state == service_state_t::STOPPED) {
                    // if we don't want to restart, don't restart dependent
                    dep_from->desired_state = service_state_t::STOPPED;
                    if (dep_from->start_explicit) {
                        dep_from->start_explicit = false;
                        dep_from->release(true);
                    }
                }
                services->add_prop_queue(dep_from);
            }
        }
        // Note that soft dependencies are retained if restarting, but otherwise
        // they are broken.
        else if (!for_restart && !dept->is_hard()) {
            if (dept->waiting_on) {
                // Note, milestone which is still waiting is considered a hard dependency and
                // is handled above. This is therefore a true soft dependency, and we can just
                // break the dependency link.
                dept->waiting_on = false;
                dept->get_from()->dependency_started();
                dept->holding_acq = false;
                release(false);
            }
            else {
                dept->holding_acq = false;
                release(false);
            }
        }
    }

    return all_deps_stopped;
}

// All dependents have stopped; we can stop now, too. Only called when STOPPING.
void service_record::bring_down() noexcept
{
    stopped();
}

void service_record::unpin() noexcept
{
    if (pinned_started) {
        pinned_started = false;
        // We only need special handling here if service was in STARTED state
        if (service_state == service_state_t::STARTED) {
            // If any dependents are stopping, then force_stop should already be set.

            // If we reached required_by 0, we need to propagate release now (since it wasn't
            // propagated as it normally would be when we hit 0, due to the pin)
            if (required_by == 0) {
                prop_release = true;
                services->add_prop_queue(this);
            }

            if (desired_state == service_state_t::STOPPED || force_stop) {
                do_stop();
                services->process_queues();
            }
        }
    }
    if (pinned_stopped) {
        pinned_stopped = false;
        // We don't need to check state. If we're pinned stopped we can't be required and so desired
        // state should always be stopped.
    }
}

void service_record::queue_for_console() noexcept
{
    waiting_for_console = true;
    services->append_console_queue(this);
}

void service_record::release_console() noexcept
{
    have_console = false;
    services->pull_console_queue();
}

bool service_record::interrupt_start() noexcept
{
    return true;
}

void service_set::service_active(service_record *sr) noexcept
{
    active_services++;
}

void service_set::service_inactive(service_record *sr) noexcept
{
    active_services--;
}
