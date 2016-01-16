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
    if (service_type != ServiceType::SCRIPTED && service_type != ServiceType::BGPROCESS && onstart_flags.runs_on_console) {
        tcsetpgrp(0, getpgrp());
        releaseConsole();
    }

    logServiceStopped(service_name);
    service_state = ServiceState::STOPPED;
    force_stop = false;
    
    notifyListeners(ServiceEvent::STOPPED);
    
    for (auto dependency : depends_on) {
        dependency->dependentStopped();
    }
    
    if (desired_state == ServiceState::STARTED && service_set->get_auto_restart()) {
        // Desired state is "started".
        do_start();
    }
    else {
        if (socket_fd != -1) {
            close(socket_fd);
            socket_fd = -1;
        }
        
        if (required_by == 0) {
            // Service is now completely inactive.
            release_dependencies();
        }
    }
}

void ServiceRecord::process_child_callback(struct ev_loop *loop, ev_child *w, int revents) noexcept
{
    ServiceRecord *sr = (ServiceRecord *) w->data;

    sr->pid = -1;
    sr->exit_status = w->rstatus;
    ev_child_stop(loop, w);
    
    // Ok, for a process service, any process death which we didn't rig
    // ourselves is a bit... unexpected. Probably, the child died because
    // we asked it to (sr->service_state == STOPPING). But even if
    // we didn't, there's not much we can do.
    
    if (sr->waiting_for_execstat) {
        // We still don't have an exec() status from the forked child, wait for that
        // before doing any further processing.
        return;
    }
    
    sr->handle_exit_status();
}

bool ServiceRecord::do_auto_restart() noexcept
{
    if (auto_restart) {
        return service_set->get_auto_restart();
    }
    return false;
}

void ServiceRecord::handle_exit_status() noexcept
{
    if (exit_status != 0 && service_state != ServiceState::STOPPING) {
        log(LogLevel::ERROR, "Service ", service_name, " process terminated with exit code ", exit_status);
    }

    if (doing_recovery) {
        // (BGPROCESS only)
        doing_recovery = false;
        bool need_stop = false;
        if (exit_status != 0) {
            need_stop = true;
        }
        else {
            // We need to re-read the PID, since it has now changed.
            if (service_type == ServiceType::BGPROCESS && pid_file.length() != 0) {
                if (! read_pid_file()) {
                    need_stop = true;
                }
            }
        }
        
        if (need_stop) {
            // Failed startup: no auto-restart.
            desired_state = ServiceState::STOPPED;
            do_stop();
        }
        
        return;
    }
    
    if (service_type == ServiceType::PROCESS || service_type == ServiceType::BGPROCESS) {
        if (service_state == ServiceState::STARTING) {
            // (only applies to BGPROCESS)
            if (exit_status == 0) {
                started();
            }
            else {
                failed_to_start();
            }
        }
        else if (service_state == ServiceState::STOPPING) {
            // TODO log non-zero rstatus?
            stopped();
        }
        else if (smooth_recovery && service_state == ServiceState::STARTED) {
            // TODO ensure a minimum time between restarts
            // TODO if we are pinned-started then we should probably check
            //      that dependencies have started before trying to re-start the
            //      service process.
            doing_recovery = (service_type == ServiceType::BGPROCESS);
            start_ps_process();
            return;
        }
        else {
            if (! do_auto_restart()) desired_state = ServiceState::STOPPED;
            forceStop();
        }
    }
    else {  // SCRIPTED
        if (service_state == ServiceState::STOPPING) {
            if (exit_status == 0) {
                stopped();
            }
            else {
                // ??? failed to stop! Let's log it as info:
                log(LogLevel::INFO, "service ", service_name, " stop command failed with exit code ", exit_status);
                // Just assume that we stopped, so that any dependencies
                // can be stopped:
                stopped();
            }
        }
        else { // STARTING
            if (exit_status == 0) {
                started();
            }
            else {
                // failed to start
                log(LogLevel::ERROR, "service ", service_name, " command failed with exit code ", exit_status);
                failed_to_start();
            }
        }
    }
}

void ServiceRecord::process_child_status(struct ev_loop *loop, ev_io * stat_io, int revents) noexcept
{
    ServiceRecord *sr = (ServiceRecord *) stat_io->data;
    sr->waiting_for_execstat = false;
    
    int exec_status;
    int r = read(stat_io->fd, &exec_status, sizeof(int));
    close(stat_io->fd);
    ev_io_stop(loop, stat_io);
    
    if (r != 0) {
        // We read an errno code; exec() failed, and the service startup failed.
        sr->pid = -1;
        log(LogLevel::ERROR, sr->service_name, ": execution failed: ", strerror(exec_status));
        if (sr->service_state == ServiceState::STARTING) {
            sr->failed_to_start();
        }
        else if (sr->service_state == ServiceState::STOPPING) {
            // Must be a scripted servce. We've logged the failure, but it's probably better
            // not to leave the service in STARTED state:
            sr->stopped();
        }
    }
    else {
        // exec() succeeded.
        if (sr->service_type == ServiceType::PROCESS) {
            if (sr->service_state != ServiceState::STARTED) {
                sr->started();
            }
        }
        
        if (sr->pid == -1) {
            // Somehow the process managed to complete before we even saw the status.
            sr->handle_exit_status();
        }
    }
}

void ServiceRecord::require() noexcept
{
    if (required_by++ == 0) {
        // Need to require all our dependencies
        for (sr_iter i = depends_on.begin(); i != depends_on.end(); ++i) {
            (*i)->require();
        }

        for (auto i = soft_deps.begin(); i != soft_deps.end(); ++i) {
            ServiceRecord * to = i->getTo();
            to->require();
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
        // Can stop, and release dependencies once we're stopped.
        if (service_state == ServiceState::STOPPED) {
            release_dependencies();
        }
        else {
            do_stop();
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
        to->release();
    }
    
    service_set->service_inactive(this);
}

void ServiceRecord::start(bool activate) noexcept
{
    if (activate) {
        if (!start_explicit) require();
        start_explicit = true;
    }
    
    if (desired_state == ServiceState::STARTED && service_state != ServiceState::STOPPED) return;
    
    if (required_by == 0) {
        service_set->service_active(this);
    }

    desired_state = ServiceState::STARTED;
    do_start();
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

    int sockfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
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

    if (service_type == ServiceType::PROCESS || service_type == ServiceType::BGPROCESS
            || service_type == ServiceType::SCRIPTED) {
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
        enable_console_log(false);
        allDepsStarted(true);
    }
    else {
        // We got the console but can't use it yet.
        releaseConsole();
    }
}

bool ServiceRecord::read_pid_file() noexcept
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
                ev_child_init(&child_listener, process_child_callback, pid, 0);
                child_listener.data = this;
                ev_child_start(ev_default_loop(EVFLAG_AUTO), &child_listener);
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
    
    if (service_type == ServiceType::BGPROCESS && pid_file.length() != 0) {
        if (! read_pid_file()) {
            failed_to_start();
            return;
        }
    }
    
    logServiceStarted(service_name);
    service_state = ServiceState::STARTED;
    notifyListeners(ServiceEvent::STARTED);

    if (onstart_flags.rw_ready) {
        open_control_socket(ev_default_loop(EVFLAG_AUTO));
    }

    if (force_stop || desired_state == ServiceState::STOPPED) {
        // We must now stop.
        do_stop();
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
    notifyListeners(ServiceEvent::FAILEDSTART);
    
    // Cancel start of dependents:
    for (sr_iter i = dependents.begin(); i != dependents.end(); i++) {
        if ((*i)->service_state == ServiceState::STARTING) {
            (*i)->failed_to_start(true);
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
    return start_ps_process(exec_arg_parts, onstart_flags.runs_on_console);
}

bool ServiceRecord::start_ps_process(const std::vector<const char *> &cmd, bool on_console) noexcept
{
    // In general, you can't tell whether fork/exec is successful. We use a pipe to communicate
    // success/failure from the child to the parent. The pipe is set CLOEXEC so a successful
    // exec closes the pipe, and the parent sees EOF. If the exec is unsuccessful, the errno
    // is written to the pipe, and the parent can read it.

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

        constexpr int bufsz = ((CHAR_BIT * sizeof(pid_t) - 1) / 3 + 2) + 11;
        // "LISTEN_PID=" - 11 characters
        char nbuf[bufsz];

        if (socket_fd != -1) {
            dup2(socket_fd, 3);
            if (socket_fd != 3) {
                close(socket_fd);
            }
            
            if (putenv(const_cast<char *>("LISTEN_FDS=1"))) goto failure_out;
            
            snprintf(nbuf, bufsz, "LISTEN_PID=%jd", static_cast<intmax_t>(getpid()));
            
            if (putenv(nbuf)) goto failure_out;
        }

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
        failure_out:
        int exec_status = errno;
        write(pipefd[1], &exec_status, sizeof(int));
        exit(0);
    }
    else {
        // Parent process
        close(pipefd[1]); // close the 'other end' fd

        pid = forkpid;

        // Listen for status
        ev_io_init(&child_status_listener, process_child_status, pipefd[0], EV_READ);
        child_status_listener.data = this;
        ev_io_start(ev_default_loop(EVFLAG_AUTO), &child_status_listener);

        // Add a process listener so we can detect when the
        // service stops
        ev_child_init(&child_listener, process_child_callback, pid, 0);
        child_listener.data = this;
        ev_child_start(ev_default_loop(EVFLAG_AUTO), &child_listener);
        waiting_for_execstat = true;
        return true;
    }
}

// Mark this and all dependent services as force-stopped.
void ServiceRecord::forceStop() noexcept
{
    if (service_state != ServiceState::STOPPED) {
        force_stop = true;
        do_stop();
    }
}

void ServiceRecord::dependentStopped() noexcept
{
    if (service_state == ServiceState::STOPPING && waiting_for_deps) {
        // Check the other dependents before we stop.
        if (stopCheckDependents()) {
            allDepsStopped();
        }
    }
}

void ServiceRecord::stop(bool bring_down) noexcept
{
    if (start_explicit) {
        start_explicit = false;
        release();
    }
    
    if (bring_down) {
        desired_state = ServiceState::STOPPED;
        do_stop();
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
        allDepsStopped();
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
            all_deps_stopped = false;
        }
        if (force_stop) {
            (*i)->forceStop();
        }
        else {
            (*i)->do_stop();
        }
    }
    
    return all_deps_stopped;
}

// Dependency stopped or is stopping; we must stop too.
void ServiceRecord::allDepsStopped()
{
    waiting_for_deps = false;
    if (service_type == ServiceType::PROCESS || service_type == ServiceType::BGPROCESS) {
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
        if (stop_command.length() == 0) {
            stopped();
        }
        else if (! start_ps_process(stop_arg_parts, false)) {
            // Couldn't execute stop script, but there's not much we can do:
            stopped();
        }
    }
    else {
        stopped();
    }
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
            do_start();
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
    enable_console_log(true);
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
