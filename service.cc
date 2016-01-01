#include <cstring>
#include <cerrno>
#include <sstream>
#include <iterator>
#include <memory>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

#include "service.h"
#include "dinit-log.h"

// from dinit.cc:
void open_control_socket(struct ev_loop *loop) noexcept;


// Find the requested service by name
static ServiceRecord * findService(const std::list<ServiceRecord *> & records,
                                    const char *name) noexcept
{
    using std::list;
    list<ServiceRecord *>::const_iterator i = records.begin();
    for ( ; i != records.end(); i++ ) {
        if (strcmp((*i)->getServiceName(), name) == 0) {
            return *i;
        }
    }
    return (ServiceRecord *)0;
}

ServiceRecord * ServiceSet::findService(const std::string &name) noexcept
{
    return ::findService(records, name.c_str());
}

void ServiceSet::startService(const char *name)
{
    using namespace std;
    ServiceRecord *record = loadServiceRecord(name);
    
    record->start();
}

void ServiceSet::stopService(const std::string & name) noexcept
{
    ServiceRecord *record = findService(name);
    if (record != nullptr) {
        record->stop();
    }
}

// Called when a service has actually stopped.
void ServiceRecord::stopped() noexcept
{
    if (service_type != ServiceType::SCRIPTED && onstart_flags.runs_on_console) {
        tcsetpgrp(0, getpgrp());
        releaseConsole();
    }

    logServiceStopped(service_name);
    service_state = ServiceState::STOPPED;
    force_stop = false;
    
    // Stop any dependencies whose desired state is STOPPED:
    for (sr_iter i = depends_on.begin(); i != depends_on.end(); i++) {
        (*i)->dependentStopped();
    }

    service_set->service_inactive(this);
    notifyListeners(ServiceEvent::STOPPED);
    
    if (desired_state == ServiceState::STARTED) {
        // Desired state is "started".
        start();
    }
}

void ServiceRecord::process_child_callback(struct ev_loop *loop, ev_child *w, int revents) noexcept
{    
    ServiceRecord *sr = (ServiceRecord *) w->data;

    sr->pid = -1;
    ev_child_stop(ev_default_loop(EVFLAG_AUTO), &sr->child_listener);
    
    // Ok, for a process service, any process death which we didn't rig
    // ourselves is a bit... unexpected. Probably, the child died because
    // we asked it to (sr->service_state == STOPPING). But even if
    // we didn't, there's not much we can do.
    
    if (sr->service_type == ServiceType::PROCESS) {
        // TODO log non-zero rstatus?
        if (sr->service_state == ServiceState::STOPPING) {
            sr->stopped();
        }
        else {
            sr->forceStop();
        }
        
        if (sr->auto_restart && sr->service_set->get_auto_restart()) {
            sr->start();
        }
    }
    else {  // SCRIPTED
        if (sr->service_state == ServiceState::STOPPING) {
            if (w->rstatus == 0) {
                sr->stopped();
            }
            else {
                // ??? failed to stop! Let's log it as info:
                log(LogLevel::INFO, "service ", sr->service_name, " stop command failed with exit code ", w->rstatus);
                // Just assume that we stopped, so that any dependencies
                // can be stopped:
                sr->stopped();
            }
        }
        else { // STARTING
            if (w->rstatus == 0) {
                sr->started();
            }
            else {
                // failed to start
                sr->failed_to_start();
            }
        }
    }
}

void ServiceRecord::start() noexcept
{
    if ((service_state == ServiceState::STARTING || service_state == ServiceState::STARTED)
            && desired_state == ServiceState::STOPPED) {
        // This service was starting, or started, but was set to be stopped.
        // Cancel the stop (and continue starting/running).
        notifyListeners(ServiceEvent::STOPCANCELLED);
    }

    if (desired_state == ServiceState::STARTED && service_state != ServiceState::STOPPED) return;

    desired_state = ServiceState::STARTED;
    
    if (pinned_stopped) return;
    
    if (service_state != ServiceState::STOPPED) {
        // We're already starting/started, or we are stopping and need to wait for
        // that the complete.
        if (service_state != ServiceState::STOPPING || ! can_interrupt_stop()) {
            return;
        }
        // We're STOPPING, and that can be interrupted. Our dependencies might be STOPPING,
        // but if so they are waiting (for us), so they too can be instantly returned to
        // STARTING state.
    }
    
    service_state = ServiceState::STARTING;
    service_set->service_active(this);

    waiting_for_deps = true;

    // Ask dependencies to start, mark them as being waited on.
    if (! startCheckDependencies(true)) {
        return;
    }

    // Actually start this service.
    allDepsStarted();
}

void ServiceRecord::dependencyStarted() noexcept
{
    if (service_state != ServiceState::STARTING || ! waiting_for_deps) {
        return;
    }

    if (startCheckDependencies(false)) {
        allDepsStarted();
    }
}

bool ServiceRecord::startCheckDependencies(bool start_deps) noexcept
{
    bool all_deps_started = true;

    for (sr_iter i = depends_on.begin(); i != depends_on.end(); ++i) {
        if ((*i)->service_state != ServiceState::STARTED) {
            if (start_deps) {
                all_deps_started = false;
                (*i)->start();
            }
            else {
                return false;
            }
        }
    }

    for (auto i = soft_deps.begin(); i != soft_deps.end(); ++i) {
        ServiceRecord * to = i->getTo();
        if (start_deps) {
            if (to->service_state != ServiceState::STARTED) {
                to->start();
                i->waiting_on = true;
                all_deps_started = false;
            }
            else {
                i->waiting_on = false;
            }
        }
        else if (i->waiting_on) {
            if (to->service_state != ServiceState::STARTING) {
                // Service has either started or is no longer starting
                i->waiting_on = false;
            }
            else {
                // We are still waiting on this service
                return false;
            }
        }
    }
    
    return all_deps_started;
}

void ServiceRecord::allDepsStarted(bool hasConsole) noexcept
{
    if (onstart_flags.runs_on_console && ! hasConsole) {
        waiting_for_deps = true;
        queueForConsole();
        return;
    }
    
    waiting_for_deps = false;

    if (service_type == ServiceType::PROCESS) {
        bool start_success = start_ps_process();
        if (start_success) {
            started();
        }
        else {
            failed_to_start();
        }
    }
    else if (service_type == ServiceType::SCRIPTED) {
        // Script-controlled service
        bool start_success = start_ps_process();
        if (! start_success) {
            failed_to_start();
        }
    }
    else {
        // "internal" service
        started();
    }
}

void ServiceRecord::acquiredConsole() noexcept
{
    if (service_state != ServiceState::STARTING) {
        // We got the console but no longer want it.
        releaseConsole();
    }
    else if (startCheckDependencies(false)) {
        allDepsStarted(true);
    }
    else {
        // We got the console but can't use it yet.
        releaseConsole();
    }
}

void ServiceRecord::started()
{
    if (onstart_flags.runs_on_console && service_type == ServiceType::SCRIPTED) {
        tcsetpgrp(0, getpgrp());
        releaseConsole();
    }

    logServiceStarted(service_name);
    service_state = ServiceState::STARTED;
    notifyListeners(ServiceEvent::STARTED);

    if (onstart_flags.release_console) {
        log_to_console = false;
    }

    if (onstart_flags.rw_ready) {
        open_control_socket(ev_default_loop(EVFLAG_AUTO));
    }

    if (force_stop || desired_state == ServiceState::STOPPED) {
        // We must now stop.
        stop();
        return;
    }

    // Notify any dependents whose desired state is STARTED:
    for (auto i = dependents.begin(); i != dependents.end(); i++) {
        (*i)->dependencyStarted();
    }
    for (auto i = soft_dpts.begin(); i != soft_dpts.end(); i++) {
        (*i)->getFrom()->dependencyStarted();
    }
}

void ServiceRecord::failed_to_start()
{
    if (onstart_flags.runs_on_console) {
        tcsetpgrp(0, getpgrp());
        releaseConsole();
    }
    
    logServiceFailed(service_name);
    service_state = ServiceState::STOPPED;
    desired_state = ServiceState::STOPPED;
    service_set->service_inactive(this);
    notifyListeners(ServiceEvent::FAILEDSTART);
    
    // failure to start
    // Cancel start of dependents:
    for (sr_iter i = dependents.begin(); i != dependents.end(); i++) {
        if ((*i)->service_state == ServiceState::STARTING) {
            (*i)->failed_dependency();
        }
    }    
    for (auto i = soft_dpts.begin(); i != soft_dpts.end(); i++) {
        // We can send 'start', because this is only a soft dependency.
        // Our startup failure means that they don't have to wait for us.
        (*i)->getFrom()->dependencyStarted();
    }
}

bool ServiceRecord::start_ps_process() noexcept
{
    try {
        return start_ps_process(exec_arg_parts, onstart_flags.runs_on_console);
    }
    catch (std::bad_alloc & bad_alloc_exc) {
        // TODO log error
        return false;
    }
}

bool ServiceRecord::start_ps_process(const std::vector<const char *> &cmd, bool on_console) noexcept
{
    // In general, you can't tell whether fork/exec is successful. We use a pipe to communicate
    // success/failure from the child to the parent. The pipe is set CLOEXEC so a successful
    // exec closes the pipe, and the parent sees EOF. If the exec is unsuccessful, the errno
    // is written to the pipe, and the parent can read it.

    // TODO should NOT wait for the exec to succeed or fail here, as that could (when/if we allow
    // running child processes with lower priority) result in priority inversion.

    using std::vector;
    using std::string;
    
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) {
        // TODO log error
        return false;
    }
    
    // Set up the argument array and other data now (before fork), in case memory allocation fails.
    
    auto args = cmd.data();
    
    const char * logfile = this->logfile.c_str();
    if (*logfile == 0) {
        logfile = "/dev/null";
    }

    // TODO make sure pipefd's are not 0/1/2 (STDIN/OUT/ERR) - if they are, dup them
    // until they are not.

    pid_t forkpid = fork();
    if (forkpid == -1) {
        // TODO log error
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    // If the console already has a session leader, presumably it is us. On the other hand
    // if it has no session leader, and we don't create one, then control inputs such as
    // ^C will have no effect.
    bool do_set_ctty = (tcgetsid(0) == -1);

    if (forkpid == 0) {
        // Child process. Must not allocate memory (or otherwise risk throwing any exception)
        // from here until exit().
        ev_default_destroy(); // won't need that on this side, free up fds.

        if (! on_console) {
            // Re-set stdin, stdout, stderr
            close(0); close(1); close(2);

            // TODO rethink this logic. If we open it at not-0, shouldn't we just dup it to 0?:
            if (open("/dev/null", O_RDONLY) == 0) {
              // stdin = 0. That's what we should have; proceed with opening
              // stdout and stderr.
              open(logfile, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
              dup2(1, 2);
            }
        }
        else {
            // "run on console" - run as a foreground job on the terminal/console device
            if (do_set_ctty) {
                setsid();
                ioctl(0, TIOCSCTTY, 0);
            }
            setpgid(0,0);
            tcsetpgrp(0, getpgrp());
            
            // TODO disable suspend (^Z)? (via tcsetattr)
            //      (should be done before TIOCSCTTY)
        }

        execvp(exec_arg_parts[0], const_cast<char **>(args));

        // If we got here, the exec failed:
        int exec_status = errno;
        write(pipefd[1], &exec_status, sizeof(int));
        exit(0);
    }
    else {
        // Parent process
        close(pipefd[1]); // close the 'other end' fd

        int exec_status;
        if (read(pipefd[0], &exec_status, sizeof(int)) == 0) {
            // pipe closed; success
            pid = forkpid;

            // Add a process listener so we can detect when the
            // service stops
            ev_child_init(&child_listener, process_child_callback, pid, 0);
            child_listener.data = this;
            ev_child_start(ev_default_loop(EVFLAG_AUTO), &child_listener);

            close(pipefd[0]);
            return true;
        }
        else {
            // TODO log error
            close(pipefd[0]);
            return false;
        }
    }
}

// Mark this and all dependent services as force-stopped.
void ServiceRecord::forceStop() noexcept
{
    if (service_state != ServiceState::STOPPED) {
        force_stop = true;
        for (sr_iter i = dependents.begin(); i != dependents.end(); i++) {
            (*i)->forceStop();
        }
        stop();
        
        // We don't want to force stop soft dependencies, however.
    }
}

// A dependency of this service failed to start.
// Only called when state == STARTING.
void ServiceRecord::failed_dependency()
{
    desired_state = ServiceState::STOPPED;
    
    // Presumably, we were starting. So now we're not.
    service_state = ServiceState::STOPPED;
    service_set->service_inactive(this);
    logServiceFailed(service_name);
    
    // Notify dependents of this service also
    for (auto i = dependents.begin(); i != dependents.end(); i++) {
        if ((*i)->service_state == ServiceState::STARTING) {
            (*i)->failed_dependency();
        }
    }
    for (auto i = soft_dpts.begin(); i != soft_dpts.end(); i++) {
        // It's a soft dependency, so send them 'started' rather than
        // 'failed dep'.
        (*i)->getFrom()->dependencyStarted();
    }    
}

void ServiceRecord::dependentStopped() noexcept
{
    if (service_state == ServiceState::STOPPING) {
        // Check the other dependents before we stop.
        if (stopCheckDependents()) {
            allDepsStopped();
        }
    }
}

void ServiceRecord::stop() noexcept
{
    if ((service_state == ServiceState::STOPPING || service_state == ServiceState::STOPPED)
            && desired_state == ServiceState::STARTED) {
        // The service *was* stopped/stopping, but it was going to restart.
        // Now, we'll cancel the restart.
        notifyListeners(ServiceEvent::STARTCANCELLED);
    }
    
    if (desired_state == ServiceState::STOPPED && service_state != ServiceState::STARTED) return;
    
    desired_state = ServiceState::STOPPED;
    
    if (pinned_started) return;

    if (service_state != ServiceState::STARTED) {
        if (service_state == ServiceState::STARTING) {
            if (! can_interrupt_start()) {
                // Well this is awkward: we're going to have to continue
                // starting, but we don't want any dependents to think that
                // they are still waiting to start.
                // Make sure they remain stopped:
                stopDependents();
                return;
            }
            
            // Reaching this point, we have can_interrupt_start() == true. So,
            // we can stop. Dependents might be starting, but they must be
            // waiting on us, so they should also be immediately stoppable.
            // Fall through to below.
        }
        else {
            // If we're starting we need to wait for that to complete.
            // If we're already stopping/stopped there's nothing to do.
            return;
        }
    }
    
    service_state = ServiceState::STOPPING;
    waiting_for_deps = true;
    
    // If we get here, we are in STARTED state; stop all dependents.
    if (stopDependents()) {
        allDepsStopped();
    }
}

bool ServiceRecord::stopCheckDependents() noexcept
{
    bool all_deps_stopped = true;
    for (sr_iter i = dependents.begin(); i != dependents.end(); ++i) {
        if ((*i)->service_state != ServiceState::STOPPED) {
            all_deps_stopped = false;
            break;
        }
    }
    
    return all_deps_stopped;
}

bool ServiceRecord::stopDependents() noexcept
{
    bool all_deps_stopped = true;
    for (sr_iter i = dependents.begin(); i != dependents.end(); ++i) {
        if ((*i)->service_state != ServiceState::STOPPED) {
            all_deps_stopped = false;
            (*i)->stop();
        }
    }
    
    return all_deps_stopped;
}



// Dependency stopped or is stopping; we must stop too.
void ServiceRecord::allDepsStopped()
{
    waiting_for_deps = false;
    if (service_type == ServiceType::PROCESS) {
        if (pid != -1) {
            // The process is still kicking on - must actually kill it.
            if (! onstart_flags.no_sigterm) {
                kill(pid, SIGTERM);
            }
            if (term_signal != -1) {
                kill(pid, term_signal);
            }
            // Now we wait; the rest is done in process_child_callback
        }
        else {
            // The process is already dead.
            stopped();
        }
    }
    else if (service_type == ServiceType::SCRIPTED) {
        // Scripted service.
        if (! start_ps_process(stop_arg_parts, false)) {
            // stop script failed, but there's not much we can do:
            stopped();
        }
    }
    else {
        stopped();
    }
}

void ServiceRecord::pinStart() noexcept
{
    start();
    pinned_started = true;
}

void ServiceRecord::pinStop() noexcept
{
    stop();
    pinned_stopped = true;
}

void ServiceRecord::unpin() noexcept
{
    if (pinned_started) {
        pinned_started = false;
        if (desired_state == ServiceState::STOPPED) {
            stop();
        }
    }
    if (pinned_stopped) {
        pinned_stopped = false;
        if (desired_state == ServiceState::STARTED) {
            start();
        }
    }
}

void ServiceRecord::queueForConsole() noexcept
{
    next_for_console = nullptr;
    auto tail = service_set->consoleQueueTail(this);
    if (tail == nullptr) {
        acquiredConsole();
    }
    else {
        tail->next_for_console = this;
    }
}

void ServiceRecord::releaseConsole() noexcept
{
    if (next_for_console != nullptr) {
        next_for_console->acquiredConsole();
    }
    else {
        service_set->consoleQueueTail(nullptr);
    }
}

void ServiceSet::service_active(ServiceRecord *sr) noexcept
{
    active_services++;
}

void ServiceSet::service_inactive(ServiceRecord *sr) noexcept
{
    active_services--;
}
