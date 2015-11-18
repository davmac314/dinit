#include <cstring>
#include <cerrno>
#include <sstream>
#include <iterator>
#include <memory>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "service.h"
#include "dinit-log.h"

// from dinit.cc:
void open_control_socket(struct ev_loop *loop);


// Find the requested service by name
static ServiceRecord * findService(const std::list<ServiceRecord *> & records,
                                    const char *name)
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

ServiceRecord * ServiceSet::findService(std::string name)
{
    return ::findService(records, name.c_str());
}

void ServiceSet::startService(const char *name)
{
    using namespace std;
    ServiceRecord *record = loadServiceRecord(name);
    
    record->start();
}

void ServiceSet::stopService(const std::string & name)
{
    ServiceRecord *record = findService(name);
    if (record != nullptr) {
        record->stop();
    }
}

// Called when a service has actually stopped.
void ServiceRecord::stopped()
{
    logServiceStopped(service_name);
    service_state = ServiceState::STOPPED;
    force_stop = false;
    
    // Stop any dependencies whose desired state is STOPPED:
    for (sr_iter i = depends_on.begin(); i != depends_on.end(); i++) {
        (*i)->dependentStopped();
    }

    service_set->service_inactive(this);
    
    // TODO inform listeners.
    if (desired_state == ServiceState::STARTED) {
        // Desired state is "started".
        start();
    }
}

void ServiceRecord::process_child_callback(struct ev_loop *loop, ev_child *w, int revents)
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

void ServiceRecord::start()
{
    if ((service_state == ServiceState::STARTING || service_state == ServiceState::STARTED)
            && desired_state == ServiceState::STOPPED) {
        // This service was starting, or started, but was set to be stopped.
        // Cancel the stop (and continue starting/running).
        // TODO any listeners waiting for stop should be notified of
        //      its cancellation
    }

    auto old_desired_state = desired_state;
    desired_state = ServiceState::STARTED;
    
    if (service_state == ServiceState::STARTED || service_state == ServiceState::STARTING) {
        // We couldn't be started or starting unless all dependencies have
        // already started: so there's nothing left to do.
        return;
    }
    
    bool all_deps_started = true;

    // Ask dependencies to start, mark them as being waited on.
    
    for (sr_iter i = depends_on.begin(); i != depends_on.end(); ++i) {
        // Note, we cannot treat a dependency as started if its force_stop
        // flag is set.
        if ((*i)->service_state != ServiceState::STARTED || (*i)->force_stop) {
            all_deps_started = false;
            (*i)->start();
        }
    }
    
    if (old_desired_state != ServiceState::STARTED) {
        // This is a fresh start, so we mark all soft dependencies as 'waiting on' and ask them
        // to start:
        for (auto i = soft_deps.begin(); i != soft_deps.end(); ++i) {
            if (i->getTo()->service_state != ServiceState::STARTED) {
                all_deps_started = false;
                i->getTo()->start();
                i->waiting_on = true;
            }
        }
    }
    else {
        // This is (or at least may be) a notification that a dependency is ready; let's
        // just check them:
        for (auto i = soft_deps.begin(); i != soft_deps.end(); ++i) {
            ServiceRecord * to = i->getTo();
            if (i->waiting_on) {
                if ((to->desired_state != ServiceState::STARTED && to->service_state != ServiceState::STARTING) || to->service_state == ServiceState::STARTED) {
                    // Service has either started or is no longer starting
                    i->waiting_on = false;
                }
                else {
                    all_deps_started = false;
                }
            }
        }
    }

    if (! all_deps_started) {
        // The dependencies will notify this service once they've started.
        return;
    }
    
    // Actually start this service.
    service_state = ServiceState::STARTING;
    service_set->service_active(this);
    
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
        bool start_success = start_ps_process(std::vector<std::string>(1, "start"));
        if (! start_success) {
            failed_to_start();
        }
    }
    else {
        // "internal" service
        started();
    }
}

void ServiceRecord::dependencyStarted()
{
    start();
}

void ServiceRecord::started()
{
    logServiceStarted(service_name);
    service_state = ServiceState::STARTED;
    // TODO - inform listeners

    if (onstart_flags.release_console) {
        log_to_console = false;
    }

    if (onstart_flags.rw_ready) {
        open_control_socket(ev_default_loop(EVFLAG_AUTO));
    }

    if (desired_state == ServiceState::STARTED) {
        // Start any dependents whose desired state is STARTED:
        for (auto i = dependents.begin(); i != dependents.end(); i++) {
            if ((*i)->desired_state == ServiceState::STARTED) {
                (*i)->dependencyStarted();
            }
        }
        for (auto i = soft_dpts.begin(); i != soft_dpts.end(); i++) {
            if ((*i)->getFrom()->desired_state == ServiceState::STARTED) {
                (*i)->getFrom()->dependencyStarted();
            }
        }
    }
    else {
        stop();
    }
}

void ServiceRecord::failed_to_start()
{
    logServiceFailed(service_name);
    service_state = ServiceState::STOPPED;
    desired_state = ServiceState::STOPPED;
    service_set->service_inactive(this);
    // failure to start
    // Cancel start of dependents:
    for (sr_iter i = dependents.begin(); i != dependents.end(); i++) {
        if ((*i)->desired_state == ServiceState::STARTED) {
            (*i)->failed_dependency();
        }
    }    
    for (auto i = soft_dpts.begin(); i != soft_dpts.end(); i++) {
        if ((*i)->getFrom()->desired_state == ServiceState::STARTED) {
            // We can send 'start', because this is only a soft dependency.
            // Our startup failure means that they don't have to wait for us.
            (*i)->getFrom()->start();
        }
    }
}

bool ServiceRecord::start_ps_process() noexcept
{
    try {
        return start_ps_process(std::vector<std::string>());
    }
    catch (std::bad_alloc & bad_alloc_exc) {
        // TODO log error
        return false;
    }
}

bool ServiceRecord::start_ps_process(const std::vector<std::string> &pargs) noexcept
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

    try {
        //auto argsv = std::vector<const char *>(num_args + pargs.size() + 1);
        auto argsv = std::vector<const char *>(num_args + pargs.size() + 1);
        auto args = argsv.data();
        int i;
        for (i = 0; i < num_args; i++) {
            args[i] = exec_arg_parts[i];
        }
        for (auto progarg : pargs) {
            args[i] = progarg.c_str();
            i++;
        }
        args[i] = nullptr;
        
        string logfile = this->logfile;
        if (logfile.length() == 0) {
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

        if (forkpid == 0) {
            // Child process. Must not allocate memory (or otherwise risk throwing any exception)
            // from here until exit().
            ev_default_destroy(); // won't need that on this side, free up fds.

            // Re-set stdin, stdout, stderr
            close(0); close(1); close(2);

            // TODO rethink this logic. If we open it at not-0, shouldn't we just dup it to 0?:
            if (open("/dev/null", O_RDONLY) == 0) {
              // stdin = 0. That's what we should have; proceed with opening
              // stdout and stderr.
              open(logfile.c_str(), O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
              dup2(1, 2);
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
    catch (std::bad_alloc &bad_alloc_exc) {
        log(LogLevel::ERROR, "Out of memory");
        return false;
    }
}

// Mark this and all dependent services as force-stopped.
void ServiceRecord::forceStop()
{
    force_stop = true;
    stop();
    for (sr_iter i = dependents.begin(); i != dependents.end(); i++) {
        (*i)->forceStop();
    }
    // We don't want to force stop soft dependencies, however.
}

// A dependency of this service failed to start.
void ServiceRecord::failed_dependency()
{
    desired_state = ServiceState::STOPPED;
    
    // Presumably, we were starting. So now we're not.
    service_state = ServiceState::STOPPED;
    
    // Notify dependents of this service also
    for (auto i = dependents.begin(); i != dependents.end(); i++) {
        if ((*i)->desired_state == ServiceState::STARTED) {
            (*i)->failed_dependency();
        }
    }
    for (auto i = soft_dpts.begin(); i != soft_dpts.end(); i++) {
        if ((*i)->getFrom()->desired_state == ServiceState::STARTED) {
            // It's a soft dependency, so send them 'started' rather than
            // 'failed dep'.
            (*i)->getFrom()->started();
        }
    }    
}

void ServiceRecord::dependentStopped()
{
    if (service_state != ServiceState::STOPPED && (desired_state == ServiceState::STOPPED || force_stop)) {
        // Check the other dependents before we stop.
        if (stopCheckDependents()) {
            stopping();
        }
    }
}

void ServiceRecord::stop()
{
    if ((service_state == ServiceState::STOPPING || service_state == ServiceState::STOPPED)
            && desired_state == ServiceState::STARTED) {
        // The service *was* stopped/stopping, but it was going to restart.
        // Now, we'll cancel the restart.
        // TODO inform listeners waiting for start of cancellation
    }
    
    if (desired_state == ServiceState::STOPPED) return;
    
    desired_state = ServiceState::STOPPED;

    if (service_state != ServiceState::STARTED) {
        if (service_state == ServiceState::STARTING) {
            // Well this is awkward: we're going to have to continue
            // starting, but we don't want any dependents to think that
            // they are still waiting to start.
            // Make sure they remain stopped:
            stopDependents();
        }
        
        // If we're starting we need to wait for that to complete.
        // If we're already stopping/stopped there's nothing to do.
        return;
    }
    
    // If we get here, we are in STARTED state; stop all dependents.
    if (stopCheckDependents()) {
        stopping();
    }
}

bool ServiceRecord::stopCheckDependents()
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

bool ServiceRecord::stopDependents()
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
void ServiceRecord::stopping()
{
    service_state = ServiceState::STOPPING;

    if (service_type == ServiceType::PROCESS) {
        if (pid != -1) {
          // The process is still kicking on - must actually kill it.
          kill(pid, SIGTERM);
          // Now we wait; the rest is done in process_child_callback
        }
        else {
            // The process is already dead.
            stopped();
        }
    }
    else if (service_type == ServiceType::SCRIPTED) {
        // Scripted service.
        start_ps_process(std::vector<string>(1, "stop"));
    }
    else {
        stopped();
    }
}

void ServiceSet::service_active(ServiceRecord *sr)
{
    active_services++;
}

void ServiceSet::service_inactive(ServiceRecord *sr)
{
    active_services--;
}
