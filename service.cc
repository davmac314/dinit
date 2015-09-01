#include "service.h"
#include <cstring>
#include <cerrno>
#include <sstream>
#include <iterator>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// Tokenize a string, allow quoting
// TODO doesn't yet allow quoting...
static std::vector<std::string> tokenize(std::string arg)
{
    // TODO rewrite to be more efficient.
    using namespace std;
    istringstream iss(arg);
    return vector<string>(istream_iterator<string>(iss), istream_iterator<string>());
}

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
    service_state = SVC_STOPPED;
    force_stop = false;
    
    // Stop any dependencies whose desired state is SVC_STOPPED:
    for (sr_iter i = depends_on.begin(); i != depends_on.end(); i++) {
        (*i)->dependentStopped();
    }

    service_set->service_inactive(this);
    
    // TODO inform listeners.
    if (desired_state == SVC_STARTED) {
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
    // we asked it to (sr->service_state == SVC_STOPPING). But even if
    // we didn't, there's not much we can do.
    
    if (sr->service_type == SVC_PROCESS) {
        // TODO log non-zero rstatus?
        if (sr->service_state == SVC_STOPPING) {
            sr->stopped();
        }
        else {
            sr->forceStop();
        }
        
        if (sr->auto_restart && sr->service_set->get_auto_restart()) {
            sr->start();
        }
    }
    else {  // SVC_SCRIPTED
        if (sr->service_state == SVC_STOPPING) {
            if (w->rstatus == 0) {
                sr->stopped();
            }
            else {
                // TODO
                // ??? failed to stop!
                // For now just pretend we stopped, so that any dependencies
                // can be stopped:
                sr->stopped();
            }
        }
        else { // SVC_STARTING
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
    if ((service_state == SVC_STARTING || service_state == SVC_STARTED)
            && desired_state == SVC_STOPPED) {
        // This service was starting, or started, but was set to be stopped.
        // Cancel the stop (and continue starting/running).
        // TODO any listeners waiting for stop should be notified of
        //      its cancellation
    }
    
    desired_state = SVC_STARTED;

    if (service_state != SVC_STOPPED) {
        // Either we need do nothing (service is already started/starting)
        // or the service is currently being stopped and we must wait for
        // that to complete.
        return;
    }
    
    // Service state is SVC_STOPPED. Start the service.
    
    // First, start dependencies
    bool all_deps_started = true;
    for (sr_iter i = depends_on.begin(); i != depends_on.end(); ++i) {
        // Note, we cannot treat a dependency as started if its force_stop
        // flag is set.
        if ((*i)->service_state != SVC_STARTED || (*i)->force_stop) {
            all_deps_started = false;
            (*i)->start();
        }
    }
    
    if (! all_deps_started) {
        // The dependencies will notify this service once they've started.
        return;
    }
    
    // Actually start this service.
    service_state = SVC_STARTING;
    service_set->service_active(this);
    
    if (service_type == SVC_PROCESS) {
        bool start_success = start_ps_process();
        if (start_success) {
            started();
        }
        else {
            failed_to_start();
        }
    }
    else {
        // Script-controlled service
        bool start_success = start_ps_process(std::vector<std::string>(1, "start"));
        if (! start_success) {
            failed_to_start();
        }
    }
}

void ServiceRecord::started()
{
    service_state = SVC_STARTED;
    // TODO - inform listeners

    if (desired_state == SVC_STARTED) {
        // Start any dependents whose desired state is SVC_STARTED:
        for (sr_iter i = dependents.begin(); i != dependents.end(); i++) {
            if ((*i)->desired_state == SVC_STARTED) {
                (*i)->start();
            }
        }
    }
    else {
        stop();
    }
}

void ServiceRecord::failed_to_start()
{
    service_state = SVC_STOPPED;
    desired_state = SVC_STOPPED;
    service_set->service_inactive(this);
    // failure to start
    // TODO - inform listeners of failure
    // Cancel start of dependents:
    for (sr_iter i = dependents.begin(); i != dependents.end(); i++) {
        if ((*i)->desired_state == SVC_STARTED) {
            (*i)->failed_dependency();
        }
    }    
}

bool ServiceRecord::start_ps_process()
{
    // BIG FAT NOTE: We rely on linux semantics of vfork() here.
    // Specifically:
    // * Parent process execution is suspended until the forked child
    //   successfully exec's another program, or it exits
    // * Memory is shared between the two processes until exec()
    //   succeeds.
    // Both of the above mean that we can determine in the parent process
    // whether or not the exec succeeded. If vfork instead is implemented
    // as an alias of fork, it will look like the exec always succeeded.
    
    /*
    volatile int exec_status = 0;
    pid_t forkpid = vfork();
    if (forkpid == 0) {
        // Child process
        // ev_default_destroy(); // won't need that on this side, free up fds.
        // Hmm. causes segfault. Of course. Memory is shared due to vfork.
        
        // Re-set stdin, stdout, stderr
        close(0); close(1); close(2);
        string logfile = this->logfile;
        if (logfile.length() == 0) {
            logfile = "/dev/null";
        }
        
        if (open("/dev/null", O_RDONLY) == 0) {
          // stdin = 0. That's what we should have; proceed with opening
          // stdout and stderr.
          open(logfile.c_str(), O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
          dup2(1, 2);
        }
        
        const char * pname = program_name.c_str();
        char const * args[2] = { pname, 0 };
        execvp(pname, (char ** const) args);
        // If we got here, the exec failed
        exec_status = errno;
        _exit(0);
    }
    else {
        // Parent process - we only reach here once the exec() above
        // has succeeded, or _exit() above was called (because vfork()
        // suspends the parent until either of those occurs).
        if (exec_status == 0) {
            // success
            pid = forkpid;

            // Add a process listener so we can detect when the
            // service stops
            ev_child_init(&child_listener, process_child_callback, pid, 0);
            child_listener.data = this;
            ev_child_start(ev_default_loop(EVFLAG_AUTO), &child_listener);

            service_state = SVC_STARTED;
            return true;
        }
        else {
            return false;
        }
    }
    */
    
    return start_ps_process(std::vector<std::string>());
}


bool ServiceRecord::start_ps_process(const std::vector<std::string> &pargs)
{
    // In general, you can't tell whether fork/exec is successful. We use a pipe to communicate
    // success/failure from the child to the parent. The pipe is set CLOEXEC so a successful
    // exec closes the pipe, and the parent sees EOF. If the exec is unsuccessful, the errno
    // is written to the pipe, and the parent can read it.
    
    using std::vector;
    using std::string;
    
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) {
        // TODO log error
        return false;
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
        // Child process
        ev_default_destroy(); // won't need that on this side, free up fds.
        
        // Re-set stdin, stdout, stderr
        close(0); close(1); close(2);
        string logfile = this->logfile;
        if (logfile.length() == 0) {
            logfile = "/dev/null";
        }
        
        // TODO rethink this logic. If we open it at not-0, shouldn't we just dup it to 0?:
        if (open("/dev/null", O_RDONLY) == 0) {
          // stdin = 0. That's what we should have; proceed with opening
          // stdout and stderr.
          open(logfile.c_str(), O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
          dup2(1, 2);
        }
        
        // Tokenize the command, and add additional arguments from pargs:
        vector<string> progAndArgs = tokenize(program_name);
        progAndArgs.insert(progAndArgs.end(), pargs.begin(), pargs.end());
        
        const char * pname = progAndArgs[0].c_str();
        const char ** args = new const char *[progAndArgs.size() + 1];
        
        for (std::vector<std::string>::size_type i = 0; i < progAndArgs.size(); i++) {
            args[i] = progAndArgs[i].c_str();
        }
        args[progAndArgs.size()] = nullptr;
        
        execvp(pname, (char ** const) args);
        
        // If we got here, the exec failed:        
        int exec_status = errno;
        write(pipefd[1], &exec_status, sizeof(int));
        exit(0);
    }
    else {
        // Parent process - we only reach here once the exec() above
        // has succeeded, or _exit() above was called (because vfork()
        // suspends the parent until either of those occurs).
        
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
void ServiceRecord::forceStop()
{
    force_stop = true;
    stop();
    for (sr_iter i = dependents.begin(); i != dependents.end(); i++) {
        (*i)->forceStop();
    }        
}

// A dependency of this service failed to start.
void ServiceRecord::failed_dependency()
{
    // TODO notify listeners
    desired_state = SVC_STOPPED;
    
    // Presumably, we were starting. So now we're not.
    service_state = SVC_STOPPED;
    
    // Notify dependents of this service also
    for (sr_iter i = dependents.begin(); i != dependents.end(); i++) {
        if ((*i)->desired_state == SVC_STARTED) {
            (*i)->failed_dependency();
        }
    }    
}

void ServiceRecord::dependentStopped()
{
    if (desired_state == SVC_STOPPED || force_stop) {
        bool all_deps_stopped = true;
        for (sr_iter i = dependents.begin(); i != dependents.end(); ++i) {
            if ((*i)->service_state != SVC_STOPPED) {
                all_deps_stopped = false;
                break;
            }
        }
        
        if (all_deps_stopped) {
            stopping();
        }
    }
}

void ServiceRecord::stop()
{
    if ((service_state == SVC_STOPPING || service_state == SVC_STOPPED)
            && desired_state == SVC_STARTED) {
        // The service *was* stopped/stopping, but it was going to restart.
        // Now, we'll cancel the restart.
        // TODO inform listeners waiting for start of cancellation
    }
    
    desired_state = SVC_STOPPED;

    if (service_state != SVC_STARTED) {
        // If we're starting we need to wait for that to complete.
        // If we're already stopping/stopped there's nothing to do.
        return;
    }

    // Make sure all dependents have stopped.
    
    bool all_deps_stopped = true;
    for (sr_iter i = dependents.begin(); i != dependents.end(); ++i) {
        if ((*i)->service_state != SVC_STOPPED) {
            all_deps_stopped = false;
            (*i)->stop();
        }
    }
    
    if (! all_deps_stopped) {
        // The dependents will notify this service once they've stopped.
        return;
    }
    
    // Ok, dependents have stopped. We can stop ourselves.
    stopping();
}

// Dependency stopped or is stopping; we must stop too.
void ServiceRecord::stopping()
{
    service_state = SVC_STOPPING;

    if (service_type == SVC_PROCESS) {
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
    else {
        // Scripted service.
        start_ps_process(std::vector<string>(1, "stop"));
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
