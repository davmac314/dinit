#include <cstring>
#include <cerrno>
#include <sstream>
#include <iterator>
#include <memory>
#include <cstddef>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

#include "service.h"
#include "dinit-log.h"
#include "dinit-socket.h"

/*
 * service.cc - Service management.
 * See service.h for details.
 */

// from dinit.cc:
void open_control_socket(bool report_ro_failure = true) noexcept;
void setup_external_log() noexcept;
extern EventLoop_t eventLoop;

// Find the requested service by name
static ServiceRecord * find_service(const std::list<ServiceRecord *> & records,
                                    const char *name) noexcept
{
    using std::list;
    list<ServiceRecord *>::const_iterator i = records.begin();
    for ( ; i != records.end(); i++ ) {
        if (strcmp((*i)->getServiceName().c_str(), name) == 0) {
            return *i;
        }
    }
    return (ServiceRecord *)0;
}

ServiceRecord * ServiceSet::find_service(const std::string &name) noexcept
{
    return ::find_service(records, name.c_str());
}

void ServiceSet::startService(const char *name)
{
    using namespace std;
    ServiceRecord *record = loadServiceRecord(name);
    
    record->start();
    processQueues(true);
}

void ServiceSet::stopService(const std::string & name) noexcept
{
    ServiceRecord *record = find_service(name);
    if (record != nullptr) {
        record->stop();
        processQueues(false);
    }
}

// Called when a service has actually stopped; dependents have stopped already, unless this stop
// is due to an unexpected process termination.
void ServiceRecord::stopped() noexcept
{
    if (service_type != ServiceType::SCRIPTED && service_type != ServiceType::BGPROCESS
            && onstart_flags.runs_on_console) {
        tcsetpgrp(0, getpgrp());
        discard_console_log_buffer();
        releaseConsole();
    }

    force_stop = false;

    // If we are a soft dependency of another target, break the acquisition from that target now:
    bool will_restart = (desired_state == ServiceState::STARTED)
            && service_set->get_auto_restart();

    if (! will_restart) {
        for (auto dependency : soft_dpts) {
            if (dependency->holding_acq) {
                dependency->holding_acq = false;
                release();
            }
        }
    }

    will_restart &= (desired_state == ServiceState::STARTED);
    for (auto dependency : depends_on) {
        // we signal dependencies in case they are waiting for us to stop - but only if we won't
        // restart or if they are stopping uninterruptibly.
        if (! will_restart || ! dependency->can_interrupt_stop()) {
            dependency->dependentStopped();
        }
    }

    service_state = ServiceState::STOPPED;

    if (will_restart) {
        // Desired state is "started".
        service_set->addToStartQueue(this);
    }
    else {
        if (socket_fd != -1) {
            close(socket_fd);
            socket_fd = -1;
        }
        
        if (start_explicit) {
            start_explicit = false;
            release();
        }
        else if (required_by == 0) {
            service_set->service_inactive(this);
        }
    }

    logServiceStopped(service_name);
    notifyListeners(ServiceEvent::STOPPED);
}

dasynq::rearm ServiceChildWatcher::status_change(EventLoop_t &loop, pid_t child, int status) noexcept
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
        return rearm::REMOVE;
    }
    
    // Must deregister now since handle_exit_status might result in re-launch:
    deregister(loop, child);
    
    sr->handle_exit_status(status);
    return rearm::REMOVED;
}

bool ServiceRecord::do_auto_restart() noexcept
{
    if (auto_restart) {
        return service_set->get_auto_restart();
    }
    return false;
}

void process_service::handle_exit_status(int exit_status) noexcept
{
    bool did_exit = WIFEXITED(exit_status);
    bool was_signalled = WIFSIGNALED(exit_status);

    if (exit_status != 0 && service_state != ServiceState::STOPPING) {
        if (did_exit) {
            log(LogLevel::ERROR, "Service ", service_name, " process terminated with exit code ", WEXITSTATUS(exit_status));
        }
        else if (was_signalled) {
            log(LogLevel::ERROR, "Service ", service_name, " terminated due to signal ", WTERMSIG(exit_status));
        }
    }

    if (service_state == ServiceState::STARTING) {
        if (did_exit && WEXITSTATUS(exit_status) == 0) {
            started();
        }
        else {
            failed_to_start();
        }
    }
    else if (service_state == ServiceState::STOPPING) {
        // We won't log a non-zero exit status or termination due to signal here -
        // we assume that the process died because we signalled it.
        stopped();
    }
    else if (smooth_recovery && service_state == ServiceState::STARTED && desired_state == ServiceState::STARTED) {
        // TODO if we are pinned-started then we should probably check
        //      that dependencies have started before trying to re-start the
        //      service process.
        restart_ps_process();
        return;
    }
    else {
        if (! do_auto_restart()) desired_state = ServiceState::STOPPED;
        forceStop();
    }
    service_set->processQueues(false);
}

void bgproc_service::handle_exit_status(int exit_status) noexcept
{
    bool did_exit = WIFEXITED(exit_status);
    bool was_signalled = WIFSIGNALED(exit_status);

    if (exit_status != 0 && service_state != ServiceState::STOPPING) {
        if (did_exit) {
            log(LogLevel::ERROR, "Service ", service_name, " process terminated with exit code ", WEXITSTATUS(exit_status));
        }
        else if (was_signalled) {
            log(LogLevel::ERROR, "Service ", service_name, " terminated due to signal ", WTERMSIG(exit_status));
        }
    }

    if (doing_recovery) {
        // (BGPROCESS only)
        doing_recovery = false;
        bool need_stop = false;
        if ((did_exit && WEXITSTATUS(exit_status) != 0) || was_signalled) {
            need_stop = true;
        }
        else {
            // We need to re-read the PID, since it has now changed.
            if (pid_file.length() != 0) {
                if (! read_pid_file()) {
                    need_stop = true;
                }
            }
        }

        if (need_stop) {
            // Failed startup: no auto-restart.
            desired_state = ServiceState::STOPPED;
            forceStop();
            service_set->processQueues(false);
        }

        return;
    }

    if (service_state == ServiceState::STARTING) {
        // POSIX requires that if the process exited clearly with a status code of 0,
        // the exit status value will be 0:
        if (exit_status == 0) {
            if (pid_file.length() != 0 && ! read_pid_file()) {
                failed_to_start();
            }
            else {
                started();
            }
        }
        else {
            failed_to_start();
        }
    }
    else if (service_state == ServiceState::STOPPING) {
        // We won't log a non-zero exit status or termination due to signal here -
        // we assume that the process died because we signalled it.
        stopped();
    }
    else if (smooth_recovery && service_state == ServiceState::STARTED && desired_state == ServiceState::STARTED) {
        // TODO if we are pinned-started then we should probably check
        //      that dependencies have started before trying to re-start the
        //      service process.
        doing_recovery = true;
        restart_ps_process();
        return;
    }
    else {
        if (! do_auto_restart()) desired_state = ServiceState::STOPPED;
        forceStop();
    }
    service_set->processQueues(false);
}

void scripted_service::handle_exit_status(int exit_status) noexcept
{
    bool did_exit = WIFEXITED(exit_status);
    bool was_signalled = WIFSIGNALED(exit_status);

    if (service_state == ServiceState::STOPPING) {
        if (did_exit && WEXITSTATUS(exit_status) == 0) {
            stopped();
        }
        else {
            // ??? failed to stop! Let's log it as info:
            if (did_exit) {
                log(LogLevel::INFO, "Service ", service_name, " stop command failed with exit code ", WEXITSTATUS(exit_status));
            }
            else if (was_signalled) {
                log(LogLevel::INFO, "Serivice ", service_name, " stop command terminated due to signal ", WTERMSIG(exit_status));
            }
            // Just assume that we stopped, so that any dependencies
            // can be stopped:
            stopped();
        }
        service_set->processQueues(false);
    }
    else { // STARTING
        if (exit_status == 0) {
            started();
        }
        else {
            // failed to start
            if (did_exit) {
                log(LogLevel::ERROR, "Service ", service_name, " command failed with exit code ", WEXITSTATUS(exit_status));
            }
            else if (was_signalled) {
                log(LogLevel::ERROR, "Service ", service_name, " command terminated due to signal ", WTERMSIG(exit_status));
            }
            failed_to_start();
        }
        service_set->processQueues(true);
    }
}

rearm ServiceIoWatcher::fd_event(EventLoop_t &loop, int fd, int flags) noexcept
{
    base_process_service *sr = service;
    sr->waiting_for_execstat = false;
    
    int exec_status;
    int r = read(get_watched_fd(), &exec_status, sizeof(int));
    deregister(loop);
    close(get_watched_fd());
    
    if (r > 0) {
        // We read an errno code; exec() failed, and the service startup failed.
        sr->pid = -1;
        log(LogLevel::ERROR, sr->service_name, ": execution failed: ", strerror(exec_status));
        if (sr->service_state == ServiceState::STARTING) {
            sr->failed_to_start();
        }
        else if (sr->service_state == ServiceState::STOPPING) {
            // Must be a scripted service. We've logged the failure, but it's probably better
            // not to leave the service in STARTED state:
            sr->stopped();
        }
    }
    else {
        // exec() succeeded.
        if (sr->service_type == ServiceType::PROCESS) {
            // This could be a smooth recovery (state already STARTED). Even more, the process
            // might be stopped (and killed via a signal) during smooth recovery.  We don't to
            // process startup again in either case, so we check for state STARTING:
            if (sr->service_state == ServiceState::STARTING) {
                sr->started();
            }
        }
        
        if (sr->pid == -1) {
            // Somehow the process managed to complete before we even saw the status.
            sr->handle_exit_status(sr->exit_status);
        }
    }
    
    sr->service_set->processQueues(true);
    
    return rearm::REMOVED;
}

void ServiceRecord::require() noexcept
{
    if (required_by++ == 0) {
        
        if (! prop_require) {
            prop_require = true;
            prop_release = false;
            service_set->addToPropQueue(this);
        }
        
        if (service_state == ServiceState::STOPPED) {
            // (In any other state, the service is already considered active.)
            service_set->service_active(this);
        }
    }
}

void ServiceRecord::release() noexcept
{
    if (--required_by == 0) {
        desired_state = ServiceState::STOPPED;
        // Can stop, and can release dependencies now:
        prop_release = true;
        prop_require = false;
        service_set->addToPropQueue(this);
        if (service_state != ServiceState::STOPPED) {
            service_set->addToStopQueue(this);
        }
        else {
            service_set->service_inactive(this);
        }
    }
}

void ServiceRecord::release_dependencies() noexcept
{
    for (sr_iter i = depends_on.begin(); i != depends_on.end(); ++i) {
        (*i)->release();
    }

    for (auto i = soft_deps.begin(); i != soft_deps.end(); ++i) {
        ServiceRecord * to = i->getTo();
        if (i->holding_acq) {
            to->release();
            i->holding_acq = false;
        }
    }
}

void ServiceRecord::start(bool activate) noexcept
{
    if (activate && ! start_explicit) {
        require();
        start_explicit = true;
    }
    
    if (desired_state == ServiceState::STARTED && service_state != ServiceState::STOPPED) return;
    
    desired_state = ServiceState::STARTED;
    service_set->addToStartQueue(this);
}

void ServiceRecord::do_propagation() noexcept
{
    if (prop_require) {
        // Need to require all our dependencies
        for (sr_iter i = depends_on.begin(); i != depends_on.end(); ++i) {
            (*i)->require();
        }

        for (auto i = soft_deps.begin(); i != soft_deps.end(); ++i) {
            ServiceRecord * to = i->getTo();
            to->require();
            i->holding_acq = true;
        }
        
        prop_require = false;
    }
    
    if (prop_release) {
        release_dependencies();
        prop_release = false;
    }
    
    if (prop_failure) {
        prop_failure = false;
        failed_to_start(true);
    }
    
    if (waiting_for_deps) {
        if (service_state == ServiceState::STARTING) {
            if (startCheckDependencies(false)) {
                allDepsStarted();
            }
        }
        else if (service_state == ServiceState::STOPPING) {
            if (stopCheckDependents()) {
                all_deps_stopped();
            }
        }
    }
}

void ServiceRecord::execute_transition() noexcept
{
    bool is_started = (service_state == ServiceState::STARTED)
            || (service_state == ServiceState::STARTING && can_interrupt_start());
    bool is_stopped = (service_state == ServiceState::STOPPED)
            || (service_state == ServiceState::STOPPING && can_interrupt_stop());

    if (is_started && (desired_state == ServiceState::STOPPED || force_stop)) {
        if (! pinned_started) {
            do_stop();
        }
    }
    else if (is_stopped && desired_state == ServiceState::STARTED) {
        if (! pinned_stopped) {
            do_start();
        }
    }
}

void ServiceRecord::do_start() noexcept
{
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
        notifyListeners(ServiceEvent::STOPCANCELLED);
    }
    
    service_state = ServiceState::STARTING;

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
    if (service_state == ServiceState::STARTING && waiting_for_deps) {
        service_set->addToPropQueue(this);
    }
}

bool ServiceRecord::startCheckDependencies(bool start_deps) noexcept
{
    bool all_deps_started = true;

    for (sr_iter i = depends_on.begin(); i != depends_on.end(); ++i) {
        if ((*i)->service_state != ServiceState::STARTED) {
            if (start_deps) {
                all_deps_started = false;
                (*i)->start(false);
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
                to->start(false);
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

bool ServiceRecord::open_socket() noexcept
{
    if (socket_path.empty() || socket_fd != -1) {
        // No socket, or already open
        return true;
    }
    
    const char * saddrname = socket_path.c_str();
    uint sockaddr_size = offsetof(struct sockaddr_un, sun_path) + socket_path.length() + 1;

    struct sockaddr_un * name = static_cast<sockaddr_un *>(malloc(sockaddr_size));
    if (name == nullptr) {
        log(LogLevel::ERROR, service_name, ": Opening activation socket: out of memory");
        return false;
    }
    
    // Un-link any stale socket. TODO: safety check? should at least confirm the path is a socket.
    unlink(saddrname);

    name->sun_family = AF_UNIX;
    strcpy(name->sun_path, saddrname);

    int sockfd = dinit_socket(AF_UNIX, SOCK_STREAM, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (sockfd == -1) {
        log(LogLevel::ERROR, service_name, ": Error creating activation socket: ", strerror(errno));
        free(name);
        return false;
    }

    if (bind(sockfd, (struct sockaddr *) name, sockaddr_size) == -1) {
        log(LogLevel::ERROR, service_name, ": Error binding activation socket: ", strerror(errno));
        close(sockfd);
        free(name);
        return false;
    }
    
    free(name);
    
    // POSIX (1003.1, 2013) says that fchown and fchmod don't necesarily work on sockets. We have to
    // use chown and chmod instead.
    if (chown(saddrname, socket_uid, socket_gid)) {
        log(LogLevel::ERROR, service_name, ": Error setting activation socket owner/group: ", strerror(errno));
        close(sockfd);
        return false;
    }
    
    if (chmod(saddrname, socket_perms) == -1) {
        log(LogLevel::ERROR, service_name, ": Error setting activation socket permissions: ", strerror(errno));
        close(sockfd);
        return false;
    }

    if (listen(sockfd, 128) == -1) { // 128 "seems reasonable".
        log(LogLevel::ERROR, ": Error listening on activation socket: ", strerror(errno));
        close(sockfd);
        return false;
    }
    
    socket_fd = sockfd;
    return true;
}

void ServiceRecord::allDepsStarted(bool has_console) noexcept
{
    if (onstart_flags.runs_on_console && ! has_console) {
        waiting_for_deps = true;
        queueForConsole();
        return;
    }
    
    waiting_for_deps = false;

    if (! open_socket()) {
        failed_to_start();
    }

    bool start_success = start_ps_process();
    if (! start_success) {
        failed_to_start();
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

bool bgproc_service::read_pid_file() noexcept
{
    const char *pid_file_c = pid_file.c_str();
    int fd = open(pid_file_c, O_CLOEXEC);
    if (fd != -1) {
        char pidbuf[21]; // just enought to hold any 64-bit integer
        int r = read(fd, pidbuf, 20);
        if (r > 0) {
            pidbuf[r] = 0; // store nul terminator
            pid = std::atoi(pidbuf);
            if (kill(pid, 0) == 0) {                
                child_listener.add_watch(eventLoop, pid);
            }
            else {
                log(LogLevel::ERROR, service_name, ": pid read from pidfile (", pid, ") is not valid");
                pid = -1;
                close(fd);
                return false;
            }
        }
        close(fd);
        return true;
    }
    else {
        log(LogLevel::ERROR, service_name, ": read pid file: ", strerror(errno));
        return false;
    }
}

void ServiceRecord::started() noexcept
{
    if (onstart_flags.runs_on_console && (service_type == ServiceType::SCRIPTED || service_type == ServiceType::BGPROCESS)) {
        tcsetpgrp(0, getpgrp());
        releaseConsole();
    }

    logServiceStarted(service_name);
    service_state = ServiceState::STARTED;
    notifyListeners(ServiceEvent::STARTED);

    if (onstart_flags.rw_ready) {
        open_control_socket();
    }
    if (onstart_flags.log_ready) {
        setup_external_log();
    }

    if (force_stop || desired_state == ServiceState::STOPPED) {
        // We must now stop.
        service_set->addToStopQueue(this);
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

void ServiceRecord::failed_to_start(bool depfailed) noexcept
{
    if (!depfailed && onstart_flags.runs_on_console) {
        tcsetpgrp(0, getpgrp());
        releaseConsole();
    }
    
    logServiceFailed(service_name);
    service_state = ServiceState::STOPPED;
    if (start_explicit) {
        start_explicit = false;
        release();
    }
    notifyListeners(ServiceEvent::FAILEDSTART);
    
    // Cancel start of dependents:
    for (sr_iter i = dependents.begin(); i != dependents.end(); i++) {
        if ((*i)->service_state == ServiceState::STARTING) {
            (*i)->prop_failure = true;
            service_set->addToPropQueue(*i);
        }
    }    
    for (auto i = soft_dpts.begin(); i != soft_dpts.end(); i++) {
        // We can send 'start', because this is only a soft dependency.
        // Our startup failure means that they don't have to wait for us.
        if ((*i)->waiting_on) {
            (*i)->holding_acq = false;
            (*i)->waiting_on = false;
            (*i)->getFrom()->dependencyStarted();
            release();
        }
    }
}

bool ServiceRecord::start_ps_process() noexcept
{
    // default implementation: there is no process, so we are started.
    started();
    return true;
}

bool base_process_service::start_ps_process() noexcept
{
    eventLoop.get_time(last_start_time, clock_type::MONOTONIC);
    return start_ps_process(exec_arg_parts, onstart_flags.runs_on_console);
}

bool base_process_service::start_ps_process(const std::vector<const char *> &cmd, bool on_console) noexcept
{
    // In general, you can't tell whether fork/exec is successful. We use a pipe to communicate
    // success/failure from the child to the parent. The pipe is set CLOEXEC so a successful
    // exec closes the pipe, and the parent sees EOF. If the exec is unsuccessful, the errno
    // is written to the pipe, and the parent can read it.

    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) {
        log(LogLevel::ERROR, service_name, ": can't create status check pipe: ", strerror(errno));
        return false;
    }

    const char * logfile = this->logfile.c_str();
    if (*logfile == 0) {
        logfile = "/dev/null";
    }

    bool child_status_registered = false;
    ControlConn *control_conn = nullptr;
    
    int control_socket[2] = {-1, -1};
    if (onstart_flags.pass_cs_fd) {
        if (dinit_socketpair(AF_UNIX, SOCK_STREAM, /* protocol */ 0, control_socket, SOCK_NONBLOCK)) {
            log(LogLevel::ERROR, service_name, ": can't create control socket: ", strerror(errno));
            goto out_p;
        }
        
        // Make the server side socket close-on-exec:
        int fdflags = fcntl(control_socket[0], F_GETFD);
        fcntl(control_socket[0], F_SETFD, fdflags | FD_CLOEXEC);
        
        try {
            control_conn = new ControlConn(&eventLoop, service_set, control_socket[0]);
        }
        catch (std::exception &exc) {
            log(LogLevel::ERROR, service_name, ": can't launch process; out of memory");
            goto out_cs;
        }
    }
    
    // Set up complete, now fork and exec:
    
    pid_t forkpid;
    
    try {
        child_status_listener.add_watch(eventLoop, pipefd[0], IN_EVENTS);
        child_status_registered = true;
        
        forkpid = child_listener.fork(eventLoop);
    }
    catch (std::exception &e) {
        log(LogLevel::ERROR, service_name, ": Could not fork: ", e.what());
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
        child_status_listener.deregister(eventLoop);
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

void ServiceRecord::run_child_proc(const char * const *args, const char *logfile, bool on_console,
        int wpipefd, int csfd) noexcept
{
    // Child process. Must not allocate memory (or otherwise risk throwing any exception)
    // from here until exit().

    // If the console already has a session leader, presumably it is us. On the other hand
    // if it has no session leader, and we don't create one, then control inputs such as
    // ^C will have no effect.
    bool do_set_ctty = (tcgetsid(0) == -1);
    
    // Copy signal mask, but unmask signals that we masked on startup. For the moment, we'll
    // also block all signals, since apparently dup() can be interrupted (!!! really, POSIX??).
    sigset_t sigwait_set;
    sigset_t sigall_set;
    sigfillset(&sigall_set);
    sigprocmask(SIG_SETMASK, &sigall_set, &sigwait_set);
    sigdelset(&sigwait_set, SIGCHLD);
    sigdelset(&sigwait_set, SIGINT);
    sigdelset(&sigwait_set, SIGTERM);
    
    constexpr int bufsz = ((CHAR_BIT * sizeof(pid_t)) / 3 + 2) + 11;
    // "LISTEN_PID=" - 11 characters; the expression above gives a conservative estimate
    // on the maxiumum number of bytes required for LISTEN=nnn, including nul terminator,
    // where nnn is a pid_t in decimal (i.e. one decimal digit is worth just over 3 bits).
    char nbuf[bufsz];
    
    // "DINIT_CS_FD=" - 12 bytes. (we -1 from sizeof(int) in account of sign bit).
    constexpr int csenvbufsz = ((CHAR_BIT * sizeof(int) - 1) / 3 + 2) + 12;
    char csenvbuf[csenvbufsz];
    
    int minfd = (socket_fd == -1) ? 3 : 4;

    // Move wpipefd/csfd to another fd if necessary
    if (wpipefd < minfd) {
        wpipefd = fcntl(wpipefd, F_DUPFD_CLOEXEC, minfd);
        if (wpipefd == -1) goto failure_out;
    }
    
    if (csfd != -1 && csfd < minfd) {
        csfd = fcntl(csfd, F_DUPFD, minfd);
        if (csfd == -1) goto failure_out;
    }
    
    if (socket_fd != -1) {
        
        if (dup2(socket_fd, 3) == -1) goto failure_out;
        if (socket_fd != 3) {
            close(socket_fd);
        }
        
        if (putenv(const_cast<char *>("LISTEN_FDS=1"))) goto failure_out;
        snprintf(nbuf, bufsz, "LISTEN_PID=%jd", static_cast<intmax_t>(getpid()));
        if (putenv(nbuf)) goto failure_out;
    }
    
    if (csfd != -1) {
        snprintf(csenvbuf, csenvbufsz, "DINIT_CS_FD=%d", csfd);
        if (putenv(csenvbuf)) goto failure_out;
    }

    if (! on_console) {
        // Re-set stdin, stdout, stderr
        close(0); close(1); close(2);

        if (open("/dev/null", O_RDONLY) == 0) {
            // stdin = 0. That's what we should have; proceed with opening
            // stdout and stderr.
            if (open(logfile, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR) != 1) {
                goto failure_out;
            }
            if (dup2(1, 2) != 2) {
                goto failure_out;
            }
        }
        else goto failure_out;
        
        // We have the option of creating a new process group and/or session. If
        // we just create a new process group, the child process cannot make itself
        // a session leader if it wants to do that (eg getty/login will generally
        // want this). If we do neither, and we are running with a controlling
        // terminal, a ^C or similar will also affect the child process.
        setsid();
    }
    else {
        // "run on console" - run as a foreground job on the terminal/console device
        
        // if do_set_ctty is false, we are the session leader; we are probably running
        // as a user process. Don't create a new session leader in that case, and run
        // as part of the parent session. Otherwise, the new session cannot claim the
        // terminal as a controlling terminal (it is already claimed), meaning that it
        // will not see control signals from ^C etc.
        
        if (do_set_ctty) {
            // Disable suspend (^Z) (and on some systems, delayed suspend / ^Y)
            signal(SIGTSTP, SIG_IGN);
            
            // Become session leader
            setsid();
            ioctl(0, TIOCSCTTY, 0);
        }
        setpgid(0,0);
        tcsetpgrp(0, getpgrp());
    }
    
    sigprocmask(SIG_SETMASK, &sigwait_set, nullptr);
    
    execvp(args[0], const_cast<char **>(args));
    
    // If we got here, the exec failed:
    failure_out:
    int exec_status = errno;
    write(wpipefd, &exec_status, sizeof(int));
    _exit(0);
}

// Mark this and all dependent services as force-stopped.
void ServiceRecord::forceStop() noexcept
{
    if (service_state != ServiceState::STOPPED) {
        force_stop = true;
        service_set->addToStopQueue(this);
    }
}

void ServiceRecord::dependentStopped() noexcept
{
    if (service_state == ServiceState::STOPPING && waiting_for_deps) {
        service_set->addToPropQueue(this);
    }
}

void ServiceRecord::stop(bool bring_down) noexcept
{
    if (start_explicit) {
        start_explicit = false;
        release();
    }
    
    if (bring_down && desired_state != ServiceState::STOPPED) {
        desired_state = ServiceState::STOPPED;
        service_set->addToStopQueue(this);
    }
}

void ServiceRecord::do_stop() noexcept
{
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

            // We must have had desired_state == STARTED.
            notifyListeners(ServiceEvent::STARTCANCELLED);
            
            // Reaching this point, we have can_interrupt_start() == true. So,
            // we can stop. Dependents might be starting, but they must be
            // waiting on us, so they should also be immediately stoppable.
            // Fall through to below,.
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
        all_deps_stopped();
    }
}

bool ServiceRecord::stopCheckDependents() noexcept
{
    bool all_deps_stopped = true;
    for (sr_iter i = dependents.begin(); i != dependents.end(); ++i) {
        if (! (*i)->is_stopped()) {
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
        if (! (*i)->is_stopped()) {
            // Note we check *first* since if the dependent service is not stopped,
            // 1. We will issue a stop to it shortly and
            // 2. It will notify us when stopped, at which point the stopCheckDependents()
            //    check is run anyway.
            all_deps_stopped = false;
        }

        (*i)->forceStop();
    }

    return all_deps_stopped;
}

// All dependents have stopped; we can stop now, too. Only called when STOPPING.
void ServiceRecord::all_deps_stopped() noexcept
{
    waiting_for_deps = false;
    stopped();
}

void base_process_service::all_deps_stopped() noexcept
{
    waiting_for_deps = false;
    if (pid != -1) {
        // The process is still kicking on - must actually kill it.
        if (! onstart_flags.no_sigterm) {
            kill(pid, SIGTERM);
        }
        if (term_signal != -1) {
            kill(pid, term_signal);
        }

        // In most cases, the rest is done in process_child_callback.
        // If we are a BGPROCESS and the process is not our immediate child, however, that
        // won't work - check for this now:
        if (service_type == ServiceType::BGPROCESS) {
            int status;
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == -1 && errno == ECHILD) {
                // We can't track this child (or it's terminated already)
                stopped();
            }
            else if (r == pid) {
                // Process may have died due to signal since we explicitly requested it to
                // stop by signalling it; no need to log any termination status.
                stopped();
            }
        }
    }
    else {
        // The process is already dead.
        stopped();
    }
}

void scripted_service::all_deps_stopped() noexcept
{
    waiting_for_deps = false;
    if (stop_command.length() == 0) {
        stopped();
    }
    else if (! start_ps_process(stop_arg_parts, false)) {
        // Couldn't execute stop script, but there's not much we can do:
        stopped();
    }
}

void ServiceRecord::unpin() noexcept
{
    if (pinned_started) {
        pinned_started = false;
        if (desired_state == ServiceState::STOPPED) {
            do_stop();
            service_set->processQueues(false);
        }
    }
    if (pinned_stopped) {
        pinned_stopped = false;
        if (desired_state == ServiceState::STARTED) {
            do_start();
            service_set->processQueues(true);
        }
    }
}

void ServiceRecord::queueForConsole() noexcept
{
    service_set->append_console_queue(this);
}

void ServiceRecord::releaseConsole() noexcept
{
    service_set->pullConsoleQueue();
}

void ServiceSet::service_active(ServiceRecord *sr) noexcept
{
    active_services++;
}

void ServiceSet::service_inactive(ServiceRecord *sr) noexcept
{
    active_services--;
}

base_process_service::base_process_service(ServiceSet *sset, string name, ServiceType service_type, string &&command,
        std::list<std::pair<unsigned,unsigned>> &command_offsets,
        sr_list * pdepends_on, sr_list * pdepends_soft)
     : ServiceRecord(sset, name, service_type, std::move(command), command_offsets,
         pdepends_on, pdepends_soft), child_listener(this), child_status_listener(this)
{
    restart_timer.service = this;
    restart_timer.add_timer(eventLoop);
}

void base_process_service::restart_ps_process() noexcept
{
    timespec current_time;
    eventLoop.get_time(current_time, clock_type::MONOTONIC);
    auto tdiff_s = current_time.tv_sec - last_start_time.tv_sec;
    decltype(current_time.tv_nsec) tdiff_ns;
    if (current_time.tv_nsec >= last_start_time.tv_nsec) {
        tdiff_ns = current_time.tv_nsec - last_start_time.tv_nsec;
    }
    else {
        tdiff_s -= 1;
        tdiff_ns = 1000000000 - (last_start_time.tv_nsec - current_time.tv_nsec);
    }

    if (tdiff_s > 0 || tdiff_ns > 200000000) {
        // > 200ms
        start_ps_process();
    }
    else {
        timespec timeout;
        timeout.tv_sec = 0;
        timeout.tv_nsec = 200000000 - tdiff_ns;
        restart_timer.arm_timer_rel(eventLoop, timeout);
    }
}

dasynq::rearm process_restart_timer::timer_expiry(EventLoop_t &, int expiry_count)
{
    return service->restart_timer_expired();
}

dasynq::rearm base_process_service::restart_timer_expired() noexcept
{
    // begin starting process:
    start_ps_process();
    return dasynq::rearm::DISARM;
}
