#include <vector>
#include <string>
#include <list>

#include <sys/types.h>
#include <sys/resource.h>

#if SUPPORT_CAPABILITIES
#include <sys/capability.h>
#endif

#include <baseproc-sys.h>
#include <service.h>
#include <dinit-utmp.h>

// This header defines base_proc_service (base process service) and several derivatives, as well as some
// utility functions and classes. See service.h for full details of services.

class process_service;

// Given a string and a list of pairs of (start,end) indices for each argument in that string,
// store a null terminator for the argument. Return a `char *` vector containing the beginning
// of each argument and a trailing nullptr. (The returned array is invalidated if the string is later
// modified).
std::vector<const char *> separate_args(ha_string &s,
        const std::list<std::pair<unsigned,unsigned>> &arg_indices);

// Parameters for process execution
struct run_proc_params
{
    const char * const *args; // program arguments including executable (args[0])
    const char *working_dir;  // working directory
    const char *logfile;      // log file or nullptr (stdout/stderr); must be valid if !on_console
    const char *env_file;     // file with environment settings (or nullptr)
    #if SUPPORT_CGROUPS
    const char *run_in_cgroup = nullptr; //  cgroup path
    #endif
    #if SUPPORT_CAPABILITIES
    cap_iab_t cap_iab = nullptr;
    unsigned int secbits = 0;
    bool no_new_privs = false;
    #endif
    bool on_console;          // whether to run on console
    bool in_foreground;       // if on console: whether to run in foreground
    bool unmask_sigint = false; // if in foreground: whether to unmask SIGINT
    int nice = INT_MIN;
    int ionice = INT_MIN;
    int oom_adj = INT_MIN;
    int wpipefd;              // pipe to which error status will be sent (if error occurs)
    int csfd;                 // control socket fd (or -1); may be moved
    int socket_fd;            // pre-opened socket fd (or -1); may be moved
    int notify_fd;            // pipe for readiness notification message (or -1); may be moved
    int force_notify_fd;      // if not -1, notification fd must be moved to this fd
    int output_fd;            // if not -1, output will be directed here (rather than logfile)
    const char *notify_var;   // environment variable name where notification fd will be stored, or nullptr
    uid_t uid;
    gid_t gid;
    const std::vector<service_rlimits> &rlimits;
    int input_fd = -1;        // file descriptor to be used for input (STDIN)

    run_proc_params(const char * const *args, const char *working_dir, const char *logfile, int wpipefd,
            uid_t uid, gid_t gid, const std::vector<service_rlimits> &rlimits)
            : args(args), working_dir(working_dir), logfile(logfile), env_file(nullptr), on_console(false),
              in_foreground(false), wpipefd(wpipefd), csfd(-1), socket_fd(-1), notify_fd(-1),
              force_notify_fd(-1), output_fd(-1), notify_var(nullptr), uid(uid), gid(gid), rlimits(rlimits)
    { }
};

// Error information from process execution transferred via this struct
struct run_proc_err
{
    exec_stage stage;
    int st_errno;
};

class base_process_service;

// A timer for process restarting. Used to ensure a minimum delay between process restarts (and
// also for timing service stop before the SIGKILL hammer is used).
class process_restart_timer : public eventloop_t::timer_impl<process_restart_timer>
{
    public:
    base_process_service *service;

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
    base_process_service *service;
    dasynq::rearm fd_event(eventloop_t &eloop, int fd, int flags) noexcept;

    exec_status_pipe_watcher(base_process_service * sr) noexcept : service(sr) { }

    exec_status_pipe_watcher(const exec_status_pipe_watcher &) = delete;
    void operator=(const exec_status_pipe_watcher &) = delete;
};

// Like exec_status_pipe_watcher, but for watching status when exec'ing the stop command
class stop_status_pipe_watcher : public eventloop_t::fd_watcher_impl<stop_status_pipe_watcher>
{
    public:
    process_service *service;
    dasynq::rearm fd_event(eventloop_t &eloop, int fd, int flags) noexcept;

    stop_status_pipe_watcher(process_service * sr) noexcept : service(sr) { }

    stop_status_pipe_watcher(const exec_status_pipe_watcher &) = delete;
    void operator=(const exec_status_pipe_watcher &) = delete;
};

// Watcher for readiness notification pipe
class ready_notify_watcher : public eventloop_t::fd_watcher_impl<ready_notify_watcher>
{
    public:
    base_process_service *service;
    dasynq::rearm fd_event(eventloop_t &eloop, int fd, int flags) noexcept;

    ready_notify_watcher(base_process_service * sr) noexcept : service(sr) { }

    ready_notify_watcher(const ready_notify_watcher &) = delete;
    void operator=(const ready_notify_watcher &) = delete;
};

// watcher for main child process
class service_child_watcher : public eventloop_t::child_proc_watcher_impl<service_child_watcher>
{
    public:
    base_process_service *service;
    dasynq::rearm status_change(eventloop_t &eloop, pid_t child, proc_status_t status) noexcept;

    service_child_watcher(base_process_service * sr) noexcept : service(sr) { }

    service_child_watcher(const service_child_watcher &) = delete;
    void operator=(const service_child_watcher &) = delete;
};

// watcher for the "stop-command" for process services
class stop_child_watcher : public eventloop_t::child_proc_watcher_impl<stop_child_watcher>
{
    public:
    process_service *service;
    dasynq::rearm status_change(eventloop_t &eloop, pid_t child, proc_status_t status) noexcept;

    stop_child_watcher(process_service * sr) noexcept : service(sr) { }

    stop_child_watcher(const service_child_watcher &) = delete;
    void operator=(const service_child_watcher &) = delete;
};

class log_output_watcher : public eventloop_t::fd_watcher_impl<log_output_watcher>
{
    public:
    base_process_service *service;

    dasynq::rearm fd_event(eventloop_t &eloop, int fd, int flags) noexcept;

    log_output_watcher(base_process_service * sr) noexcept : service(sr) { }

    log_output_watcher(const ready_notify_watcher &) = delete;
    void operator=(const ready_notify_watcher &) = delete;
};

// Base class for process-based services.
class base_process_service : public service_record
{
    friend class service_child_watcher;
    friend class exec_status_pipe_watcher;
    friend class base_process_service_test;
    friend class ready_notify_watcher;
    friend class log_output_watcher;

    protected:
    ha_string program_name;          // storage for program/script and arguments
    // pointer to each argument/part of the program_name, and nullptr:
    std::vector<const char *> exec_arg_parts;

    ha_string stop_command;          // storage for stop program/script and arguments
    // pointer to each argument/part of the stop_command, and nullptr:
    std::vector<const char *> stop_arg_parts;

    string working_dir;       // working directory (or empty)
    string env_file;          // file with environment settings for this service

    log_type_id log_type = log_type_id::NONE;
    string logfile;          // log file name, empty string specifies /dev/null
    int logfile_perms = 0;   // logfile permissions("mode")
    uid_t logfile_uid = -1;  // logfile owner user id
    gid_t logfile_gid = -1;  // logfile group id
    unsigned log_buf_max = 0; // log buffer maximum size
    unsigned log_buf_size = 0; // log buffer current size
    std::vector<char, default_init_allocator<char>> log_buffer;

    std::vector<service_rlimits> rlimits; // resource limits

#if SUPPORT_CAPABILITIES
    cap_iab_t cap_iab = nullptr;
    unsigned int secbits = 0;
    bool no_new_privs = false;
#endif

#ifdef __linux__
    int nice = INT_MIN;
    int ionice = INT_MIN;
    int oom_adj = INT_MIN;
#endif

#if SUPPORT_CGROUPS
    string run_in_cgroup;
#endif

    service_child_watcher child_listener;
    exec_status_pipe_watcher child_status_listener;
    process_restart_timer process_timer; // timer is used for start, stop and restart
    log_output_watcher log_output_listener;
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

    pid_t pid = -1;  // PID of the process. For a scripted service which is STARTING or STOPPING,
                     // this is PID of the service script; otherwise it is the PID of the process
                     // itself (process service).
    proc_status_t exit_status; // Exit status, if the process has exited (pid == -1).
    int socket_fd = -1;  // For socket-activation services, this is the file descriptor for the socket.
    int notification_fd = -1;  // If readiness notification is via fd
    int log_output_fd = -1; // If logging via buffer/pipe, write end of the pipe
    int log_input_fd = -1; // If logging via buffer/pipe, read end of the pipe

    // Only one of waiting_restart_timer and waiting_stopstart_timer should be set at any time.
    // They indicate that the process timer is armed (and why).
    bool waiting_restart_timer : 1;
    bool waiting_stopstart_timer : 1;

    bool reserved_child_watch : 1;
    bool tracking_child : 1;  // whether we expect to see child process status

    // If executing child process failed, information about the error
    run_proc_err exec_err_info;

    private:
    // Re-launch process
    void do_restart() noexcept;

    protected:
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

    // Called when the process exits. The exit_status variable must be set before calling.
    virtual void handle_exit_status() noexcept = 0;

    void handle_unexpected_termination() noexcept;

    // Called if an exec fails.
    virtual void exec_failed(run_proc_err errcode) noexcept = 0;

    // Called if exec succeeds.
    virtual void exec_succeeded() noexcept { }

    virtual bool can_interrupt_start() noexcept override
    {
        return waiting_restart_timer || onstart_flags.start_interruptible
                || service_record::can_interrupt_start();
    }

    virtual bool interrupt_start() noexcept override;

    void becoming_inactive() noexcept override;

    // Get the file descriptor which the process should read input from (STDIN).
    // Return false on failure. If input_fd returned is -1, process has no specific input.
    virtual bool get_input_fd(int *input_fd) noexcept { *input_fd = -1; return true; }

    // Kill with SIGKILL
    virtual void kill_with_fire() noexcept;

    // Signal the process group of the service process
    void kill_pg(int signo) noexcept;

    // Open the activation socket, return false on failure
    bool open_socket() noexcept;

    // Get the readiness notification watcher for this service, if it has one; may return nullptr.
    virtual ready_notify_watcher *get_ready_watcher() noexcept
    {
        return nullptr;
    }

    bool ensure_log_buffer_backing(unsigned size) noexcept;

    public:
    // Constructor for a base_process_service. Note that the various parameters not specified here must in
    // general be set separately (using the appropriate set_xxx function for each).
    base_process_service(service_set *sset, string name, service_type_t record_type_p, ha_string &&command,
            const std::list<std::pair<unsigned,unsigned>> &command_offsets,
            const std::list<prelim_dep> &deplist_p);

    ~base_process_service() noexcept
    {
        if (reserved_child_watch) {
            child_listener.unreserve(event_loop);
        }
        process_timer.deregister(event_loop);
        set_log_mode(log_type_id::NONE);
        #if SUPPORT_CAPABILITIES
        cap_free(cap_iab);
        #endif
    }

    // Set the command to run this service (executable and arguments, nul separated). The command_parts_p
    // vector must contain pointers to each part.
    void set_command(ha_string &&command_p, std::vector<const char *> &&command_parts_p) noexcept
    {
        program_name = std::move(command_p);
        exec_arg_parts = std::move(command_parts_p);
    }

    void get_command(ha_string &command_p, std::vector<const char *> &command_parts_p)
    {
        command_p = program_name;
        command_parts_p = exec_arg_parts;
    }

    // Set the stop command and arguments (may throw std::bad_alloc)
    void set_stop_command(ha_string &command,
            std::list<std::pair<unsigned,unsigned>> &stop_command_offsets)
    {
        stop_command = command;
        stop_arg_parts = separate_args(stop_command, stop_command_offsets);
    }

    // Set the stop command as a sequence of nul-terminated parts (arguments).
    //   command - the command and arguments, each terminated with nul ('\0')
    //   command_parts - pointers to the beginning of each command part
    void set_stop_command(ha_string &&command,
            std::vector<const char *> &&command_parts) noexcept
    {
        stop_command = std::move(command);
        stop_arg_parts = std::move(command_parts);
    }

    void set_logfile_details(string &&logfile, int logfile_perms, uid_t logfile_uid, gid_t logfile_gid)
            noexcept
    {
        this->logfile = std::move(logfile);
        this->logfile_perms = logfile_perms;
        this->logfile_uid = logfile_uid;
        this->logfile_gid = logfile_gid;
    }

    // Set log buffer maximum size (for if mode is BUFFER). Maximum allowed size is UINT_MAX / 2
    // (must be checked by caller).
    void set_log_buf_max(unsigned max_size) noexcept
    {
        this->log_buf_max = max_size;
    }

    // Set log mode (NONE, BUFFER, FILE, PIPE) (must not change mode when service is not STOPPED).
    void set_log_mode(log_type_id log_type) noexcept
    {
        if (this->log_type == log_type) {
            return;
        }
        if (log_output_fd != -1) {
            if (this->log_type == log_type_id::BUFFER) {
                log_output_listener.deregister(event_loop);
            }
            if (!value(log_type).is_in(log_type_id::BUFFER, log_type_id::PIPE)) {
                bp_sys::close(log_output_fd);
                bp_sys::close(log_input_fd);
                log_output_fd = log_input_fd = -1;
            }
        }
        this->log_type = log_type;
    }

    log_type_id get_log_mode() noexcept
    {
        return this->log_type;
    }

    // Set the output pipe descriptors (both read and write end). This is only valid to call for
    // log mode PIPE and only when the service has just been loaded. It is intended for transfer
    // of fds from a placeholder service.
    void set_output_pipe_fds(std::pair<int,int> fds)
    {
        log_input_fd = fds.first;
        log_output_fd = fds.second;
    }

    std::pair<int,int> transfer_output_pipe() noexcept override
    {
        std::pair<int,int> r { log_input_fd, log_output_fd };
        if (log_type == log_type_id::BUFFER && log_output_fd != -1) {
            log_output_listener.deregister(event_loop);
        }
        log_input_fd = log_output_fd = -1;
        return r;
    }

    int get_output_pipe_fd() noexcept override
    {
        if (log_input_fd != -1) {
            return log_input_fd;
        }

        int pipefds[2];
        if (bp_sys::pipe2(pipefds, O_CLOEXEC) == -1) {
            log(loglevel_t::ERROR, get_name(), ": Can't open output pipe: ", std::strerror(errno));
            return -1;
        }
        log_input_fd = pipefds[0];
        log_output_fd = pipefds[1];
        return log_input_fd;
    }

    // Get the log buffer (address, length)
    std::pair<const char *, unsigned> get_log_buffer() noexcept
    {
        return {log_buffer.data(), log_buf_size};
    }

    void clear_log_buffer() noexcept
    {
        log_buffer.clear();
        log_buffer.shrink_to_fit();
        log_buf_size = 0;
    }

    void set_env_file(const std::string &env_file_p)
    {
        env_file = env_file_p;
    }

    void set_env_file(std::string &&env_file_p) noexcept
    {
        env_file = std::move(env_file_p);
    }

    #if SUPPORT_CGROUPS
    void set_cgroup(std::string &&run_in_cgroup_p) noexcept
    {
        run_in_cgroup = std::move(run_in_cgroup_p);
    }
    #endif

    #if SUPPORT_CAPABILITIES
    void set_cap(cap_iab_t &iab, unsigned int sbits) noexcept
    {
        cap_iab = iab;
        secbits = sbits;
        iab = nullptr;
    }
    #endif

    #ifdef __linux__
    void set_nice(int nice_v) noexcept
    {
        nice = nice_v;
    }
    void set_ionice(int ionice_v) noexcept
    {
        ionice = ionice_v;
    }
    void set_oom_adj(int oom_adj_v) noexcept
    {
        oom_adj = oom_adj_v;
    }
    #endif

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
    // Note: constructing parameter may throw!
    void set_working_dir(string working_dir_p) noexcept
    {
        working_dir = std::move(working_dir_p);
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

    pid_t get_pid() noexcept override
    {
        return pid;
    }

    proc_status_t get_exit_status() noexcept override
    {
        return exit_status;
    }

    // Get reason for failure to exec process (if stop reason indicates exec failure)
    run_proc_err get_exec_err_info()
    {
        return exec_err_info;
    }
};

// Standard process service.
class process_service : public base_process_service
{
    friend class stop_child_watcher;
    friend class stop_status_pipe_watcher;
    friend class base_process_service_test;

    protected:
    virtual void handle_exit_status() noexcept override;
    virtual void exec_failed(run_proc_err errcode) noexcept override;
    virtual void exec_succeeded() noexcept override;
    virtual void bring_down() noexcept override;
    virtual void kill_with_fire() noexcept override;

    bool start_stop_process(const std::vector<const char *> &cmd) noexcept;

    bool reserved_stop_watch : 1;
    bool stop_issued : 1;

    pid_t stop_pid = -1;
    proc_status_t stop_status = {};

    ready_notify_watcher readiness_watcher;
    stop_child_watcher stop_watcher;
    stop_status_pipe_watcher stop_pipe_watcher;

    bool doing_smooth_recovery = false; // if we are performing smooth recovery

    service_record *consumer_for = nullptr;

#if USE_UTMPX

    private:
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

    void handle_stop_exit() noexcept
    {
        if (!stop_status.did_exit_clean()) {
            if (stop_status.did_exit()) {
                log(loglevel_t::ERROR, "Service ", get_name(), " stop command terminated with exit code ",
                        stop_status.get_exit_status());
            }
            else if (stop_status.was_signalled()) {
                log(loglevel_t::ERROR, "Service ", get_name(), " stop command terminated due to signal ",
                        stop_status.get_signal());
            }
        }

        if (pid == -1 || !tracking_child) {
            // If service process has already finished, we were just waiting for the stop command
            // process:
            stop_issued = false; // reset for next time
            stopped();
        }
    }

    virtual bool check_restart() noexcept override
    {
        if (max_restart_interval_count != 0) {
            // Check whether we're still in the most recent restart check interval:
            time_val current_time;
            event_loop.get_time(current_time, clock_type::MONOTONIC);
            time_val int_diff = current_time - restart_interval_time;
            if (int_diff < restart_interval) {
                // Within the restart limiting interval; check number of restarts
                if (restart_interval_count >= max_restart_interval_count) {
                    log(loglevel_t::ERROR, "Service ", get_name(), " restarting too quickly; stopping.");
                    set_target_state(service_state_t::STOPPED);
                    return false;
                }
                ++restart_interval_count;
            }
            else {
                // Not within the last limiting interval; start a new interval
                restart_interval_time = current_time;
                restart_interval_count = 1;
            }
        }

        return true;
    }

    bool get_input_fd(int *input_fd) noexcept override
    {
        if (consumer_for == nullptr) {
            *input_fd = -1;
            return true;
        }

        int cfd = consumer_for->get_output_pipe_fd();
        if (cfd == -1) return false;
        *input_fd = cfd;
        return true;
    }

    process_service(service_set *sset, const string &name, service_type_t s_type, ha_string &&command,
            std::list<std::pair<unsigned,unsigned>> &command_offsets,
            const std::list<prelim_dep> &depends_p)
         : base_process_service(sset, name, s_type, std::move(command), command_offsets,
             depends_p), reserved_stop_watch(false), stop_issued(false),
             readiness_watcher(this), stop_watcher(this), stop_pipe_watcher(this)
    {
    }

    public:
    process_service(service_set *sset, const string &name, ha_string &&command,
            std::list<std::pair<unsigned,unsigned>> &command_offsets,
            const std::list<prelim_dep> &depends_p)
         : base_process_service(sset, name, service_type_t::PROCESS, std::move(command), command_offsets,
             depends_p), reserved_stop_watch(false), stop_issued(false),
             readiness_watcher(this), stop_watcher(this), stop_pipe_watcher(this)
    {
    }

#if USE_UTMPX

    // Set the id of the process in utmp (the "inittab" id)
    void set_utmp_id(const char *id) noexcept
    {
        strncpy(inittab_id, id, sizeof(inittab_id));
    }

    // Set the device line of the process in utmp database
    void set_utmp_line(const char *line) noexcept
    {
        strncpy(inittab_line, line, sizeof(inittab_line));
    }

    // Get the utmp (inittab) id, may not be nul terminated if maximum length!
    const char *get_utmp_id() noexcept
    {
        return inittab_id;
    }

    // Get the utmp (inittab) line, may not be nul terminated if maximum length!
    const char *get_utmp_line() noexcept
    {
        return inittab_line;
    }

    constexpr size_t get_utmp_id_size() const noexcept { return sizeof(inittab_id); }
    constexpr size_t get_utmp_line_size() const noexcept { return sizeof(inittab_line); }

#endif

    // Set this service to consume output of specified service (only call while service stopped).
    void set_consumer_for(service_record *consumed) noexcept
    {
        consumer_for = consumed;
    }

    service_record *get_consumed() noexcept
    {
        return consumer_for;
    }

    ~process_service() noexcept
    {
        if (reserved_stop_watch) {
            stop_watcher.unreserve(event_loop);
        }
        if (consumer_for != nullptr) {
            consumer_for->set_log_consumer(nullptr);
        }
    }
};

// Bgproc (self-"backgrounding", i.e. double-forking) process service
class bgproc_service : public process_service
{
    virtual void handle_exit_status() noexcept override;
    virtual void exec_failed(run_proc_err errcode) noexcept override;

    enum class pid_result_t {
        OK,
        FAILED,      // failed to read pid or read invalid pid
        TERMINATED   // read pid successfully, but the process already terminated
    };

    string pid_file;

    // Read the pid-file contents
    pid_result_t read_pid_file(proc_status_t *exit_status) noexcept;

    public:
    bgproc_service(service_set *sset, const string &name, ha_string &&command,
            std::list<std::pair<unsigned,unsigned>> &command_offsets,
            const std::list<prelim_dep> &depends_p)
         : process_service(sset, name, service_type_t::BGPROCESS, std::move(command), command_offsets,
             depends_p)
    {
    }

    ~bgproc_service() noexcept
    {
    }

    void set_pid_file(string &&pid_file) noexcept
    {
        this->pid_file = std::move(pid_file);
    }

    const std::string &get_pid_file() noexcept
    {
        return pid_file;
    }
};

// Service which is started and stopped via separate commands
class scripted_service : public base_process_service
{
    virtual void handle_exit_status() noexcept override;
    virtual void exec_succeeded() noexcept override;
    virtual void exec_failed(run_proc_err errcode) noexcept override;
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
    scripted_service(service_set *sset, const string &name, ha_string &&command,
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
