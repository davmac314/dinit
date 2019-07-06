#include <sys/types.h>
#include <sys/resource.h>

#include "baseproc-sys.h"
#include "service.h"
#include "dinit-utmp.h"

// This header defines base_proc_service (base process service) and several derivatives, as well as some
// utility functions and classes. See service.h for full details of services.

// Given a string and a list of pairs of (start,end) indices for each argument in that string,
// store a null terminator for the argument. Return a `char *` vector containing the beginning
// of each argument and a trailing nullptr. (The returned array is invalidated if the string is later
// modified).
std::vector<const char *> separate_args(std::string &s,
        const std::list<std::pair<unsigned,unsigned>> &arg_indices);

struct service_rlimits
{
    int resource_id; // RLIMIT_xxx identifying resource
    bool soft_set : 1;
    bool hard_set : 1;
    struct rlimit limits;

    service_rlimits(int id) : resource_id(id), soft_set(0), hard_set(0), limits({0,0}) { }
};

// Parameters for process execution
struct run_proc_params
{
    const char * const *args; // program arguments including executable (args[0])
    const char *working_dir;  // working directory
    const char *logfile;      // log file or nullptr (stdout/stderr); must be valid if !on_console
    const char *env_file;      // file with environment settings (or nullptr)
    bool on_console;          // whether to run on console
    int wpipefd;              // pipe to which error status will be sent (if error occurs)
    int csfd;                 // control socket fd (or -1); may be moved
    int socket_fd;            // pre-opened socket fd (or -1); may be moved
    int notify_fd;            // pipe for readiness notification message (or -1); may be moved
    int force_notify_fd;      // if not -1, notification fd must be moved to this fd
    const char *notify_var;   // environment variable name where notification fd will be stored, or nullptr
    uid_t uid;
    gid_t gid;
    const std::vector<service_rlimits> &rlimits;

    run_proc_params(const char * const *args, const char *working_dir, const char *logfile, int wpipefd,
            uid_t uid, gid_t gid, const std::vector<service_rlimits> &rlimits)
            : args(args), working_dir(working_dir), logfile(logfile), env_file(nullptr), on_console(false),
              wpipefd(wpipefd), csfd(-1), socket_fd(-1), notify_fd(-1), force_notify_fd(-1), notify_var(nullptr),
              uid(uid), gid(gid), rlimits(rlimits)
    { }
};

class base_process_service;

// A timer for process restarting. Used to ensure a minimum delay between process restarts (and
// also for timing service stop before the SIGKILL hammer is used).
class process_restart_timer : public eventloop_t::timer_impl<process_restart_timer>
{
    public:
    base_process_service * service;

    explicit process_restart_timer(base_process_service *service_p)
        : service(service_p)
    {
    }

    dasynq::rearm timer_expiry(eventloop_t &, int expiry_count);
};

// Watcher for the pipe used to receive exec() failure status errno
class exec_status_pipe_watcher : public eventloop_t::fd_watcher_impl<exec_status_pipe_watcher>
{
    public:
    base_process_service * service;
    dasynq::rearm fd_event(eventloop_t &eloop, int fd, int flags) noexcept;

    exec_status_pipe_watcher(base_process_service * sr) noexcept : service(sr) { }

    exec_status_pipe_watcher(const exec_status_pipe_watcher &) = delete;
    void operator=(const exec_status_pipe_watcher &) = delete;
};

// Watcher for readiness notification pipe
class ready_notify_watcher : public eventloop_t::fd_watcher_impl<ready_notify_watcher>
{
    public:
    base_process_service * service;
    dasynq::rearm fd_event(eventloop_t &eloop, int fd, int flags) noexcept;

    ready_notify_watcher(base_process_service * sr) noexcept : service(sr) { }

    ready_notify_watcher(const ready_notify_watcher &) = delete;
    void operator=(const ready_notify_watcher &) = delete;
};


class service_child_watcher : public eventloop_t::child_proc_watcher_impl<service_child_watcher>
{
    public:
    base_process_service * service;
    dasynq::rearm status_change(eventloop_t &eloop, pid_t child, int status) noexcept;

    service_child_watcher(base_process_service * sr) noexcept : service(sr) { }

    service_child_watcher(const service_child_watcher &) = delete;
    void operator=(const service_child_watcher &) = delete;
};

// Base class for process-based services.
class base_process_service : public service_record
{
    friend class service_child_watcher;
    friend class exec_status_pipe_watcher;
    friend class base_process_service_test;
    friend class ready_notify_watcher;

    private:
    // Re-launch process
    void do_restart() noexcept;

    protected:
    string program_name;          // storage for program/script and arguments
    // pointer to each argument/part of the program_name, and nullptr:
    std::vector<const char *> exec_arg_parts;

    string stop_command;          // storage for stop program/script and arguments
    // pointer to each argument/part of the stop_command, and nullptr:
    std::vector<const char *> stop_arg_parts;

    string working_dir;       // working directory (or empty)
    string env_file;          // file with environment settings for this service

    std::vector<service_rlimits> rlimits; // resource limits

    service_child_watcher child_listener;
    exec_status_pipe_watcher child_status_listener;
    process_restart_timer restart_timer;
    time_val last_start_time;

    // Restart interval time and restart count are used to track the number of automatic restarts
    // over an interval. Too many restarts over an interval will inhibit further restarts.
    time_val restart_interval_time;  // current restart interval
    int restart_interval_count;      // count of restarts within current interval

    time_val restart_interval;       // maximum restart interval
    int max_restart_interval_count;  // number of restarts allowed over maximum interval
    time_val restart_delay;          // delay between restarts

    // Time allowed for service stop, after which SIGKILL is sent. 0 to disable.
    time_val stop_timeout = {10, 0}; // default of 10 seconds

    // Time allowed for service start, after which SIGINT is sent (and then SIGKILL after
    // <stop_timeout>). 0 to disable.
    time_val start_timeout = {60, 0}; // default of 1 minute

    uid_t run_as_uid = -1;
    gid_t run_as_gid = -1;
    int force_notification_fd = -1;  // if set, notification fd for service process is set to this fd
    string notification_var; // if set, name of an environment variable for notification fd

    pid_t pid = -1;  // PID of the process. If state is STARTING or STOPPING,
                     //   this is PID of the service script; otherwise it is the
                     //   PID of the process itself (process service).
    bp_sys::exit_status exit_status; // Exit status, if the process has exited (pid == -1).
    int socket_fd = -1;  // For socket-activation services, this is the file
                         // descriptor for the socket.
    int notification_fd = -1;  // If readiness notification is via fd

    bool waiting_restart_timer : 1;
    bool stop_timer_armed : 1;
    bool reserved_child_watch : 1;
    bool tracking_child : 1;  // whether we expect to see child process status

    // Run a child process (call after forking). Note that some parameters specify file descriptors,
    // but in general file descriptors may be moved before the exec call.
    void run_child_proc(run_proc_params params) noexcept;

    // Launch the process with the given arguments, return true on success
    bool start_ps_process(const std::vector<const char *> &args, bool on_console) noexcept;

    // Restart the process (due to start failure or unexpected termination). Restarts will be
    // rate-limited.
    bool restart_ps_process() noexcept;

    // Perform smooth recovery process
    void do_smooth_recovery() noexcept;

    // Start the process, return true on success
    virtual bool bring_up() noexcept override;

    // Called after forking (before executing remote process).
    virtual void after_fork(pid_t child_pid) noexcept { }

    // Called when the process exits. The exit_status is the status value yielded by
    // the "wait" system call.
    virtual void handle_exit_status(bp_sys::exit_status exit_status) noexcept = 0;

    // Called if an exec fails.
    virtual void exec_failed(int errcode) noexcept = 0;

    // Called if exec succeeds.
    virtual void exec_succeeded() noexcept { };

    virtual bool can_interrupt_start() noexcept override
    {
        return waiting_restart_timer || onstart_flags.start_interruptible
                || service_record::can_interrupt_start();
    }

    virtual bool can_proceed_to_start() noexcept override
    {
        return ! waiting_restart_timer;
    }

    virtual bool interrupt_start() noexcept override;

    void becoming_inactive() noexcept override;

    // Kill with SIGKILL
    void kill_with_fire() noexcept;

    // Signal the process group of the service process
    void kill_pg(int signo) noexcept;

    // stop immediately
    void emergency_stop() noexcept;

    // Open the activation socket, return false on failure
    bool open_socket() noexcept;

    // Get the readiness notification watcher for this service, if it has one; may return nullptr.
    virtual ready_notify_watcher *get_ready_watcher() noexcept
    {
        return nullptr;
    }

    public:
    // Constructor for a base_process_service. Note that the various parameters not specified here must in
    // general be set separately (using the appropriate set_xxx function for each).
    base_process_service(service_set *sset, string name, service_type_t record_type_p, string &&command,
            const std::list<std::pair<unsigned,unsigned>> &command_offsets,
            const std::list<prelim_dep> &deplist_p);

    ~base_process_service() noexcept
    {
        if (reserved_child_watch) {
            child_listener.unreserve(event_loop);
        }
        restart_timer.deregister(event_loop);
    }

    // Set the stop command and arguments (may throw std::bad_alloc)
    void set_stop_command(const std::string &command,
            std::list<std::pair<unsigned,unsigned>> &stop_command_offsets)
    {
        stop_command = command;
        stop_arg_parts = separate_args(stop_command, stop_command_offsets);
    }

    void set_env_file(const std::string &env_file_p)
    {
        env_file = env_file_p;
    }

    void set_rlimits(std::vector<service_rlimits> &&rlimits_p)
    {
        rlimits = std::move(rlimits_p);
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

    void set_start_timeout(timespec timeout) noexcept
    {
        start_timeout = timeout;
    }

    // Set an additional signal (other than SIGTERM) to be used to terminate the process
    void set_extra_termination_signal(int signo) noexcept
    {
        this->term_signal = signo;
    }

    // Set the uid/gid that the service process will be run as
    void set_run_as_uid_gid(uid_t uid, gid_t gid) noexcept
    {
        run_as_uid = uid;
        run_as_gid = gid;
    }

    // Set the working directory
    void set_working_dir(const string &working_dir_p)
    {
        working_dir = working_dir_p;
    }

    // Set the notification fd number that the service process will use
    void set_notification_fd(int fd)
    {
        force_notification_fd = fd;
    }

    // Set the name of the environment variable that will be set to the notification fd number
    // when the service process is run
    void set_notification_var(string &&varname)
    {
        notification_var = std::move(varname);
    }

    // The restart/stop timer expired.
    void timer_expired() noexcept;

    // Accessor for testing:
    const std::vector<const char *> & get_exec_arg_parts() noexcept
    {
        return exec_arg_parts;
    }

    pid_t get_pid() override
    {
        return pid;
    }

    int get_exit_status() override
    {
        return exit_status.as_int();
    }
};

// Standard process service.
class process_service : public base_process_service
{
    virtual void handle_exit_status(bp_sys::exit_status exit_status) noexcept override;
    virtual void exec_failed(int errcode) noexcept override;
    virtual void exec_succeeded() noexcept override;
    virtual void bring_down() noexcept override;

    ready_notify_watcher readiness_watcher;

#if USE_UTMPX

    char inittab_id[sizeof(utmpx().ut_id)];
    char inittab_line[sizeof(utmpx().ut_line)];

    protected:
    void after_fork(pid_t child_pid) noexcept override
    {
        if (*inittab_id || *inittab_line) {
            create_utmp_entry(inittab_id, inittab_line, child_pid);
        }
    }

#endif

    protected:
    ready_notify_watcher *get_ready_watcher() noexcept override
    {
        return &readiness_watcher;
    }

    public:
    process_service(service_set *sset, const string &name, string &&command,
            std::list<std::pair<unsigned,unsigned>> &command_offsets,
            const std::list<prelim_dep> &depends_p)
         : base_process_service(sset, name, service_type_t::PROCESS, std::move(command), command_offsets,
             depends_p), readiness_watcher(this)
    {
    }

#if USE_UTMPX

    // Set the id of the process in utmp (the "inittab" id)
    void set_utmp_id(const char *id)
    {
        strncpy(inittab_id, id, sizeof(inittab_id));
    }

    // Set the device line of the process in utmp database
    void set_utmp_line(const char *line)
    {
        strncpy(inittab_line, line, sizeof(inittab_line));
    }

#endif

    ~process_service() noexcept
    {
    }
};

// Bgproc (self-"backgrounding", i.e. double-forking) process service
class bgproc_service : public base_process_service
{
    virtual void handle_exit_status(bp_sys::exit_status exit_status) noexcept override;
    virtual void exec_failed(int errcode) noexcept override;
    virtual void bring_down() noexcept override;

    enum class pid_result_t {
        OK,
        FAILED,      // failed to read pid or read invalid pid
        TERMINATED   // read pid successfully, but the process already terminated
    };

    // Read the pid-file, return false on failure
    pid_result_t read_pid_file(bp_sys::exit_status *exit_status) noexcept;

    public:
    bgproc_service(service_set *sset, const string &name, string &&command,
            std::list<std::pair<unsigned,unsigned>> &command_offsets,
            const std::list<prelim_dep> &depends_p)
         : base_process_service(sset, name, service_type_t::BGPROCESS, std::move(command), command_offsets,
             depends_p)
    {
    }

    ~bgproc_service() noexcept
    {
    }
};

// Service which is started and stopped via separate commands
class scripted_service : public base_process_service
{
    virtual void handle_exit_status(bp_sys::exit_status exit_status) noexcept override;
    virtual void exec_succeeded() noexcept override;
    virtual void exec_failed(int errcode) noexcept override;
    virtual void bring_down() noexcept override;

    virtual bool interrupt_start() noexcept override
    {
        // if base::interrupt_start() returns false, then start hasn't been fully interrupted, but an
        // interrupt has been issued:
        interrupting_start = ! base_process_service::interrupt_start();
        return ! interrupting_start;
    }

    bool interrupting_start : 1;  // running start script (true) or stop script (false)

    public:
    scripted_service(service_set *sset, const string &name, string &&command,
            std::list<std::pair<unsigned,unsigned>> &command_offsets,
            const std::list<prelim_dep> &depends_p)
         : base_process_service(sset, name, service_type_t::SCRIPTED, std::move(command), command_offsets,
             depends_p), interrupting_start(false)
    {
    }

    ~scripted_service() noexcept
    {
    }
};
