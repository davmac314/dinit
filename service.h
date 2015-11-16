#include <string>
#include <list>
#include <vector>
#include "ev.h"

/*
 * Possible service states
 *
 * Services have both a current state and a desired state. The desired state can be
 * either STARTED or STOPPED. The current state can also be STARTING or STOPPING.
 *
 * The total state is a combination of the two, current and desired:
 *      STOPPED/STOPPED  : stopped and will remain stopped
 *      STOPPED/STARTED  : stopped and will be started; waiting for dependencies to start.
 *      STARTING/STARTED : starting, but not yet started. All dependencies have started already.
 *      STARTING/STOPPED : as above, but the service will be stopped again as soon as it has
 *                         completed startup.
 *      STARTED/STARTED  : running and will continue running.
 *      STARTED/STOPPED  : running but will stop; waiting for dependents to stop.
 *      STOPPING/STOPPED : stopping and will stop. All dependents have stopped.
 *      STOPPING/STARTED : as above, but the service will be re-started again once it stops.
 *
 * A scripted service is in the STARTING/STOPPING states during the script execution.
 * A process service is in the STOPPING state when it has been signalled to stop (and is never
 *       in the STARTING state; it moves directly from STOPPED to STARTED).
 */
enum class ServiceState {
    STOPPED,    // service is not running.
    STARTING,   // service is starting, and will start (or fail to start) in time. All dependencies have started.
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


// Exception while loading a service
class ServiceLoadExc
{
    public:
    std::string serviceName;
    
    ServiceLoadExc(std::string serviceName)
        : serviceName(serviceName)
    {
    }
};

class ServiceNotFound : public ServiceLoadExc
{
    public:
    ServiceNotFound(std::string serviceName)
        : ServiceLoadExc(serviceName)
    {
    }
};

class ServiceCyclicDependency : public ServiceLoadExc
{
    public:
    ServiceCyclicDependency(std::string serviceName)
        : ServiceLoadExc(serviceName)
    {
    }
};

class ServiceDescriptionExc : public ServiceLoadExc
{
    public:
    std::string extraInfo;
    
    ServiceDescriptionExc(std::string serviceName, std::string extraInfo)
        : ServiceLoadExc(serviceName), extraInfo(extraInfo)
    {
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
    
    ServiceDep(ServiceRecord * from, ServiceRecord * to) : from(from), to(to), waiting_on(false)
    {  }
    
    ServiceRecord * getFrom()
    {
        return from;
    }
    
    ServiceRecord * getTo()
    {
        return to;
    }
};



class ServiceRecord
{
    typedef std::string string;
    
    string service_name;
    ServiceType service_type;  /* ServiceType::DUMMY, PROCESS, SCRIPTED, INTERNAL */
    ServiceState service_state; /* ServiceState::STOPPED, STARTING, STARTED, STOPPING */
    ServiceState desired_state; /* ServiceState::STOPPED / STARTED */
    bool force_stop; // true if the service must actually stop. This is the
                     // case if for example the process dies; the service,
                     // and all its dependencies, MUST be stopped.

    string program_name;          /* storage for program/script and arguments */
    const char **exec_arg_parts;  /* pointer to each argument/part of the program_name */
    int num_args;                 /* number of argumrnets (including program) */

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
    
    // Implementation details
    
    pid_t pid;  /* PID of the process. If state is STARTING or STOPPING,
                   this is PID of the service script; otherwise it is the
                   PID of the process itself (process service).
                   */

    ev_child child_listener;
    
    // Move service to STOPPING state. This can only be called once
    // all dependents have stopped.
    void stopping();
    
    // Service has actually stopped (includes having all dependents
    // reaching STOPPED state).
    void stopped();
    
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
            int revents);
    
    // A dependent has reached STOPPED state
    void dependentStopped();

    // check if all dependents have stopped
    bool stopCheckDependents();
    
    // issue a stop to all dependents, return true if they are all already stopped
    bool stopDependents();
    
    void forceStop(); // force-stop this service and all dependents
    
    public:

    ServiceRecord(ServiceSet *set, string name)
        : service_state(ServiceState::STOPPED), desired_state(ServiceState::STOPPED), force_stop(false), auto_restart(false)
    {
        service_set = set;
        service_name = name;
        service_type = ServiceType::DUMMY;
    }
    
    ServiceRecord(ServiceSet *set, string name, ServiceType service_type, string command, const char ** commands,
            int num_argsx, sr_list * pdepends_on, sr_list * pdepends_soft)
        : service_state(ServiceState::STOPPED), desired_state(ServiceState::STOPPED), force_stop(false), auto_restart(false)
    {
        service_set = set;
        service_name = name;
        this->service_type = service_type;
        this->depends_on = std::move(*pdepends_on);

        program_name = command;
        exec_arg_parts = commands;
        num_args = num_argsx;

        for (sr_iter i = pdepends_on->begin(); i != pdepends_on->end(); ++i) {
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
    void setAutoRestart(bool auto_restart)
    {
        this->auto_restart = auto_restart;
    }
    
    const char *getServiceName() const { return service_name.c_str(); }
    ServiceState getState() const { return service_state; }
    
    void start();  // start the service
    void stop();   // stop the service
    
    bool isDummy()
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
    
    // Private methods
    
    // Locate an existing service record.
    ServiceRecord *findService(std::string name);
    
    // Load a service description, and dependencies, if there is no existing
    // record for the given name.
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
    // Throws an exception if the
    // service description cannot be loaded.
    void startService(const char *name);
    
    // Stop the service with the given name. The named service will begin
    // transition to the 'stopped' state.
    void stopService(const std::string &name);
    
    // Notification from service that it is active (state != STOPPED)
    // Only to be called on the transition from inactive to active.
    void service_active(ServiceRecord *);
    
    // Notification from service that it is inactive (STOPPED)
    // Only to be called on the transition from active to inactive.
    void service_inactive(ServiceRecord *);
    
    // Find out how many services are active (starting, running or stopping,
    // but not stopped).
    int count_active_services()
    {
        return active_services;
    }
    
    void stop_all_services()
    {
        restart_enabled = false;
        for (std::list<ServiceRecord *>::iterator i = records.begin(); i != records.end(); ++i) {
            (*i)->stop();
        }
    }
    
    void set_auto_restart(bool restart)
    {
        restart_enabled = restart;
    }
    
    bool get_auto_restart()
    {
        return restart_enabled;
    }
};
