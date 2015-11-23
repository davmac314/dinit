#include <string>
#include <list>
#include <vector>
#include <csignal>
#include "ev.h"
#include "control.h"

/*
 * Possible service states
 *
 * Services have both a current state and a desired state. The desired state can be
 * either STARTED or STOPPED. The current state can also be STARTING or STOPPING.
 *
 * The total state is a combination of the two, current and desired:
 *      STOPPED/STOPPED  : stopped and will remain stopped
 *      STOPPED/STARTED  :  - (this state cannot occur)
 *      STARTING/STARTED : starting, but not yet started. Dependencies may also be starting.
 *      STARTING/STOPPED : as above, but the service will be stopped again as soon as it has
 *                         completed startup.
 *      STARTED/STARTED  : running and will continue running.
 *      STARTED/STOPPED  :  - (this state cannot occur)
 *      STOPPING/STOPPED : stopping and will stop. Dependents may be stopping.
 *      STOPPING/STARTED : as above, but the service will be re-started again once it stops.
 *
 * A scripted service is in the STARTING/STOPPING states during the script execution.
 * A process service is in the STOPPING state when it has been signalled to stop, and is
 *       in the STARTING state when waiting for dependencies to start.
 */
enum class ServiceState {
    STOPPED,    // service is not running.
    STARTING,   // service is starting, and will start (or fail to start) in time.
    STARTED,    // service is running,
    STOPPING    // service script is stopping and will stop.
};



/* Service types */
enum class ServiceType {
    DUMMY,      // dummy service, used to detect cyclice dependencies
    PROCESS,    // service runs as a process, and can be stopped by
                // sending the process a signal (SIGTERM)
    SCRIPTED,   // service requires an external command to start,
                // and a second command to stop
    INTERNAL    // internal service, runs no external process
};


struct OnstartFlags {
    bool release_console : 1;
    bool rw_ready : 1;
    
    OnstartFlags() noexcept : release_console(false), rw_ready(false)
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
        excDescription = "Not found.";
    }
};

class ServiceCyclicDependency : public ServiceLoadExc
{
    public:
    ServiceCyclicDependency(std::string serviceName) noexcept
        : ServiceLoadExc(serviceName)
    {
        excDescription = "Cyclic dependency.";
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
// store a null terminator for the argument. Return a `char *` array pointing at the beginning
// of each argument. (The returned array is invalidated if the string is later modified).
static const char ** separate_args(std::string &s, std::list<std::pair<unsigned,unsigned>> &arg_indices)
{
    const char ** r = new const char *[arg_indices.size()];
    int i = 0;

    // First store nul terminator for each part:
    for (auto index_pair : arg_indices) {
        if (index_pair.second < s.length()) {
            s[index_pair.second] = 0;
        }
    }

    // Now we can get the C string (c_str) and store offsets into it:
    const char * cstr = s.c_str();
    for (auto index_pair : arg_indices) {
        r[i] = cstr + index_pair.first;
        i++;
    }
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
    const char **exec_arg_parts;  /* pointer to each argument/part of the program_name */
    int num_args;                 /* number of argumrnets (including program) */
    OnstartFlags onstart_flags;

    string logfile; /* log file name, empty string specifies /dev/null */
    bool auto_restart; /* whether to restart this (process) if it dies unexpectedly */


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
    
    // Process services:
    bool force_stop; // true if the service must actually stop. This is the
                     // case if for example the process dies; the service,
                     // and all its dependencies, MUST be stopped.

    int term_signal = -1;  // signal to use for process termination

    // Implementation details
    
    pid_t pid;  /* PID of the process. If state is STARTING or STOPPING,
                   this is PID of the service script; otherwise it is the
                   PID of the process itself (process service).
                   */

    ev_child child_listener;
    
    // All dependents have stopped.
    void allDepsStopped();
    
    // Service has actually stopped (includes having all dependents
    // reaching STOPPED state).
    void stopped() noexcept;
    
    // Service has successfully started
    void started();
    
    // Service failed to start
    void failed_to_start();
    
    // A dependency of this service failed to start.
    void failed_dependency();
    
    // For process services, start the process, return true on success
    bool start_ps_process() noexcept;
    bool start_ps_process(const std::vector<std::string> &args) noexcept;

    // Callback from libev when a child process dies
    static void process_child_callback(struct ev_loop *loop, struct ev_child *w,
            int revents) noexcept;

    // A dependency has reached STARTED state
    void dependencyStarted() noexcept;
    
    void allDepsStarted() noexcept;
    
    // Check whether dependencies have started, and optionally ask them to start
    bool startCheckDependencies(bool do_start) noexcept;

    // A dependent has reached STOPPED state
    void dependentStopped() noexcept;

    // check if all dependents have stopped
    bool stopCheckDependents() noexcept;
    
    // issue a stop to all dependents, return true if they are all already stopped
    bool stopDependents() noexcept;
    
    void forceStop() noexcept; // force-stop this service and all dependents
    
    public:

    ServiceRecord(ServiceSet *set, string name)
        : service_state(ServiceState::STOPPED), desired_state(ServiceState::STOPPED), auto_restart(false), force_stop(false)
    {
        service_set = set;
        service_name = name;
        service_type = ServiceType::DUMMY;
    }
    
    ServiceRecord(ServiceSet *set, string name, ServiceType service_type, string &&command, std::list<std::pair<unsigned,unsigned>> &command_offsets,
            sr_list * pdepends_on, sr_list * pdepends_soft)
        : service_state(ServiceState::STOPPED), desired_state(ServiceState::STOPPED), auto_restart(false), force_stop(false)
    {
        service_set = set;
        service_name = name;
        this->service_type = service_type;
        this->depends_on = std::move(*pdepends_on);

        program_name = command;
        exec_arg_parts = separate_args(program_name, command_offsets);
        num_args = command_offsets.size();

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

    const char *getServiceName() const noexcept { return service_name.c_str(); }
    ServiceState getState() const noexcept { return service_state; }
    
    void start() noexcept;  // start the service
    void stop() noexcept;   // stop the service
    
    bool isDummy() noexcept
    {
        return service_type == ServiceType::DUMMY;
    }
};


class ServiceSet
{
    int active_services;
    std::list<ServiceRecord *> records;
    const char *service_dir;  // directory containing service descriptions
    bool restart_enabled; // whether automatic restart is enabled (allowed)
    ControlConn *rollback_handler; // recieves notification when all services stopped
    
    // Private methods
    
    // Locate an existing service record.
    ServiceRecord *findService(std::string name) noexcept;
    
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
    
    // Stop the service with the given name. The named service will begin
    // transition to the 'stopped' state.
    void stopService(const std::string &name) noexcept;
    
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
    
    void stop_all_services() noexcept
    {
        restart_enabled = false;
        for (std::list<ServiceRecord *>::iterator i = records.begin(); i != records.end(); ++i) {
            (*i)->stop();
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
    
    // Set the rollback handler, which will be notified when all services have stopped.
    // There can be only one rollback handler; attempts to set it when already set will
    // fail. Returns true if successful.
    bool setRollbackHandler(ControlConn *conn) noexcept
    {
        if (rollback_handler == nullptr) {
            rollback_handler = conn;
            return true;
        }
        else {
            return false;
        }
    }
    
    void clearRollbackHandler(ControlConn *conn) noexcept
    {
        if (rollback_handler == conn) {
            rollback_handler = nullptr;
        }
    }
};
