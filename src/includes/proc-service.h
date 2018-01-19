#include "service.h"

// Given a string and a list of pairs of (start,end) indices for each argument in that string,
// store a null terminator for the argument. Return a `char *` vector containing the beginning
// of each argument and a trailing nullptr. (The returned array is invalidated if the string is later modified).
std::vector<const char *> separate_args(std::string &s, std::list<std::pair<unsigned,unsigned>> &arg_indices);

class base_process_service;

// A timer for process restarting. Used to ensure a minimum delay between process restarts (and
// also for timing service stop before the SIGKILL hammer is used).
class process_restart_timer : public eventloop_t::timer_impl<process_restart_timer>
{
    public:
    base_process_service * service;

    process_restart_timer(base_process_service *service_p)
        : service(service_p)
    {
    }

    dasynq::rearm timer_expiry(eventloop_t &, int expiry_count);
};

class base_process_service : public service_record
{
    friend class service_child_watcher;
    friend class exec_status_pipe_watcher;
    friend class base_process_service_test;

    private:
    // Re-launch process
    void do_restart() noexcept;

    protected:
    string program_name;          // storage for program/script and arguments
    std::vector<const char *> exec_arg_parts; // pointer to each argument/part of the program_name, and nullptr

    string stop_command;          // storage for stop program/script and arguments
    std::vector<const char *> stop_arg_parts; // pointer to each argument/part of the stop_command, and nullptr

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

    pid_t pid = -1;  // PID of the process. If state is STARTING or STOPPING,
                     //   this is PID of the service script; otherwise it is the
                     //   PID of the process itself (process service).
    int exit_status = 0; // Exit status, if the process has exited (pid == -1).
    int socket_fd = -1;  // For socket-activation services, this is the file
                         // descriptor for the socket.

    bool waiting_restart_timer : 1;
    bool stop_timer_armed : 1;
    bool reserved_child_watch : 1;
    bool tracking_child : 1;  // whether we expect to see child process status
    bool start_is_interruptible : 1;  // whether we can interrupt start

    // Launch the process with the given arguments, return true on success
    bool start_ps_process(const std::vector<const char *> &args, bool on_console) noexcept;

    // Restart the process (due to start failure or unexpected termination). Restarts will be
    // rate-limited.
    bool restart_ps_process() noexcept;

    // Perform smooth recovery process
    void do_smooth_recovery() noexcept;

    // Start the process, return true on success
    virtual bool bring_up() noexcept override;

    virtual void bring_down() noexcept override;

    // Called when the process exits. The exit_status is the status value yielded by
    // the "wait" system call.
    virtual void handle_exit_status(int exit_status) noexcept = 0;

    // Called if an exec fails.
    virtual void exec_failed(int errcode) noexcept = 0;

    // Called if exec succeeds.
    virtual void exec_succeeded() noexcept { };

    virtual bool can_interrupt_start() noexcept override
    {
        return waiting_restart_timer || start_is_interruptible || service_record::can_interrupt_start();
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

    public:
    // Constructor for a base_process_service. Note that the various parameters not specified here must in
    // general be set separately (using the appropriate set_xxx function for each).
    base_process_service(service_set *sset, string name, service_type_t record_type_p, string &&command,
            std::list<std::pair<unsigned,unsigned>> &command_offsets,
            const std::list<prelim_dep> &deplist_p);

    ~base_process_service() noexcept
    {
    }

    // Set the stop command and arguments (may throw std::bad_alloc)
    void set_stop_command(std::string command, std::list<std::pair<unsigned,unsigned>> &stop_command_offsets)
    {
        stop_command = command;
        stop_arg_parts = separate_args(stop_command, stop_command_offsets);
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

    void set_start_interruptible(bool value) noexcept
    {
        start_is_interruptible = value;
    }

    // Set an additional signal (other than SIGTERM) to be used to terminate the process
    void set_extra_termination_signal(int signo) noexcept
    {
        this->term_signal = signo;
    }

    // The restart/stop timer expired.
    void timer_expired() noexcept;
};

class process_service : public base_process_service
{
    virtual void handle_exit_status(int exit_status) noexcept override;
    virtual void exec_failed(int errcode) noexcept override;
    virtual void exec_succeeded() noexcept override;
    virtual void bring_down() noexcept override;

    public:
    process_service(service_set *sset, string name, string &&command,
            std::list<std::pair<unsigned,unsigned>> &command_offsets,
            std::list<prelim_dep> depends_p)
         : base_process_service(sset, name, service_type_t::PROCESS, std::move(command), command_offsets,
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
    virtual void exec_failed(int errcode) noexcept override;

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
         : base_process_service(sset, name, service_type_t::BGPROCESS, std::move(command), command_offsets,
             depends_p)
    {
    }

    ~bgproc_service() noexcept
    {
    }
};

class scripted_service : public base_process_service
{
    virtual void handle_exit_status(int exit_status) noexcept override;
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
    scripted_service(service_set *sset, string name, string &&command,
            std::list<std::pair<unsigned,unsigned>> &command_offsets,
            std::list<prelim_dep> depends_p)
         : base_process_service(sset, name, service_type_t::SCRIPTED, std::move(command), command_offsets,
             depends_p), interrupting_start(false)
    {
    }

    ~scripted_service() noexcept
    {
    }
};
