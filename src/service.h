#ifndef SERVICE_H
#define SERVICE_H

#include <string>
#include <list>
#include <vector>
#include <csignal>
#include <unordered_set>
#include "ev.h"
#include "control.h"
#include "service-listener.h"
#include "service-constants.h"

/*
 * Possible service states
 *
 * Services have both a current state and a desired state. The desired state can be
 * either STARTED or STOPPED. The current state can also be STARTING or STOPPING.
 * A service can be "pinned" in either the STARTED or STOPPED states to prevent it
 * from leaving that state until it is unpinned.
 *
 * The total state is a combination of the two, current and desired:
 *      STOPPED/STOPPED  : stopped and will remain stopped
 *      STOPPED/STARTED  : stopped (pinned), must be unpinned to start
 *      STARTING/STARTED : starting, but not yet started. Dependencies may also be starting.
 *      STARTING/STOPPED : as above, but the service will be stopped again as soon as it has
 *                         completed startup.
 *      STARTED/STARTED  : running and will continue running.
 *      STARTED/STOPPED  : started (pinned), must be unpinned to stop
 *      STOPPING/STOPPED : stopping and will stop. Dependents may be stopping.
 *      STOPPING/STARTED : as above, but the service will be re-started again once it stops.
 *
 * A scripted service is in the STARTING/STOPPING states during the script execution.
 * A process service is in the STOPPING state when it has been signalled to stop, and is
 * in the STARTING state when waiting for dependencies to start or for the exec() call in
 * the forked child to complete and return a status.
 */

struct OnstartFlags {
    bool rw_ready : 1;
    
    // Not actually "onstart" commands:
    bool no_sigterm : 1;  // do not send SIGTERM
    bool runs_on_console : 1;  // run "in the foreground"
    
    OnstartFlags() noexcept : rw_ready(false),
            no_sigterm(false), runs_on_console(false)
    {
    }
};

// Exception while loading a service
class ServiceLoadExc
{
    public:
    std::string serviceName;
    const char *excDescription;
    
    protected:
    ServiceLoadExc(std::string serviceName) noexcept
        : serviceName(serviceName)
    {
    }
};

class ServiceNotFound : public ServiceLoadExc
{
    public:
    ServiceNotFound(std::string serviceName) noexcept
        : ServiceLoadExc(serviceName)
    {
        excDescription = "Service description not found.";
    }
};

class ServiceCyclicDependency : public ServiceLoadExc
{
    public:
    ServiceCyclicDependency(std::string serviceName) noexcept
        : ServiceLoadExc(serviceName)
    {
        excDescription = "Has cyclic dependency.";
    }
};

class ServiceDescriptionExc : public ServiceLoadExc
{
    public:
    std::string extraInfo;
    
    ServiceDescriptionExc(std::string serviceName, std::string extraInfo) noexcept
        : ServiceLoadExc(serviceName), extraInfo(extraInfo)
    {
        excDescription = extraInfo.c_str();
    }    
};

class ServiceRecord; // forward declaration
class ServiceSet; // forward declaration

/* Service dependency record */
class ServiceDep
{
    ServiceRecord * from;
    ServiceRecord * to;

    public:
    /* Whether the 'from' service is waiting for the 'to' service to start */
    bool waiting_on;

    ServiceDep(ServiceRecord * from, ServiceRecord * to) noexcept : from(from), to(to), waiting_on(false)
    {  }

    ServiceRecord * getFrom() noexcept
    {
        return from;
    }

    ServiceRecord * getTo() noexcept
    {
        return to;
    }
};

// Given a string and a list of pairs of (start,end) indices for each argument in that string,
// store a null terminator for the argument. Return a `char *` vector containing the beginning
// of each argument and a trailing nullptr. (The returned array is invalidated if the string is later modified).
static std::vector<const char *> separate_args(std::string &s, std::list<std::pair<unsigned,unsigned>> &arg_indices)
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


class ServiceRecord
{
    typedef std::string string;
    
    string service_name;
    ServiceType service_type;  /* ServiceType::DUMMY, PROCESS, SCRIPTED, INTERNAL */
    ServiceState service_state = ServiceState::STOPPED; /* ServiceState::STOPPED, STARTING, STARTED, STOPPING */
    ServiceState desired_state = ServiceState::STOPPED; /* ServiceState::STOPPED / STARTED */

    string program_name;          /* storage for program/script and arguments */
    std::vector<const char *> exec_arg_parts; /* pointer to each argument/part of the program_name */
    
    string stop_command;          /* storage for stop program/script and arguments */
    std::vector<const char *> stop_arg_parts; /* pointer to each argument/part of the stop_command */
    
    string pid_file;
    
    OnstartFlags onstart_flags;

    string logfile;           // log file name, empty string specifies /dev/null
    
    bool auto_restart : 1;    // whether to restart this (process) if it dies unexpectedly
    bool smooth_recovery : 1; // whether the service process can restart without bringing down service
    
    bool pinned_stopped : 1;
    bool pinned_started : 1;
    bool waiting_for_deps : 1;  // if STARTING, whether we are waiting for dependencies (inc console) to start
    bool waiting_for_execstat : 1;  // if we are waiting for exec status after fork()
    bool doing_recovery : 1;    // if we are currently recovering a BGPROCESS (restarting process, while
                                //   holding STARTED service state)
    bool start_explicit : 1;    // whether we are are explictly required to be started
    int started_deps = 0;       // number of dependents that require this service to be started

    typedef std::list<ServiceRecord *> sr_list;
    typedef sr_list::iterator sr_iter;
    
    // list of soft dependencies
    typedef std::list<ServiceDep> softdep_list;
    
    // list of soft dependents
    typedef std::list<ServiceDep *> softdpt_list;
    
    sr_list depends_on; // services this one depends on
    sr_list dependents; // services depending on this one
    softdep_list soft_deps;  // services this one depends on via a soft dependency
    softdpt_list soft_dpts;  // services depending on this one via a soft dependency
    
    // unsigned wait_count;  /* if we are waiting for dependents/dependencies to
    //                         start/stop, this is how many we're waiting for */
    
    ServiceSet *service_set; // the set this service belongs to
    
    // Next service (after this one) in the queue for the console:
    ServiceRecord *next_for_console;

    std::unordered_set<ServiceListener *> listeners;
    
    // Process services:
    bool force_stop; // true if the service must actually stop. This is the
                     // case if for example the process dies; the service,
                     // and all its dependencies, MUST be stopped.
    
    int term_signal = -1;  // signal to use for process termination
    
    string socket_path; // path to the socket for socket-activation service
    int socket_perms;   // socket permissions ("mode")
    uid_t socket_uid = -1;  // socket user id or -1
    gid_t socket_gid = -1;  // sockget group id or -1

    // Implementation details
    
    pid_t pid = -1;  // PID of the process. If state is STARTING or STOPPING,
                     //   this is PID of the service script; otherwise it is the
                     //   PID of the process itself (process service).
    int exit_status; // Exit status, if the process has exited (pid == -1).
    int socket_fd = -1;  // For socket-activation services, this is the file
                         // descriptor for the socket.

    ev_child child_listener;
    ev_io child_status_listener;
    
    // All dependents have stopped.
    void allDepsStopped();
    
    // Service has actually stopped (includes having all dependents
    // reaching STOPPED state).
    void stopped() noexcept;
    
    // Service has successfully started
    void started() noexcept;
    
    // Service failed to start (only called when in STARTING state).
    //   dep_failed: whether failure is recorded due to a dependency failing
    void failed_to_start(bool dep_failed = false) noexcept;

    // For process services, start the process, return true on success
    bool start_ps_process() noexcept;
    bool start_ps_process(const std::vector<const char *> &args, bool on_console) noexcept;
    
    // Callback from libev when a child process dies
    static void process_child_callback(struct ev_loop *loop, struct ev_child *w,
            int revents) noexcept;
    
    static void process_child_status(struct ev_loop *loop, ev_io * stat_io,
            int revents) noexcept;
    
    void handle_exit_status() noexcept;
    
    // A dependency has reached STARTED state
    void dependencyStarted() noexcept;
    
    void allDepsStarted(bool haveConsole = false) noexcept;
    
    // Read the pid-file, return false on failure
    bool read_pid_file() noexcept;
    
    // Open the activation socket, return false on failure
    bool open_socket() noexcept;
    
    // Check whether dependencies have started, and optionally ask them to start
    bool startCheckDependencies(bool do_start) noexcept;
    
    // Whether a STARTING service can immediately transition to STOPPED (as opposed to
    // having to wait for it reach STARTED and then go through STOPPING).
    bool can_interrupt_start() noexcept
    {
        return waiting_for_deps;
    }
    
    // Whether a STOPPING service can immediately transition to STARTED.
    bool can_interrupt_stop() noexcept
    {
        return waiting_for_deps && ! force_stop;
    }

    // A dependent has reached STOPPED state
    void dependentStopped() noexcept;

    // check if all dependents have stopped
    bool stopCheckDependents() noexcept;
    
    // issue a stop to all dependents, return true if they are all already stopped
    bool stopDependents() noexcept;
    
    void forceStop() noexcept; // force-stop this service and all dependents
    
    void notifyListeners(ServiceEvent event) noexcept
    {
        for (auto l : listeners) {
            l->serviceEvent(this, event);
        }
    }
    
    // Queue to run on the console. 'acquiredConsole()' will be called when the console is available.
    void queueForConsole() noexcept;
    
    // Console is available.
    void acquiredConsole() noexcept;
    
    // Release console (console must be currently held by this service)
    void releaseConsole() noexcept;
    
    bool get_start_flag(bool transitive)
    {
        return transitive ? started_deps != 0 : start_explicit;
    }
    
    public:

    ServiceRecord(ServiceSet *set, string name)
        : service_state(ServiceState::STOPPED), desired_state(ServiceState::STOPPED), auto_restart(false),
            pinned_stopped(false), pinned_started(false), waiting_for_deps(false),
            waiting_for_execstat(false), doing_recovery(false),
            start_explicit(false), force_stop(false)
    {
        service_set = set;
        service_name = name;
        service_type = ServiceType::DUMMY;
    }
    
    ServiceRecord(ServiceSet *set, string name, ServiceType service_type, string &&command, std::list<std::pair<unsigned,unsigned>> &command_offsets,
            sr_list * pdepends_on, sr_list * pdepends_soft)
        : ServiceRecord(set, name)
    {
        service_set = set;
        service_name = name;
        this->service_type = service_type;
        this->depends_on = std::move(*pdepends_on);

        program_name = command;
        exec_arg_parts = separate_args(program_name, command_offsets);

        for (sr_iter i = depends_on.begin(); i != depends_on.end(); ++i) {
            (*i)->dependents.push_back(this);
        }

        // Soft dependencies
        auto b_iter = soft_deps.end();
        for (sr_iter i = pdepends_soft->begin(); i != pdepends_soft->end(); ++i) {
            b_iter = soft_deps.emplace(b_iter, this, *i);
            (*i)->soft_dpts.push_back(&(*b_iter));
            ++b_iter;
        }
    }
    
    // TODO write a destructor
    
    // Set the stop command and arguments (may throw std::bad_alloc)
    void setStopCommand(std::string command, std::list<std::pair<unsigned,unsigned>> &stop_command_offsets)
    {
        stop_command = command;
        stop_arg_parts = separate_args(stop_command, stop_command_offsets);
    }
    
    // Get the current service state.
    ServiceState getState() noexcept
    {
        return service_state;
    }
    
    // Get the target (aka desired) state.
    ServiceState getTargetState() noexcept
    {
        return desired_state;
    }

    // Set logfile, should be done before service is started
    void setLogfile(string logfile)
    {
        this->logfile = logfile;
    }
    
    // Set whether this service should automatically restart when it dies
    void setAutoRestart(bool auto_restart) noexcept
    {
        this->auto_restart = auto_restart;
    }
    
    void setSmoothRecovery(bool smooth_recovery) noexcept
    {
        this->smooth_recovery = smooth_recovery;
    }
    
    // Set "on start" flags (commands)
    void setOnstartFlags(OnstartFlags flags) noexcept
    {
        this->onstart_flags = flags;
    }
    
    // Set an additional signal (other than SIGTERM) to be used to terminate the process
    void setExtraTerminationSignal(int signo) noexcept
    {
        this->term_signal = signo;
    }
    
    void set_pid_file(string &&pid_file) noexcept
    {
        this->pid_file = pid_file;
    }
    
    void set_socket_details(string &&socket_path, int socket_perms, uid_t socket_uid, uid_t socket_gid) noexcept
    {
        this->socket_path = socket_path;
        this->socket_perms = socket_perms;
        this->socket_uid = socket_uid;
        this->socket_gid = socket_gid;
    }

    const char *getServiceName() const noexcept { return service_name.c_str(); }
    ServiceState getState() const noexcept { return service_state; }
    
    void start(bool transitive = false) noexcept;  // start the service
    void stop() noexcept;   // stop the service
    void do_start() noexcept;
    void do_stop() noexcept;
    
    void pinStart() noexcept;  // start the service and pin it
    void pinStop() noexcept;   // stop the service and pin it
    void unpin() noexcept;     // unpin the service
    
    bool isDummy() noexcept
    {
        return service_type == ServiceType::DUMMY;
    }
    
    // Add a listener. A listener must only be added once. May throw std::bad_alloc.
    void addListener(ServiceListener * listener)
    {
        listeners.insert(listener);
    }
    
    // Remove a listener.    
    void removeListener(ServiceListener * listener) noexcept
    {
        listeners.erase(listener);
    }
};


class ServiceSet
{
    int active_services;
    std::list<ServiceRecord *> records;
    const char *service_dir;  // directory containing service descriptions
    bool restart_enabled; // whether automatic restart is enabled (allowed)
    
    ShutdownType shutdown_type = ShutdownType::CONTINUE;  // Shutdown type, if stopping
    
    ServiceRecord * console_queue_tail = nullptr; // last record in console queue
    
    // Private methods
        
    // Load a service description, and dependencies, if there is no existing
    // record for the given name.
    // Throws:
    //   ServiceLoadException (or subclass) on problem with service description
    //   std::bad_alloc on out-of-memory condition
    ServiceRecord *loadServiceRecord(const char *name);

    // Public
    
    public:
    ServiceSet(const char *service_dir)
    {
        this->service_dir = service_dir;
        active_services = 0;
        restart_enabled = true;
    }
    
    // Start the service with the given name. The named service will begin
    // transition to the 'started' state.
    //
    // Throws a ServiceLoadException (or subclass) if the service description
    // cannot be loaded or is invalid;
    // Throws std::bad_alloc if out of memory.
    void startService(const char *name);
    
    // Locate an existing service record.
    ServiceRecord *findService(const std::string &name) noexcept;
    
    // Find a loaded service record, or load it if it is not loaded.
    // Throws:
    //   ServiceLoadException (or subclass) on problem with service description
    //   std::bad_alloc on out-of-memory condition 
    ServiceRecord *loadService(const std::string &name)
    {
        ServiceRecord *record = findService(name);
        if (record == nullptr) {
            record = loadServiceRecord(name.c_str());
        }
        return record;
    }
    
    // Stop the service with the given name. The named service will begin
    // transition to the 'stopped' state.
    void stopService(const std::string &name) noexcept;
    
    // Set the console queue tail (returns previous tail)
    ServiceRecord * consoleQueueTail(ServiceRecord * newTail) noexcept
    {
        auto prev_tail = console_queue_tail;
        console_queue_tail = newTail;
        return prev_tail;
    }
    
    // Notification from service that it is active (state != STOPPED)
    // Only to be called on the transition from inactive to active.
    void service_active(ServiceRecord *) noexcept;
    
    // Notification from service that it is inactive (STOPPED)
    // Only to be called on the transition from active to inactive.
    void service_inactive(ServiceRecord *) noexcept;
    
    // Find out how many services are active (starting, running or stopping,
    // but not stopped).
    int count_active_services() noexcept
    {
        return active_services;
    }
    
    void stop_all_services(ShutdownType type = ShutdownType::HALT) noexcept
    {
        restart_enabled = false;
        shutdown_type = type;
        for (std::list<ServiceRecord *>::iterator i = records.begin(); i != records.end(); ++i) {
            (*i)->stop();
            (*i)->unpin();
        }
    }
    
    void set_auto_restart(bool restart) noexcept
    {
        restart_enabled = restart;
    }
    
    bool get_auto_restart() noexcept
    {
        return restart_enabled;
    }
    
    ShutdownType getShutdownType() noexcept
    {
        return shutdown_type;
    }
};

#endif
