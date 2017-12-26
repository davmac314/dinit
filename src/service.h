#ifndef SERVICE_H
#define SERVICE_H

#include <string>
#include <list>
#include <vector>
#include <csignal>
#include <unordered_set>
#include <algorithm>

#include "dasynq.h"

#include "control.h"
#include "service-listener.h"
#include "service-constants.h"
#include "dinit-ll.h"

/*
 * This header defines service_record, a data record maintaining information about a service,
 * and service_set, a set of interdependent service records. It also defines some associated
 * types and exceptions.
 *
 * Service states
 * --------------
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
 *
 * Acquisition/release:
 * ------------------
 * Each service has a dependent-count ("required_by"). This starts at 0, adds 1 if the
 * service has explicitly been started (i.e. "start_explicit" is true), and adds 1 for
 * each dependent service which is not STOPPED (including dependents with a soft dependency).
 * When required_by transitions to 0, the service is stopped (unless it is pinned). When
 * require_by transitions from 0, the service is started (unless pinned).
 *
 * So, in general, the dependent-count determines the desired state (STARTED if the count
 * is greater than 0, otherwise STOPPED). However, a service can be issued a stop-and-take
 * down order (via `stop(true)'); this will first stop dependent services, which may restart
 * and cancel the stop of the former service. Finally, a service can be force-stopped, which
 * means that its stop process cannot be cancelled (though it may still be put in a desired
 * state of STARTED, meaning it will start immediately upon stopping).
 *
 * Pinning
 * -------
 * A service may be "pinned" in either STARTED or STOPPED states (or even both). Once it
 * reaches a pinned state, a service will not leave that state, though its desired state
 * may still be set. (Note that pinning prevents, but never causes, state transition).
 *
 * The priority of the different state deciders is:
 *  - pins
 *  - force stop flag
 *  - desired state (which is manipulated by require/release operations)
 *
 * So a forced stop cannot occur until the service is not pinned started, for instance.
 *
 * Two-phase transition
 * --------------------
 * Transition between states occurs in two phases: propagation and execution. In both phases
 * a linked-list queue is used to keep track of which services need processing; this avoids
 * recursion (which would be of unknown depth and therefore liable to stack overflow).
 *
 * In the propagation phase, acquisition/release messages are processed, and desired state may be
 * altered accordingly. Start and stop requests are also propagated in this phase. The state may
 * be set to STARTING or STOPPING to reflect the desired state, but will never be set to STARTED
 * or STOPPED (that happens in the execution phase).
 *
 * Propagation variables:
 *   prop_acquire:  the service has transitioned to an acquired state and must issue an acquire
 *                  on its dependencies
 *   prop_release:  the service has transitioned to a released state and must issue a release on
 *                  its dependencies.
 *
 *   prop_start:    the service should start
 *   prop_stop:     the service should stop
 *
 * Note that "prop_acquire"/"prop_release" form a pair which cannot both be set at the same time
 * which is enforced via explicit checks. For "prop_start"/"prop_stop" this occurs implicitly.
 *
 * In the execution phase, actions are taken to achieve the desired state. Actual state may
 * transition according to the current and desired states. Processes can be sent signals, etc
 * in order to stop them. A process can restart if it stops, but it does so by raising prop_start
 * which needs to be processed in a second transition phase. Seeing as starting never causes
 * another process to stop, the transition-execute-transition cycle always ends at the 2nd
 * transition stage, at the latest.
 */

struct onstart_flags_t {
    bool rw_ready : 1;
    bool log_ready : 1;
    
    // Not actually "onstart" commands:
    bool no_sigterm : 1;  // do not send SIGTERM
    bool runs_on_console : 1;  // run "in the foreground"
    bool starts_on_console : 1; // starts in the foreground
    bool pass_cs_fd : 1;  // pass this service a control socket connection via fd
    
    onstart_flags_t() noexcept : rw_ready(false), log_ready(false),
            no_sigterm(false), runs_on_console(false), starts_on_console(false), pass_cs_fd(false)
    {
    }
};

// Exception while loading a service
class service_load_exc
{
    public:
    std::string serviceName;
    std::string excDescription;
    
    protected:
    service_load_exc(std::string serviceName, std::string &&desc) noexcept
        : serviceName(serviceName), excDescription(std::move(desc))
    {
    }
};

class service_not_found : public service_load_exc
{
    public:
    service_not_found(std::string serviceName) noexcept
        : service_load_exc(serviceName, "Service description not found.")
    {
    }
};

class service_cyclic_dependency : public service_load_exc
{
    public:
    service_cyclic_dependency(std::string serviceName) noexcept
        : service_load_exc(serviceName, "Has cyclic dependency.")
    {
    }
};

class service_description_exc : public service_load_exc
{
    public:
    service_description_exc(std::string serviceName, std::string &&extraInfo) noexcept
        : service_load_exc(serviceName, std::move(extraInfo))
    {
    }    
};

class service_record;
class service_set;
class base_process_service;

enum class dependency_type
{
    REGULAR,
    SOFT,       // dependency starts in parallel, failure/stop does not affect dependent
    WAITS_FOR,  // as for SOFT, but dependent waits until dependency starts/fails before starting
    MILESTONE   // dependency must start successfully, but once started the dependency becomes soft
};

/* Service dependency record */
class service_dep
{
    service_record * from;
    service_record * to;

    public:
    /* Whether the 'from' service is waiting for the 'to' service to start */
    bool waiting_on;
    /* Whether the 'from' service is holding an acquire on the 'to' service */
    bool holding_acq;

    const dependency_type dep_type;

    service_dep(service_record * from, service_record * to, dependency_type dep_type_p) noexcept
            : from(from), to(to), waiting_on(false), holding_acq(false), dep_type(dep_type_p)
    {  }

    service_record * get_from() noexcept
    {
        return from;
    }

    service_record * get_to() noexcept
    {
        return to;
    }
};

/* preliminary service dependency information */
class prelim_dep
{
    public:
    service_record * const to;
    dependency_type const dep_type;

    prelim_dep(service_record *to_p, dependency_type dep_type_p) : to(to_p), dep_type(dep_type_p)
    {
        //
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

class service_child_watcher : public eventloop_t::child_proc_watcher_impl<service_child_watcher>
{
    public:
    base_process_service * service;
    rearm status_change(eventloop_t &eloop, pid_t child, int status) noexcept;
    
    service_child_watcher(base_process_service * sr) noexcept : service(sr) { }
};

// Watcher for the pipe used to receive exec() failure status errno
class exec_status_pipe_watcher : public eventloop_t::fd_watcher_impl<exec_status_pipe_watcher>
{
    public:
    base_process_service * service;
    rearm fd_event(eventloop_t &eloop, int fd, int flags) noexcept;
    
    exec_status_pipe_watcher(base_process_service * sr) noexcept : service(sr) { }
};

// service_record: base class for service record containing static information
// and current state of each service.
class service_record
{
    protected:
    typedef std::string string;
    
    string service_name;
    service_type record_type;  /* ServiceType::DUMMY, PROCESS, SCRIPTED, INTERNAL */
    service_state_t service_state = service_state_t::STOPPED; /* service_state_t::STOPPED, STARTING, STARTED, STOPPING */
    service_state_t desired_state = service_state_t::STOPPED; /* service_state_t::STOPPED / STARTED */

    string program_name;          // storage for program/script and arguments
    std::vector<const char *> exec_arg_parts; // pointer to each argument/part of the program_name, and nullptr
    
    string stop_command;          // storage for stop program/script and arguments
    std::vector<const char *> stop_arg_parts; // pointer to each argument/part of the stop_command, and nullptr
    
    string pid_file;
    
    onstart_flags_t onstart_flags;

    string logfile;           // log file name, empty string specifies /dev/null
    
    bool auto_restart : 1;    // whether to restart this (process) if it dies unexpectedly
    bool smooth_recovery : 1; // whether the service process can restart without bringing down service
    
    bool pinned_stopped : 1;
    bool pinned_started : 1;
    bool waiting_for_deps : 1;  // if STARTING, whether we are waiting for dependencies (inc console) to start
    bool waiting_for_execstat : 1;  // if we are waiting for exec status after fork()
    bool start_explicit : 1;    // whether we are are explictly required to be started

    bool prop_require : 1;      // require must be propagated
    bool prop_release : 1;      // release must be propagated
    bool prop_failure : 1;      // failure to start must be propagated
    bool prop_start   : 1;
    bool prop_stop    : 1;
    bool restarting   : 1;      // re-starting after unexpected termination
    
    int required_by = 0;        // number of dependents wanting this service to be started

    typedef std::list<service_record *> sr_list;
    typedef sr_list::iterator sr_iter;
    
    // list of soft dependencies
    typedef std::list<service_dep> dep_list;
    
    // list of soft dependents
    typedef std::list<service_dep *> dpt_list;
    
    dep_list depends_on;  // services this one depends on
    dpt_list dependents;  // services depending on this one
    
    service_set *services; // the set this service belongs to
    
    std::unordered_set<service_listener *> listeners;
    
    // Process services:
    bool force_stop; // true if the service must actually stop. This is the
                     // case if for example the process dies; the service,
                     // and all its dependencies, MUST be stopped.
    
    int term_signal = -1;  // signal to use for process termination
    
    string socket_path; // path to the socket for socket-activation service
    int socket_perms;   // socket permissions ("mode")
    uid_t socket_uid = -1;  // socket user id or -1
    gid_t socket_gid = -1;  // socket group id or -1

    // Implementation details
    
    pid_t pid = -1;  // PID of the process. If state is STARTING or STOPPING,
                     //   this is PID of the service script; otherwise it is the
                     //   PID of the process itself (process service).
    int exit_status; // Exit status, if the process has exited (pid == -1).
    int socket_fd = -1;  // For socket-activation services, this is the file
                         // descriptor for the socket.
    
    // Data for use by service_set
    public:
    
    // Console queue.
    lld_node<service_record> console_queue_node;
    
    // Propagation and start/stop queues
    lls_node<service_record> prop_queue_node;
    lls_node<service_record> stop_queue_node;
    
    protected:
    
    // stop immediately
    void emergency_stop() noexcept;

    // All dependents have stopped.
    virtual void all_deps_stopped() noexcept;
    
    // Service has actually stopped (includes having all dependents
    // reaching STOPPED state).
    void stopped() noexcept;
    
    // Service has successfully started
    void started() noexcept;
    
    // Service failed to start (only called when in STARTING state).
    //   dep_failed: whether failure is recorded due to a dependency failing
    void failed_to_start(bool dep_failed = false) noexcept;

    void run_child_proc(const char * const *args, const char *logfile, bool on_console, int wpipefd,
            int csfd) noexcept;
    
    // A dependency has reached STARTED state
    void dependencyStarted() noexcept;
    
    void all_deps_started(bool haveConsole = false) noexcept;

    // Do any post-dependency startup; return false on failure
    virtual bool start_ps_process() noexcept;

    // Open the activation socket, return false on failure
    bool open_socket() noexcept;

    // Start all dependencies, return true if all have started
    bool start_check_dependencies() noexcept;

    // Check whether all dependencies have started (i.e. whether we can start now)
    bool check_deps_started() noexcept;

    // Whether a STARTING service can immediately transition to STOPPED (as opposed to
    // having to wait for it reach STARTED and then go through STOPPING).
    virtual bool can_interrupt_start() noexcept
    {
        return waiting_for_deps;
    }
    
    virtual void interrupt_start() noexcept;

    // Whether a STOPPING service can immediately transition to STARTED.
    bool can_interrupt_stop() noexcept
    {
        return waiting_for_deps && ! force_stop;
    }

    // A dependent has reached STOPPED state
    void dependent_stopped() noexcept;

    // check if all dependents have stopped
    bool stop_check_dependents() noexcept;
    
    // issue a stop to all dependents, return true if they are all already stopped
    bool stop_dependents() noexcept;
    
    void require() noexcept;
    void release() noexcept;
    void release_dependencies() noexcept;
    
    // Check if service is, fundamentally, stopped.
    bool is_stopped() noexcept
    {
        return service_state == service_state_t::STOPPED
            || (service_state == service_state_t::STARTING && waiting_for_deps);
    }
    
    void notify_listeners(service_event event) noexcept
    {
        for (auto l : listeners) {
            l->serviceEvent(this, event);
        }
    }
    
    // Queue to run on the console. 'acquired_console()' will be called when the console is available.
    // Has no effect if the service has already queued for console.
    void queue_for_console() noexcept;
    
    // Release console (console must be currently held by this service)
    void release_console() noexcept;
    
    bool do_auto_restart() noexcept;

    // Started state reached
    bool process_started() noexcept;

    public:

    service_record(service_set *set, string name)
        : service_state(service_state_t::STOPPED), desired_state(service_state_t::STOPPED),
            auto_restart(false), smooth_recovery(false),
            pinned_stopped(false), pinned_started(false), waiting_for_deps(false),
            waiting_for_execstat(false), start_explicit(false),
            prop_require(false), prop_release(false), prop_failure(false),
            prop_start(false), prop_stop(false), restarting(false), force_stop(false)
    {
        services = set;
        service_name = name;
        record_type = service_type::DUMMY;
        socket_perms = 0;
        exit_status = 0;
    }

    service_record(service_set *set, string name, service_type record_type_p,
            const std::list<prelim_dep> &deplist_p)
        : service_record(set, name)
    {
        services = set;
        service_name = name;
        this->record_type = record_type_p;

        for (auto & pdep : deplist_p) {
            auto b = depends_on.emplace(depends_on.end(), this, pdep.to, pdep.dep_type);
            pdep.to->dependents.push_back(&(*b));
        }
    }
    
    service_record(service_set *set, string name, service_type record_type_p, string &&command,
            std::list<std::pair<unsigned,unsigned>> &command_offsets,
            const std::list<prelim_dep> &deplist_p)
        : service_record(set, name, record_type_p, deplist_p)
    {
        program_name = std::move(command);
        exec_arg_parts = separate_args(program_name, command_offsets);
    }
    
    virtual ~service_record() noexcept
    {
    }
    
    // begin transition from stopped to started state or vice versa depending on current and desired state
    void execute_transition() noexcept;
    
    void do_propagation() noexcept;
    
    // Called on transition of desired state from stopped to started (or unpinned stop)
    void do_start() noexcept;

    // Called on transition of desired state from started to stopped (or unpinned start)
    void do_stop() noexcept;
    
    // Console is available.
    void acquired_console() noexcept;
    
    // Set the stop command and arguments (may throw std::bad_alloc)
    void set_stop_command(std::string command, std::list<std::pair<unsigned,unsigned>> &stop_command_offsets)
    {
        stop_command = command;
        stop_arg_parts = separate_args(stop_command, stop_command_offsets);
    }
    
    // Get the target (aka desired) state.
    service_state_t get_target_state() noexcept
    {
        return desired_state;
    }

    // Set logfile, should be done before service is started
    void set_log_file(string logfile)
    {
        this->logfile = logfile;
    }
    
    // Set whether this service should automatically restart when it dies
    void set_auto_restart(bool auto_restart) noexcept
    {
        this->auto_restart = auto_restart;
    }
    
    void set_smooth_recovery(bool smooth_recovery) noexcept
    {
        this->smooth_recovery = smooth_recovery;
    }
    
    // Set "on start" flags (commands)
    void set_flags(onstart_flags_t flags) noexcept
    {
        this->onstart_flags = flags;
    }
    
    // Set an additional signal (other than SIGTERM) to be used to terminate the process
    void set_extra_termination_signal(int signo) noexcept
    {
        this->term_signal = signo;
    }
    
    void set_pid_file(string &&pid_file) noexcept
    {
        this->pid_file = std::move(pid_file);
    }
    
    void set_socket_details(string &&socket_path, int socket_perms, uid_t socket_uid, uid_t socket_gid) noexcept
    {
        this->socket_path = std::move(socket_path);
        this->socket_perms = socket_perms;
        this->socket_uid = socket_uid;
        this->socket_gid = socket_gid;
    }

    const std::string &get_service_name() const noexcept { return service_name; }
    service_state_t get_state() const noexcept { return service_state; }
    
    void start(bool activate = true) noexcept;  // start the service
    void stop(bool bring_down = true) noexcept;   // stop the service
    
    void forced_stop() noexcept; // force-stop this service and all dependents
    
    // Pin the service in "started" state (when it reaches the state)
    void pin_start() noexcept
    {
        pinned_started = true;
    }
    
    // Pin the service in "stopped" state (when it reaches the state)
    void pin_stop() noexcept
    {
        pinned_stopped = true;
    }
    
    // Remove both "started" and "stopped" pins. If the service is currently pinned
    // in either state but would naturally be in the opposite state, it will immediately
    // commence starting/stopping.
    void unpin() noexcept;
    
    bool isDummy() noexcept
    {
        return record_type == service_type::DUMMY;
    }
    
    // Add a listener. A listener must only be added once. May throw std::bad_alloc.
    void addListener(service_listener * listener)
    {
        listeners.insert(listener);
    }
    
    // Remove a listener.    
    void removeListener(service_listener * listener) noexcept
    {
        listeners.erase(listener);
    }
};

class base_process_service;

// A timer for process restarting. Used to ensure a minimum delay between process restarts (and
// also for timing service stop before the SIGKILL hammer is used).
class process_restart_timer : public eventloop_t::timer_impl<process_restart_timer>
{
    public:
    base_process_service * service;

    process_restart_timer()
    {
    }

    rearm timer_expiry(eventloop_t &, int expiry_count);
};

class base_process_service : public service_record
{
    friend class service_child_watcher;
    friend class exec_status_pipe_watcher;
    friend class process_restart_timer;

    private:
    // Re-launch process
    void do_restart() noexcept;

    protected:
    service_child_watcher child_listener;
    exec_status_pipe_watcher child_status_listener;
    process_restart_timer restart_timer;
    time_val last_start_time;

    // Restart interval time and restart count are used to track the number of automatic restarts
    // over an interval. Too many restarts over an interval will inhibit further restarts.
    time_val restart_interval_time;
    int restart_interval_count;

    time_val restart_interval;
    int max_restart_interval_count;
    time_val restart_delay;

    // Time allowed for service stop, after which SIGKILL is sent. 0 to disable.
    time_val stop_timeout = {10, 0}; // default of 10 seconds

    bool waiting_restart_timer : 1;
    bool stop_timer_armed : 1;
    bool reserved_child_watch : 1;
    bool tracking_child : 1;

    // Start the process, return true on success
    virtual bool start_ps_process() noexcept override;
    bool start_ps_process(const std::vector<const char *> &args, bool on_console) noexcept;

    // Restart the process (due to start failure or unexpected termination). Restarts will be
    // rate-limited.
    bool restart_ps_process() noexcept;

    // Perform smooth recovery process
    void do_smooth_recovery() noexcept;

    virtual void all_deps_stopped() noexcept override;
    virtual void handle_exit_status(int exit_status) noexcept = 0;

    virtual bool can_interrupt_start() noexcept override
    {
        return waiting_restart_timer || service_record::can_interrupt_start();
    }

    virtual void interrupt_start() noexcept override;

    // Kill with SIGKILL
    void kill_with_fire() noexcept;

    // Signal the process group of the service process
    void kill_pg(int signo) noexcept;

    public:
    base_process_service(service_set *sset, string name, service_type record_type_p, string &&command,
            std::list<std::pair<unsigned,unsigned>> &command_offsets,
            const std::list<prelim_dep> &deplist_p);

    ~base_process_service() noexcept
    {
    }

    void set_restart_interval(timespec interval, int max_restarts) noexcept
    {
        restart_interval = interval;
        max_restart_interval_count = max_restarts;
    }

    void set_restart_delay(timespec delay) noexcept
    {
        restart_delay = delay;
    }

    void set_stop_timeout(timespec timeout) noexcept
    {
        stop_timeout = timeout;
    }
};

class process_service : public base_process_service
{
    // called when the process exits. The exit_status is the status value yielded by
    // the "wait" system call.
    virtual void handle_exit_status(int exit_status) noexcept override;

    public:
    process_service(service_set *sset, string name, string &&command,
            std::list<std::pair<unsigned,unsigned>> &command_offsets,
            std::list<prelim_dep> depends_p)
         : base_process_service(sset, name, service_type::PROCESS, std::move(command), command_offsets,
             depends_p)
    {
    }

    ~process_service() noexcept
    {
    }
};

class bgproc_service : public base_process_service
{
    virtual void handle_exit_status(int exit_status) noexcept override;

    enum class pid_result_t {
        OK,
        FAILED,      // failed to read pid or read invalid pid
        TERMINATED   // read pid successfully, but the process already terminated
    };

    // Read the pid-file, return false on failure
    pid_result_t read_pid_file(int *exit_status) noexcept;

    public:
    bgproc_service(service_set *sset, string name, string &&command,
            std::list<std::pair<unsigned,unsigned>> &command_offsets,
            std::list<prelim_dep> depends_p)
         : base_process_service(sset, name, service_type::BGPROCESS, std::move(command), command_offsets,
             depends_p)
    {
    }

    ~bgproc_service() noexcept
    {
    }
};

class scripted_service : public base_process_service
{
    virtual void all_deps_stopped() noexcept override;
    virtual void handle_exit_status(int exit_status) noexcept override;

    public:
    scripted_service(service_set *sset, string name, string &&command,
            std::list<std::pair<unsigned,unsigned>> &command_offsets,
            std::list<prelim_dep> depends_p)
         : base_process_service(sset, name, service_type::SCRIPTED, std::move(command), command_offsets,
             depends_p)
    {
    }

    ~scripted_service() noexcept
    {
    }
};

inline auto extract_prop_queue(service_record *sr) -> decltype(sr->prop_queue_node) &
{
    return sr->prop_queue_node;
}

inline auto extract_stop_queue(service_record *sr) -> decltype(sr->stop_queue_node) &
{
    return sr->stop_queue_node;
}

inline auto extract_console_queue(service_record *sr) -> decltype(sr->console_queue_node) &
{
    return sr->console_queue_node;
}

/*
 * A service_set, as the name suggests, manages a set of services.
 *
 * Other than the ability to find services by name, the service set manages various queues.
 * One is the queue for processes wishing to acquire the console. There is also a set of
 * processes that want to start, and another set of those that want to stop. These latter
 * two "queues" (not really queues since their order is not important) are used to prevent too
 * much recursion and to prevent service states from "bouncing" too rapidly.
 * 
 * A service that wishes to start or stop puts itself on the start/stop queue; a service that
 * needs to propagate changes to dependent services or dependencies puts itself on the
 * propagation queue. Any operation that potentially manipulates the queues must be followed
 * by a "process queues" order (processQueues() method).
 *
 * Note that processQueues always repeatedly processes both queues until they are empty. The
 * process is finite because starting a service can never cause services to stop, unless they
 * fail to start, which should cause them to stop semi-permanently.
 */
class service_set
{
    protected:
    int active_services;
    std::list<service_record *> records;
    bool restart_enabled; // whether automatic restart is enabled (allowed)
    
    shutdown_type_t shutdown_type = shutdown_type_t::CONTINUE;  // Shutdown type, if stopping
    
    // Services waiting for exclusive access to the console
    dlist<service_record, extract_console_queue> console_queue;

    // Propagation and start/stop "queues" - list of services waiting for processing
    slist<service_record, extract_prop_queue> prop_queue;
    slist<service_record, extract_stop_queue> stop_queue;
    
    public:
    service_set()
    {
        active_services = 0;
        restart_enabled = true;
    }
    
    virtual ~service_set()
    {
        for (auto * s : records) {
            delete s;
        }
    }

    // Start the specified service. The service will be marked active.
    void start_service(service_record *svc)
    {
        svc->start();
        process_queues();
    }

    // Stop the specified service. Its active mark will be cleared.
    void stop_service(service_record *svc)
    {
        svc->stop(true);
        process_queues();
    }

    // Locate an existing service record.
    service_record *find_service(const std::string &name) noexcept;

    // Load a service description, and dependencies, if there is no existing
    // record for the given name.
    // Throws:
    //   ServiceLoadException (or subclass) on problem with service description
    //   std::bad_alloc on out-of-memory condition
    virtual service_record *load_service(const char *name)
    {
        auto r = find_service(name);
        if (r == nullptr) {
            throw service_not_found(name);
        }
        return r;
    }

    // Start the service with the given name. The named service will begin
    // transition to the 'started' state.
    //
    // Throws a ServiceLoadException (or subclass) if the service description
    // cannot be loaded or is invalid;
    // Throws std::bad_alloc if out of memory.
    void start_service(const char *name)
    {
        using namespace std;
        service_record *record = load_service(name);
        service_set::start_service(record);
    }
    
    void add_service(service_record *svc)
    {
        records.push_back(svc);
    }
    
    void remove_service(service_record *svc)
    {
        std::remove(records.begin(), records.end(), svc);
    }

    // Get the list of all loaded services.
    const std::list<service_record *> &listServices()
    {
        return records;
    }
    
    // Stop the service with the given name. The named service will begin
    // transition to the 'stopped' state.
    void stop_service(const std::string &name) noexcept;
    
    // Add a service record to the state propagation queue. The service record will have its
    // do_propagation() method called when the queue is processed.
    void add_prop_queue(service_record *service) noexcept
    {
        if (! prop_queue.is_queued(service)) {
            prop_queue.insert(service);
        }
    }
    
    // Add a service record to the stop queue. The service record will have its
    // execute_transition() method called when the queue is processed.
    void add_transition_queue(service_record *service) noexcept
    {
        if (! stop_queue.is_queued(service)) {
            stop_queue.insert(service);
        }
    }
    
    // Process state propagation and start/stop queues, until they are empty.
    void process_queues() noexcept
    {
        while (! stop_queue.is_empty() || ! prop_queue.is_empty()) {
            while (! prop_queue.is_empty()) {
                auto next = prop_queue.pop_front();
                next->do_propagation();
            }
            while (! stop_queue.is_empty()) {
                auto next = stop_queue.pop_front();
                next->execute_transition();
            }
        }
    }
    
    // Set the console queue tail (returns previous tail)
    void append_console_queue(service_record * newTail) noexcept
    {
        bool was_empty = console_queue.is_empty();
        console_queue.append(newTail);
        if (was_empty) {
            enable_console_log(false);
        }
    }
    
    // Pull and dispatch a waiter from the console queue
    void pull_console_queue() noexcept
    {
        if (console_queue.is_empty()) {
            enable_console_log(true);
        }
        else {
            service_record * front = console_queue.pop_front();
            front->acquired_console();
        }
    }
    
    void unqueue_console(service_record * service) noexcept
    {
        if (console_queue.is_queued(service)) {
            console_queue.unlink(service);
        }
    }

    // Notification from service that it is active (state != STOPPED)
    // Only to be called on the transition from inactive to active.
    void service_active(service_record *) noexcept;
    
    // Notification from service that it is inactive (STOPPED)
    // Only to be called on the transition from active to inactive.
    void service_inactive(service_record *) noexcept;
    
    // Find out how many services are active (starting, running or stopping,
    // but not stopped).
    int count_active_services() noexcept
    {
        return active_services;
    }
    
    void stop_all_services(shutdown_type_t type = shutdown_type_t::HALT) noexcept
    {
        restart_enabled = false;
        shutdown_type = type;
        for (std::list<service_record *>::iterator i = records.begin(); i != records.end(); ++i) {
            (*i)->stop(false);
            (*i)->unpin();
        }
        process_queues();
    }
    
    void set_auto_restart(bool restart) noexcept
    {
        restart_enabled = restart;
    }
    
    bool get_auto_restart() noexcept
    {
        return restart_enabled;
    }
    
    shutdown_type_t getShutdownType() noexcept
    {
        return shutdown_type;
    }
};

class dirload_service_set : public service_set
{
    const char *service_dir;  // directory containing service descriptions

    public:
    dirload_service_set(const char *service_dir_p) : service_set(), service_dir(service_dir_p)
    {
    }

    service_record *load_service(const char *name) override;
};

#endif
