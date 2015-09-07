#include <string>
#include <list>
#include <vector>
#include "ev.h"

/*
 * Possible service states
 *
 * Services have both a current state and a desired state. The desired state can be
 * either SVC_STARTED or SVC_STOPPED. The current state can also be SVC_STARTING
 * or SVC_STOPPING.
 *
 * The total state is a combination of the two, current and desired:
 * SVC_STOPPED/SVC_STOPPED  : stopped and will remain stopped
 * SVC_STOPPED/SVC_STARTED  : stopped and will be started; waiting for dependencies to start.
 * SVC_STARTING/SVC_STARTED : starting, but not yet started. All dependencies have started already.
 * SVC_STARTING/SVC_STOPPED : as above, but the service will be stopped again as soon as it has
 *                            completed startup.
 * SVC_STARTED/SVC_STARTED  : running and will continue running.
 * SVC_STARTED/SVC_STOPPED  : running but will stop; waiting for dependents to stop.
 * SVC_STOPPING/SVC_STOPPED : stopping and will stop. All dependents have stopped.
 * SVC_STOPPING/SVC_STARTED : as above, but the service will be re-started again once it stops.
 *
 * A scripted service is in the STARTING/STOPPING states during the script execution.
 * A process service is in the STOPPING state when it has been signalled to stop (and is never
 *       in the STARTING state; it moves directly from STOPPED to STARTED).
 */
// TODO can we use typesafe enum?
constexpr static int SVC_STOPPED  = 0;  // service is not running
constexpr static int SVC_STARTING = 1;  // service is starting, and will start (or fail to start) in time. All dependencies have started.
constexpr static int SVC_STARTED  = 2;  // service is running
constexpr static int SVC_STOPPING = 3;  // service script is stopping and will stop.


/* Service types */
#define SVC_DUMMY    0  /* dummy service, used to detect cyclic dependencies */
#define SVC_PROCESS  1  /* service runs as a process, and can be stopped
                           by sending the process a signal */
#define SVC_SCRIPTED 2  /* service requires a command to start, and another
                           command to stop */


// Exception loading service
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


class ServiceSet; // forward declaration

class ServiceRecord
{
    typedef std::string string;
    
    string service_name;
    int service_type;  /* SVC_DAEMON or SVC_SCRIPTED */
    int service_state; /* SVC_STOPPED, _STARTING, _STARTED, _STOPPING */
    int desired_state; /* SVC_STOPPED / SVC_STARTED */
    bool force_stop; // true if the service must actually stop. This is the
                     // case if for example the process dies; the service,
                     // and all its dependencies, MUST be stopped.
    string program_name;  /* executable program or script */
    string logfile; /* log file name, empty string specifies /dev/null */
    bool auto_restart; /* whether to restart this (process) if it dies unexpectedly */
    
    typedef std::list<ServiceRecord *> sr_list;
    typedef sr_list::iterator sr_iter;
    
    sr_list depends_on; // services this one depends on
    sr_list dependents; // services depending on this one
    sr_list soft_dependents;  // services depending on this one via a "soft" dependency
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
    bool start_ps_process();
    bool start_ps_process(const std::vector<std::string> &args);
   
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
        : service_state(SVC_STOPPED), desired_state(SVC_STOPPED), force_stop(false), auto_restart(false)
    {
        service_set = set;
        service_name = name;
        service_type = SVC_DUMMY;
    }
    
    ServiceRecord(ServiceSet *set, string name, int service_type, string command,
            sr_list * pdepends_on, sr_list * pdepends_soft)
        : service_state(SVC_STOPPED), desired_state(SVC_STOPPED), force_stop(false), auto_restart(false)
    {
        service_set = set;
        service_name = name;
        this->service_type = service_type;
        program_name = command;
        // TODO splice the contents from the depends_on parameter
        // rather than duplicating the list.
        this->depends_on = *pdepends_on;
        this->depends_on.insert(this->depends_on.end(), pdepends_soft->begin(), pdepends_soft->end());
        
        // For each dependency, add us as a dependent.
        for (sr_iter i = pdepends_on->begin(); i != pdepends_on->end(); ++i) {
            (*i)->dependents.push_back(this);
        }
        for (sr_iter i = pdepends_soft->begin(); i != pdepends_soft->end(); ++i) {
            (*i)->soft_dependents.push_back(this);
        }
    }
    
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
    int getState() const { return service_state; }
    
    void start();  // start the service
    void stop();   // stop the service
    
    bool isDummy()
    {
        return service_type == SVC_DUMMY;
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
    
    // Notification from service that it is active (state != SVC_STOPPED)
    // Only to be called on the transition from inactive to active.
    void service_active(ServiceRecord *);
    
    // Notification from service that it is inactive (SVC_STOPPED)
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
