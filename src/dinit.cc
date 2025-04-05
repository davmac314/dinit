#include <iostream>
#include <fstream>
#include <list>
#include <cstring>
#include <csignal>
#include <cstddef>
#include <cstdlib>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>
#include <termios.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/klog.h>
#include <sys/reboot.h>
#endif
#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/procctl.h>
#endif

#include <dasynq.h>

#include "dinit.h"
#include "service.h"
#include "control.h"
#include "dinit-log.h"
#include "dinit-socket.h"
#include "static-string.h"
#include "dinit-utmp.h"
#include "dinit-env.h"
#include "options-processing.h"

#include "mconfig.h"

/*
 * When running as the system init process, Dinit processes the following signals:
 *
 * SIGTERM - roll back services and then fork/exec /sbin/halt
 * SIGINT - roll back services and then fork/exec /sbin/reboot
 * SIGQUIT - exec() /sbin/shutdown without rolling back services
 *
 * It's an open question about whether Dinit should roll back services *before*
 * running halt/reboot, since those commands should prompt rollback of services
 * anyway. But it seems safe to do so, and it means the user can at least stop
 * services even if the halt/reboot commands are unavailable for some reason.
 */

using namespace cts;

using eventloop_t = dasynq::event_loop<dasynq::null_mutex>;

eventloop_t event_loop(dasynq::delayed_init {});

static void sigint_reboot_cb(eventloop_t &eloop) noexcept;
static void sigquit_cb(eventloop_t &eloop) noexcept;
static void sigterm_cb(eventloop_t &eloop) noexcept;
static bool open_control_socket(bool report_ro_failure = true) noexcept;
static void close_control_socket() noexcept;
static void control_socket_ready() noexcept;
static void confirm_restart_boot() noexcept;
static void flush_log() noexcept;

static void control_socket_cb(eventloop_t *loop, int fd) noexcept;

#ifdef SUPPORT_CGROUPS
static void find_cgroup_path() noexcept;
#endif

static void printVersion();

// Variables

static dirload_service_set *services;

static bool am_system_mgr = false;     // true if we are PID 1
static bool am_system_init = false; // true if we are the system init process
static bool auto_recovery = false;  // automatically run recovery service on boot failure

static bool did_log_boot = false;
static bool control_socket_open = false;
bool external_log_open = false;
int active_control_conns = 0;
int socket_ready_fd = -1;

sigset_t orig_signal_mask; // signal mask when started

// Control socket path. We maintain a string (control_socket_str) in case we need
// to allocate storage, but control_socket_path is the authoritative value.
static const char *control_socket_path = SYSCONTROLSOCKET;
static std::string control_socket_str;

static const char *env_file_path = "/etc/dinit/environment";

static const char *log_path = "/dev/log";
static bool log_is_syslog = true; // if false, log is a file

// Set to true (when console_input_watcher is active) if console input becomes available
static bool console_input_ready = false;

#ifdef SUPPORT_CGROUPS
// Path of the root cgroup according to dinit. This will be dinit's own cgroup path.
std::string cgroups_path;
bool have_cgroups_path = false;
#endif

namespace {
    // Event-loop handler for a signal, which just delegates to a function (pointer).
    class callback_signal_handler : public eventloop_t::signal_watcher_impl<callback_signal_handler>
    {
        using rearm = dasynq::rearm;

        public:
        typedef void (*cb_func_t)(eventloop_t &);
        
        private:
        cb_func_t cb_func;
        
        public:
        callback_signal_handler() : cb_func(nullptr) { }
        callback_signal_handler(cb_func_t pcb_func) :  cb_func(pcb_func) { }
        
        void set_cb_func(cb_func_t cb_func)
        {
            this->cb_func = cb_func;
        }
        
        rearm received(eventloop_t &eloop, int signo, siginfo_p siginfo)
        {
            cb_func(eloop);
            return rearm::REARM;
        }
    };

    // Event-loop handler for when a connection is made to the control socket.
    class control_socket_watcher : public eventloop_t::fd_watcher_impl<control_socket_watcher>
    {
        using rearm = dasynq::rearm;

        public:
        rearm fd_event(eventloop_t &loop, int fd, int flags) noexcept
        {
            control_socket_cb(&loop, fd);
            return rearm::REARM;
        }
    };

    // Watch for console input and set a flag when it is available.
    class console_input_watcher : public eventloop_t::fd_watcher_impl<console_input_watcher>
    {
        using rearm = dasynq::rearm;

        public:
        rearm fd_event(eventloop_t &loop, int fd, int flags) noexcept
        {
            console_input_ready = true;
            return rearm::DISARM;
        }
    };

    // Simple timer used to limit the amount of time waiting for the log flush to complete (at shutdown)
    class log_flush_timer_t : public eventloop_t::timer_impl<log_flush_timer_t>
    {
        using rearm = dasynq::rearm;

        bool expired;

        public:
        rearm timer_expiry(eventloop_t &, int expiry_count)
        {
            expired = true;
            return rearm::DISARM;
        }

        bool has_expired()
        {
            return expired;
        }

        void reset()
        {
            expired = false;
        }
    };

    control_socket_watcher control_socket_io;
    console_input_watcher console_input_io;
    log_flush_timer_t log_flush_timer;

    // These need to be at namespace scope to prevent causing stack allocations when using them:
    constexpr auto shutdown_exec = literal(SBINDIR) + "/" + SHUTDOWN_PREFIX + "shutdown";
    constexpr auto dinit_exec = literal(SBINDIR) + "/" + "dinit";
    constexpr auto error_exec_sd = literal("Error executing ") + shutdown_exec + ": ";
    constexpr auto error_exec_dinit = literal("Error executing ") + dinit_exec + ": ";
}

// Options handled in dinit_main
struct options {
    const char * env_file = nullptr;
    bool control_socket_path_set = false;
    bool env_file_set = false;
    bool log_specified = false;

    bool process_sys_args = false;

    service_dir_opt service_dir_opts;

    // list of services to start
    std::list<const char *> services_to_start;
};

// Process a command line argument (and possibly its follow-up value)
// Returns -1 for clean exit required, 0 for success, 1 for error exit required
static int process_commandline_arg(char **argv, int argc, int &i, options &opts)
{
    using std::cerr;
    using std::cout;
    using std::endl;
    using std::list;

    const char * &env_file = opts.env_file;
    bool &control_socket_path_set = opts.control_socket_path_set;
    bool &env_file_set = opts.env_file_set;
    bool &log_specified = opts.log_specified;
    service_dir_opt &service_dir_opts = opts.service_dir_opts;
    list<const char *> &services_to_start = opts.services_to_start;

    auto arg_to_loglevel = [&](const char *option_name, loglevel_t &wanted_level) {
        if (++i < argc && argv[i][0] != '\0') {
            if (strcmp(argv[i], "none") == 0) {
                wanted_level = loglevel_t::ZERO;
            }
            else if (strcmp(argv[i], "error") == 0) {
                wanted_level = loglevel_t::ERROR;
            }
            else if (strcmp(argv[i], "warn") == 0) {
                wanted_level = loglevel_t::WARN;
            }
            else if (strcmp(argv[i], "info") == 0) {
                wanted_level = loglevel_t::NOTICE;
            }
            else if (strcmp(argv[i], "debug") == 0) {
                wanted_level = loglevel_t::DEBUG;
            }
            else {
                cerr << "dinit: '" << option_name << "' accepts only arguments: 'none', 'error', 'warn', 'info', 'debug'\n";
                return false;
            }
            return true;
        }
        else {
            cerr << "dinit: '" << option_name << "' requires an argument\n";
            return false;
        }
    };

    if (argv[i][0] == '-') {
        // An option...
        if (strcmp(argv[i], "--env-file") == 0 || strcmp(argv[i], "-e") == 0) {
            if (++i < argc && argv[i][0] != '\0') {
                env_file_set = true;
                env_file = argv[i];
            }
            else {
                cerr << "dinit: '--env-file' (-e) requires an argument\n";
                return 1;
            }
        }
        else if (strcmp(argv[i], "--services-dir") == 0 || strcmp(argv[i], "-d") == 0) {
            if (++i < argc && argv[i][0] != '\0') {
                service_dir_opts.set_specified_service_dir(argv[i]);
            }
            else {
                cerr << "dinit: '--services-dir' (-d) requires an argument\n";
                return 1;
            }
        }
        else if (strcmp(argv[i], "--system") == 0 || strcmp(argv[i], "-s") == 0) {
            am_system_init = true;
        }
        else if (strcmp(argv[i], "--system-mgr") == 0 || strcmp(argv[i], "-m") == 0) {
            am_system_mgr = true;
            opts.process_sys_args = false;
        }
        else if (strcmp(argv[i], "--user") == 0 || strcmp(argv[i], "-u") == 0) {
            am_system_init = false;
        }
        else if (strcmp(argv[i], "--container") == 0 || strcmp(argv[i], "-o") == 0) {
            am_system_mgr = false;
            opts.process_sys_args = false;
        }
        else if (strcmp(argv[i], "--auto-recovery") == 0 || strcmp(argv[i], "-r") == 0) {
            auto_recovery = true;
        }
        else if (strcmp(argv[i], "--socket-path") == 0 || strcmp(argv[i], "-p") == 0) {
            if (++i < argc && argv[i][0] != '\0') {
                control_socket_path = argv[i];
                control_socket_path_set = true;
            }
            else {
                cerr << "dinit: '--socket-path' (-p) requires an argument\n";
                return 1;
            }
        }
        else if (strcmp(argv[i], "--ready-fd") == 0 || strcmp(argv[i], "-F") == 0) {
            if (++i < argc) {
                char *endp = nullptr;
                auto fdn = strtoul(argv[i], &endp, 10);
                if (endp == argv[i] || *endp) {
                    cerr << "dinit: '--ready-fd' (-F) requires a numerical argument\n";
                    return 1;
                }
                socket_ready_fd = int(fdn);
                auto fl = fcntl(socket_ready_fd, F_GETFD);
                // We also want to make sure stdin is not allowed
                if (socket_ready_fd == 0 || fl < 0) {
                    cerr << "dinit: '--ready-fd' (-F) requires an open file descriptor\n";
                    return 1;
                }
                // Leave standard file descriptors alone, but make sure
                // anything else is not leaked to child processes
                if (socket_ready_fd > 2) {
                    fcntl(socket_ready_fd, F_SETFD, FD_CLOEXEC | fl);
                }
            }
            else {
                cerr << "dinit: '--ready-fd' (-F) requires an argument\n";
                return 1;
            }
        }
        else if (strcmp(argv[i], "--log-file") == 0 || strcmp(argv[i], "-l") == 0) {
            if (++i < argc && argv[i][0] != '\0') {
                log_path = argv[i];
                log_is_syslog = false;
                log_specified = true;
            }
            else {
                cerr << "dinit: '--log-file' (-l) requires an argument\n";
                return 1;
            }
        }
        else if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "-q") == 0) {
            console_service_status = false;
            log_level[DLOG_CONS] = loglevel_t::ZERO;
        }
        else if (strcmp(argv[i], "--console-level") == 0) {
            loglevel_t wanted_level;
            if (!arg_to_loglevel("--console-level", wanted_level)) return 1;
            log_level[DLOG_CONS] = wanted_level;
        }
        else if (strcmp(argv[i], "--log-level") == 0) {
            loglevel_t wanted_level;
            if (!arg_to_loglevel("--log-level", wanted_level)) return 1;
            log_level[DLOG_MAIN] = wanted_level;
        }
        #ifdef SUPPORT_CGROUPS
        else if (strcmp(argv[i], "--cgroup-path") == 0 || strcmp(argv[i], "-b") == 0) {
            if (++i < argc && argv[i][0] != '\0') {
                cgroups_path = argv[i];
                have_cgroups_path = true;
            }
            else {
                cerr << "dinit: '--cgroup-path' (-b) requires an argument\n";
                return 1;
            }
        }
        #endif
        else if (strcmp(argv[i], "--service") == 0 || strcmp(argv[i], "-t") == 0) {
            if (++i < argc && argv[i][0] != '\0') {
                services_to_start.push_back(argv[i]);
            }
            else {
                cerr << "dinit: '--service' (-t) requires an argument\n";
                return 1;
            }
        }
        else if (strcmp(argv[i], "--version") == 0) {
            printVersion();
            return -1;
        }
        else if (strcmp(argv[i], "--help") == 0) {
            cout << "dinit: init/service manager daemon\n"
                    " --help                       display (this) help\n"
                    " --version                    display version\n"
                    " --env-file <file>, -e <file>\n"
                    "                              environment variable initialisation file\n"
                    " --services-dir <dir>, -d <dir>\n"
                    "                              set base directory for service description\n"
                    "                              files, can be specified multiple times\n"
                    " --system, -s                 run as the system service manager\n"
                    " --system-mgr, -m             run as system manager (perform shutdown etc)\n"
                    " --user, -u                   run as a user service manager\n"
                    " --container, -o              run in container mode (do not manage system)\n"
                    " --auto-recovery, -r          auto-run recovery service on system manager boot failure\n"
                    " --socket-path <path>, -p <path>\n"
                    "                              path to control socket\n"
                    " --ready-fd <fd>, -F <fd>\n"
                    "                              file descriptor to report readiness\n"
                    #ifdef SUPPORT_CGROUPS
                    " --cgroup-path <path>, -b <path>\n"
                    "                              cgroup base path (for resolving relative paths)\n"
                    #endif
                    " --log-file <file>, -l <file> log to the specified file\n"
                    " --quiet, -q                  disable output to standard output\n"
                    " <service-name>, --service <service-name>, -t <service-name>\n"
                    "                              start service with name <service-name>\n";
            return -1;
        }
        else {
            // unrecognized
            if (!opts.process_sys_args) {
                cerr << "dinit: unrecognized option: " << argv[i] << endl;
                return 1;
            }
        }
    }
    else {
        if (argv[i][0] == '\0') {
            cerr << "dinit: error: empty command-line argument\n";
            return 1;
        }
#ifdef __linux__
        // If we are running as init (PID=1), the Linux kernel gives us all command line arguments it was
        // given but didn't recognize, and, uh, *some* that it did recognize, which means we can't assume
        // that anything is a service name (for example "nopti" seems to get passed through to init).
        // However, we can look for special names that we know aren't kernel parameters, such as "single".
        //
        // (Note this may have been fixed in recent kernels: see changelog for 5.15.46/5.18.3,
        // "x86: Fix return value of __setup handlers")
        //
        // LILO puts "auto" on the command line for unattended boots, but we don't care about that and want
        // it filtered.
        //
        // We don't expect to see options beginning with '-' appear on the kernel command line either, so we
        // can interpret those as dinit arguments. In particular if we see -m or -o, we assume that every
        // name we see from then is a service name (i.e. process_sys_args is set false when we seem them,
        // see above).
        //
        // (Note, you can give "--" on the kernel command line to pass every option from that point to init
        // directly, but init doesn't see the "--" itself, which makes it less useful, since we still can't
        // tell whether a "name" was intended as a kernel parameter or init parameter).

        // So, long story short: if we think we're PID 1 and we haven't seen -m or -c options yet, only
        // recognise "single" as a service name and ignore everything else.

        if (!opts.process_sys_args || strcmp(argv[i], "single") == 0) {
            services_to_start.push_back(argv[i]);
        }
#else
        services_to_start.push_back(argv[i]);
#endif
    }

    return 0;
}

// Main entry point
int dinit_main(int argc, char **argv)
{
    using namespace std;
    
    am_system_mgr = (getpid() == 1);
    am_system_init = (getuid() == 0);
    
    struct options opts;

    // if we are PID 1 and user id 0, we are *most probably* the system init. (Or on linux at least, we
    // could instead be in a container; then we expect -o argument and unset this then).
    opts.process_sys_args = am_system_mgr && am_system_init;

    const char * &env_file = opts.env_file;
    bool &control_socket_path_set = opts.control_socket_path_set;
    bool &env_file_set = opts.env_file_set;
    bool &log_specified = opts.log_specified;
    service_dir_opt &service_dir_opts = opts.service_dir_opts;
    list<const char *> &services_to_start = opts.services_to_start;

    for (int i = 1; i < argc; i++) {
        int p = process_commandline_arg(argv, argc, i, opts);
        if (p == -1) {
            // clean exit
            return 0;
        }
        if (p == 1) {
            // error exit
            return 1;
        }
    }

    if (am_system_mgr) {
        // setup STDIN, STDOUT, STDERR so that we can use them
        int onefd = open("/dev/console", O_RDONLY, 0);
        if (onefd != -1) {
            dup2(onefd, 0);
        }
        int twofd = open("/dev/console", O_RDWR, 0);
        if (twofd != -1) {
            dup2(twofd, 1);
            dup2(twofd, 2);
        }
        
        if (onefd > 2) close(onefd);
        if (twofd > 2) close(twofd);

        if (! env_file_set) {
            env_file = env_file_path;
        }

        // we will assume an empty cgroups root path
        #if SUPPORT_CGROUPS
        have_cgroups_path = true;
        #endif
    }

    /* Set up signal handlers etc */
    sigset_t sigwait_set;
    if (am_system_mgr) {
        // Block all signals in system manager mode - don't want to chance provoking a signal that
        // will suspend or terminate the process
        sigfillset(&sigwait_set);
    }
    else {
        sigemptyset(&sigwait_set);
        sigaddset(&sigwait_set, SIGCHLD);
        sigaddset(&sigwait_set, SIGINT);
        sigaddset(&sigwait_set, SIGTERM);
    }
    sigprocmask(SIG_BLOCK, &sigwait_set, &orig_signal_mask);

    // Terminal access control signals - we ignore these so that dinit can't be
    // suspended if it writes to the terminal after some other process has claimed
    // ownership of it.
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    
    signal(SIGPIPE, SIG_IGN);
    
    event_loop.init();

    if (!am_system_init && !control_socket_path_set) {
        const char * rundir = getenv("XDG_RUNTIME_DIR");
        const char * sockname = "dinitctl";
        if (rundir == nullptr) {
            rundir = service_dir_opt::get_user_home();
            sockname = ".dinitctl";
        }
        if (rundir != nullptr) {
            control_socket_str = rundir;
            control_socket_str.push_back('/');
            control_socket_str += sockname;
            control_socket_path = control_socket_str.c_str();
        }
    }
    
    if (services_to_start.empty()) {
        services_to_start.push_back("boot");
    }

    // Set up signal handlers
    callback_signal_handler sigterm_watcher {sigterm_cb};
    callback_signal_handler sigint_watcher;
    callback_signal_handler sigquit_watcher;

    if (am_system_mgr) {
        sigint_watcher.set_cb_func(sigint_reboot_cb);
        sigquit_watcher.set_cb_func(sigquit_cb);
    }
    else {
        sigint_watcher.set_cb_func(sigterm_cb);
    }

    sigint_watcher.add_watch(event_loop, SIGINT);
    sigterm_watcher.add_watch(event_loop, SIGTERM);
    
    if (am_system_mgr) {
        // PID 1: we may ask for console input; SIGQUIT exec's shutdown
        console_input_io.add_watch(event_loop, STDIN_FILENO, dasynq::IN_EVENTS, false);
        sigquit_watcher.add_watch(event_loop, SIGQUIT);
        // (If not PID 1, we instead just let SIGQUIT perform the default action.)
    }

    init_log(log_is_syslog);
    log_flush_timer.add_timer(event_loop, dasynq::clock_type::MONOTONIC);

    #if SUPPORT_CGROUPS
    if (!have_cgroups_path) {
        find_cgroup_path();
        // We will press on if the cgroup root path could not be identified, since services might
        // not require cgroups anyway and/or might only specify absolute cgroups paths.
    }
    #endif

    // Try to open control socket (may fail due to readonly filesystem, we ignore that if we are
    // system init)
    if (!open_control_socket(!am_system_init)) {
        flush_log();
        return EXIT_FAILURE;
    }
    
#ifdef __linux__
    if (am_system_mgr) {
        // Disable non-critical kernel output to console
        klogctl(6 /* SYSLOG_ACTION_CONSOLE_OFF */, nullptr, 0);
        // Make ctrl+alt+del combination send SIGINT to PID 1 (this process)
        reboot(RB_DISABLE_CAD);
    }

    // Mark ourselves as a subreaper. This means that if a process we start double-forks, the
    // orphaned child will re-parent to us rather than to PID 1 (although that could be us too).
    prctl(PR_SET_CHILD_SUBREAPER, 1);
#elif defined(__FreeBSD__) || defined(__DragonFly__)
    // Documentation (man page) for this kind-of sucks. PROC_REAP_ACQUIRE "acquires the reaper status for
    // the current process" but does that mean the first two arguments still need valid values to be
    // supplied? We'll play it safe and explicitly target our own process:
    procctl(P_PID, getpid(), PROC_REAP_ACQUIRE, NULL);
#endif
    
    service_dir_opts.build_paths(am_system_init);

    // Start requested services

    services = new dirload_service_set(std::move(service_dir_opts.get_paths()));

    setup_log_console_handoff(services);

    if (am_system_init) {
        log(loglevel_t::NOTICE, false, "Starting system");
    }
    
    // If a log file was specified, open it now.
    if (log_specified) {
        setup_external_log();
        if (!am_system_init && !external_log_open) {
            flush_log(); // flush console messages
            return EXIT_FAILURE;
        }
    }

    if (env_file != nullptr) {
        read_env_file(env_file, true, main_env, false);
    }

    for (auto svc : services_to_start) {
        try {
            services->start_service(svc);
            // Note in general if we fail to start a service we don't need any special error handling,
            // since we either leave other services running or, if it was the only service, then no
            // services will be running and we will process normally (reboot if system process,
            // exit if user process).
        }
        catch (service_not_found &snf) {
            log(loglevel_t::ERROR, snf.service_name, ": could not find service description.");
        }
        catch (service_description_exc &sde) {
            log_service_load_failure(sde);
        }
        catch (service_load_exc &sle) {
            log(loglevel_t::ERROR, sle.service_name, ": error loading: ", sle.exc_description);
        }
        catch (std::bad_alloc &badalloce) {
            log(loglevel_t::ERROR, "Out of memory when trying to start service: ", svc, ".");
            break;
        }
    }

    // Notify readiness just before the event loop starts (and after services
    // are scheduled to start). If the socket is not ready yet (may be in case
    // of read-only file system), we will report it when it is.
    control_socket_ready();
    
    run_event_loop:
    
    // Process events until all services have terminated.
    while (services->count_active_services() != 0) {
        event_loop.run();
    }

    shutdown_type_t shutdown_type = services->get_shutdown_type();
    if (shutdown_type == shutdown_type_t::REMAIN) {
        goto run_event_loop;
    }
    
    if (am_system_mgr) {
        log_msg_begin(loglevel_t::NOTICE, "No more active services.");
        
        if (shutdown_type == shutdown_type_t::REBOOT) {
            log_msg_end(" Will reboot.");
        }
        if (shutdown_type == shutdown_type_t::SOFTREBOOT) {
            log_msg_end(" Will soft-reboot.");
        }
        else if (shutdown_type == shutdown_type_t::HALT) {
            log_msg_end(" Will halt.");
        }
        else if (shutdown_type == shutdown_type_t::POWEROFF) {
            log_msg_end(" Will power down.");
        }
        else if (shutdown_type == shutdown_type_t::KEXEC) {
            log_msg_end(" Will kexec.");
        }
        else if (shutdown_type == shutdown_type_t::NONE) {
            log_msg_end(" Will handle boot failure.");
        }
    }

    flush_log();
    bool need_log_flush = false;
    close_control_socket();
    
    if (am_system_mgr) {
        if (shutdown_type == shutdown_type_t::SOFTREBOOT) {
            sync(); // Sync to minimise data loss in case soft-boot fails

            execv(dinit_exec, argv);
            log(loglevel_t::ERROR, error_exec_dinit, strerror(errno));

            // if we get here, soft reboot failed; reboot normally
            log(loglevel_t::ERROR, "Could not soft-reboot. Will attempt reboot.");
            shutdown_type = shutdown_type_t::REBOOT;
            need_log_flush = true;
        }

        if (shutdown_type == shutdown_type_t::NONE) {
            // Services all stopped but there was no shutdown issued. Inform user, wait for ack, and
            // re-start boot sequence.
            sync(); // Sync to minimise data loss if user elects to power off / hard reset
            if (auto_recovery) {
                try {
                    services->start_service("recovery");
                }
                catch (std::exception &exc) {
                    log(loglevel_t::ERROR, "Unable to start recovery service: ", exc.what());
                    // As the following prompt UI could be inaccessible flush the log again already
                    flush_log();
                }
            }
            confirm_restart_boot();
            if (services->count_active_services() != 0) {
                // Recovery service started
                goto run_event_loop;
            }
            shutdown_type = services->get_shutdown_type();
            if (shutdown_type == shutdown_type_t::NONE) {
                try {
                    services->start_service("boot");
                    goto run_event_loop; // yes, the "evil" goto
                }
                catch (...) {
                    // Couldn't start boot service, let's reboot the system
                    log(loglevel_t::ERROR, "Could not start 'boot' service. Will attempt reboot.");
                    need_log_flush = true;
                    shutdown_type = shutdown_type_t::REBOOT;
                }
            }
        }
        
        if (need_log_flush) {
            // In case of error since the log was previously flushed, flush again now
            flush_log();
        }

        const char * cmd_arg;
        if (shutdown_type == shutdown_type_t::HALT) {
            cmd_arg = "-h";
        }
        else if (shutdown_type == shutdown_type_t::REBOOT) {
            cmd_arg = "-r";
        }
        else if (shutdown_type == shutdown_type_t::KEXEC) {
            cmd_arg = "-k";
        }
        else {
            // power off.
            cmd_arg = "-p";
        }
        
        // Fork and execute dinit-reboot.
        execl(shutdown_exec.c_str(), shutdown_exec.c_str(), "--system", cmd_arg, nullptr);
        log(loglevel_t::ERROR, error_exec_sd, strerror(errno));
        
        // PID 1 must not actually exit, although we should never reach this point:
        while (true) {
            event_loop.run();
        }
    }
    else if (shutdown_type == shutdown_type_t::REBOOT) {
        // Non-system-process. If we got SIGINT, let's die due to it:
        sigset_t sigwait_set_int;
        sigemptyset(&sigwait_set_int);
        sigaddset(&sigwait_set_int, SIGINT);
        raise(SIGINT);
        sigprocmask(SIG_UNBLOCK, &sigwait_set_int, NULL);
    }
    
    return EXIT_SUCCESS;
}

// Get user confirmation before proceeding with restarting boot sequence.
// Returns after confirmation, possibly with shutdown type altered.
static void confirm_restart_boot() noexcept
{
    // Bypass log; we want to make certain the message is seen:
    std::cout << "All services have stopped with no shutdown issued; boot failure?\n";

    // Drain input, set non-canonical input mode (receive characters as they are typed)
    struct termios term_attr;
    if (tcgetattr(STDIN_FILENO, &term_attr) != 0) {
        // Not a terminal?
        std::cout << "Halting." << std::endl;
        services->stop_all_services(shutdown_type_t::HALT);
        return;
    }
    term_attr.c_lflag &= ~ICANON;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_attr);

    // Set non-blocking mode
    int origFlags = fcntl(STDIN_FILENO, F_GETFL);
    fcntl(STDIN_FILENO, F_SETFL, origFlags | O_NONBLOCK);

    do_prompt:
    std::cout << "Choose: (r)eboot, r(e)covery, re(s)tart boot sequence, (p)ower off? " << std::flush;

    console_input_io.set_enabled(event_loop, true);
    do {
        event_loop.run();
    } while (! console_input_ready && services->get_shutdown_type() == shutdown_type_t::NONE);
    console_input_io.set_enabled(event_loop, false);

    // We either have input, or shutdown type has been set, or both.
    if (console_input_ready) {
        console_input_ready = false;
        char buf[1];
        int r = read(STDIN_FILENO, buf, 1);  // read a single character, to make sure we wait for input
        if (r == 1) {
            std::cout << "\n"; // force new line after input
            if (buf[0] == 'r' || buf[0] == 'R') {
                services->stop_all_services(shutdown_type_t::REBOOT);
            }
            else if (buf[0] == 'e' || buf[0] == 'E') {
                try {
                    services->start_service("recovery");
                }
                catch (...) {
                    std::cout << "Unable to start recovery service.\n";
                    goto do_prompt;
                }
            }
            else if (buf[0] == 's' || buf[0] == 'S') {
                // nothing - leave no shutdown type
            }
            else if (buf[0] == 'p' || buf[0] == 'P') {
                services->stop_all_services(shutdown_type_t::POWEROFF);
            }
            else {
                goto do_prompt;
            }
        }
        tcflush(STDIN_FILENO, TCIFLUSH); // discard the rest of input
    }

    term_attr.c_lflag |= ICANON;
    tcsetattr(STDIN_FILENO, TCSANOW, &term_attr);
    fcntl(STDIN_FILENO, F_SETFL, origFlags);
}

// Callback for control socket
static void control_socket_cb(eventloop_t *loop, int sockfd) noexcept
{
    // Considered keeping a limit the number of active connections, however, there doesn't
    // seem much to be gained from that. Only root can create connections and not being
    // able to establish a control connection is as much a denial-of-service as is not being
    // able to start a service due to lack of fd's.

    // Accept a connection
    int newfd = dinit_accept4(sockfd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (newfd != -1) {
        try {
            new control_conn_t(*loop, services, newfd);  // will delete itself when it's finished
        }
        catch (std::exception &exc) {
            log(loglevel_t::ERROR, "Error accepting control connection: ", exc.what());
            close(newfd);
        }
    }
}

static void control_socket_ready() noexcept
{
    if (!control_socket_open || socket_ready_fd < 0) {
        return;
    }
    write(socket_ready_fd, control_socket_path, strlen(control_socket_path) + 1);
    // Once done with, close it (but leave stdout/stderr alone)
    if (socket_ready_fd > 2) {
        close(socket_ready_fd);
    }
    // Ensure that we don't try to issue readiness again:
    socket_ready_fd = -1;
}

// Callback when the root filesystem is read/write:
void rootfs_is_rw() noexcept
{
    open_control_socket(true);
    control_socket_ready();
    if (!log_is_syslog && !external_log_open) {
        // Try (again) to open log file if we couldn't do so earlier.
        setup_external_log();
    }
    if (!did_log_boot) {
        did_log_boot = log_boot();
    }
}

// Open/create the control socket, normally /run/dinitctl, used to allow client programs to connect
// and issue service orders and shutdown commands etc. This can safely be called multiple times;
// once the socket has been successfully opened, further calls will check the socket file is still
// present and re-create it if not.
static bool open_control_socket(bool report_ro_failure) noexcept
{
    if (control_socket_open) {
        struct stat stat_buf;
        if (stat(control_socket_path, &stat_buf) != 0 && errno == ENOENT) {
            // Looks like our control socket has disappeared from the filesystem. Close our control
            // socket and re-create it:
            control_socket_io.deregister(event_loop);
            close(control_socket_io.get_watched_fd());
            control_socket_open = false; // now re-open below
        }
    }

    if (!control_socket_open) {
        const char * saddrname = control_socket_path;
        size_t saddrname_len = strlen(saddrname);
        uint sockaddr_size = offsetof(struct sockaddr_un, sun_path) + saddrname_len + 1;
        
        struct sockaddr_un * name = static_cast<sockaddr_un *>(malloc(sockaddr_size));
        if (name == nullptr) {
            log(loglevel_t::ERROR, "Opening control socket: out of memory");
            return false;
        }

        name->sun_family = AF_UNIX;
        memcpy(name->sun_path, saddrname, saddrname_len + 1);

        int sockfd = dinit_socket(AF_UNIX, SOCK_STREAM, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (sockfd == -1) {
            log(loglevel_t::ERROR, "Error creating control socket: ", strerror(errno));
            free(name);
            return false;
        }

        // Check if there is already an active control socket (from another instance).
        // Unfortunately, there's no way to check atomically if a socket file is stale. Still, we
        // will try to check, since the consequences of running a system dinit instance twice are
        // potentially severe.
        int connr = connect(sockfd, (struct sockaddr *) name, sockaddr_size);
        if (connr != -1 || errno == EAGAIN) {
            log(loglevel_t::ERROR, "Control socket is already active"
                    " (another instance already running?)");

            close(connr);
            close(sockfd);
            free(name);

            return false;
        }

        // Unlink any stale control socket file.
        //
        // In the worst case, this potentially removes a socket which was not active at the time
        // we checked (just above) but has since become active; there's just no good API to avoid
        // this (we'd have to use a file lock, on yet another file). Since that's unlikely to
        // occur in practice, and because a stale socket will prevent communication with dinit (or
        // prevent it starting), then we'll take the chance on unlinking here.
        unlink(saddrname);

        if (bind(sockfd, (struct sockaddr *) name, sockaddr_size) == -1) {
            bool have_error = (errno != EROFS || report_ro_failure);
            if (have_error) {
                log(loglevel_t::ERROR, "Error binding control socket: ", strerror(errno));
            }
            close(sockfd);
            free(name);
            return !have_error;
        }
        
        free(name);

        // No connections can be made until we listen, so it is fine to change the permissions now
        // (and anyway there is no way to atomically create the socket and set permissions):
        if (chmod(saddrname, S_IRUSR | S_IWUSR) == -1) {
            log(loglevel_t::ERROR, "Error setting control socket permissions: ", strerror(errno));
            close(sockfd);
            return false;
        }

        if (listen(sockfd, 10) == -1) {
            log(loglevel_t::ERROR, "Error listening on control socket: ", strerror(errno));
            close(sockfd);
            return false;
        }

        try {
            control_socket_io.add_watch(event_loop, sockfd, dasynq::IN_EVENTS);
            control_socket_open = true;
        }
        catch (std::exception &e)
        {
            log(loglevel_t::ERROR, "Could not setup I/O on control socket: ", e.what());
            close(sockfd);
        }
    }

    return control_socket_open;
}

static void close_control_socket() noexcept
{
    if (control_socket_open) {
        int fd = control_socket_io.get_watched_fd();
        control_socket_io.deregister(event_loop);
        close(fd);
        
        // Unlink the socket:
        unlink(control_socket_path);

        control_socket_open = false;
    }
}

void setup_external_log() noexcept
{
    if (!external_log_open) {
        if (log_is_syslog) {
            const char * saddrname = log_path;
            size_t saddrname_len = strlen(saddrname);
            uint sockaddr_size = offsetof(struct sockaddr_un, sun_path) + saddrname_len + 1;

            struct sockaddr_un * name = static_cast<sockaddr_un *>(malloc(sockaddr_size));
            if (name == nullptr) {
                log(loglevel_t::ERROR, "Connecting to log socket: out of memory");
                return;
            }

            name->sun_family = AF_UNIX;
            memcpy(name->sun_path, saddrname, saddrname_len + 1);

            int sockfd = dinit_socket(AF_UNIX, SOCK_DGRAM, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (sockfd == -1) {
                log(loglevel_t::ERROR, "Error creating log socket: ", strerror(errno));
                free(name);
                return;
            }

            if (connect(sockfd, (struct sockaddr *) name, sockaddr_size) == 0 || errno == EINPROGRESS) {
                // For EINPROGRESS, connection is still being established; however, we can select on
                // the file descriptor so we will be notified when it's ready. In other words we can
                // basically use it anyway.
                try {
                    setup_main_log(sockfd);
                    external_log_open = true;
                }
                catch (std::exception &e) {
                    log(loglevel_t::ERROR, "Setting up log failed: ", e.what());
                    close(sockfd);
                }
            }
            else {
                // Note if connect fails, we haven't warned at all, because the syslog server might not
                // have started yet.
                close(sockfd);
            }

            free(name);
        }
        else {
            // log to file:
            int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_NONBLOCK | O_CLOEXEC, 0644);
            if (log_fd >= 0) {
                try {
                    setup_main_log(log_fd);
                    external_log_open = true;
                }
                catch (std::exception &e) {
                    log(loglevel_t::ERROR, "Setting up log failed: ", e.what());
                    close(log_fd);
                }
            }
            else {
                // log failure to log? It makes more sense than first appears, because we also log
                // to console:
                log(loglevel_t::ERROR, "Opening log file failed: ", strerror(errno));
            }
        }
    }
}

static void flush_log() noexcept
{
    log_flush_timer.reset();
    log_flush_timer.arm_timer_rel(event_loop, timespec{5,0}); // 5 seconds
    while (!is_log_flushed() && !log_flush_timer.has_expired()) {
        event_loop.run();
    }
}

#ifdef SUPPORT_CGROUPS

static void find_cgroup_path() noexcept
{
    if (have_cgroups_path) {
        return;
    }

    int pfd = open("/proc/self/cgroup", O_RDONLY);
    if (pfd == -1) {
        return;
    }

    try {
        size_t cgroup_line_sz = 64;
        size_t cur_read = 0;
        size_t line_end_pos = (size_t)-1;
        size_t colon_count = 0; // how many colons have we seen?
        size_t second_colon_pos = 0;
        std::vector<char, default_init_allocator<char>> cgroup_line(cgroup_line_sz);

        while (true) {
            ssize_t r = read(pfd, cgroup_line.data() + cur_read, cgroup_line_sz - cur_read);
            if (r == 0) {
                if (line_end_pos == (size_t)-1) {
                    line_end_pos = cur_read + 1;
                }
                break;
            }
            if (r == -1) {
                close(pfd);
                return;
            }

            size_t rr = (size_t)r;
            for (size_t i = 0; i < rr; ++i) {
                if (cgroup_line[cur_read + i] == '\n') {
                    line_end_pos = cur_read + i;
                }
                else if (line_end_pos != (size_t)-1) {
                    log(loglevel_t::WARN, "In multiple cgroups, cannot determine cgroup root path");
                    close(pfd);
                    return;
                }
                else if (cgroup_line[cur_read + i] == ':') {
                    if (++colon_count == 2) {
                        second_colon_pos = cur_read + i;
                    }
                }
            }

            cur_read += rr;
            if (line_end_pos != (size_t)-1) {
                break;
            }

            if (cur_read == cgroup_line_sz) {
                cgroup_line.resize(cgroup_line_sz * 2);
                cgroup_line_sz *= 2;
            }
        };

        close(pfd);
        pfd = -1;

        // Now extract the path
        // The group line should look something like:
        //
        //    0::/some/path
        //
        // We want "some/path", i.e. we'll skip the leading slash.
        if (colon_count < 2 || (line_end_pos - second_colon_pos) == 1
                || cgroup_line[second_colon_pos+1] != '/') {
            // path is from 2nd colon to end
            log(loglevel_t::WARN, "Could not determine cgroup root path");
            return;
        }

        cgroups_path.clear();
        size_t first_char_pos = second_colon_pos + 2;
        size_t root_path_len = line_end_pos - first_char_pos;
        cgroups_path.append(cgroup_line.data() + first_char_pos, root_path_len);
        have_cgroups_path = true;
        return;
    }
    catch (std::bad_alloc &b) {
        if (pfd != -1) {
            close(pfd);
        }
        log(loglevel_t::WARN, "Out-of-memory reading cgroup root path");
        return;
    }
}

#endif // SUPPORT_CGROUPS

static void printVersion()
{
    std::cout << "Dinit version " << DINIT_VERSION << '.' << std::endl;
    const unsigned feature_count = 0
#ifdef SUPPORT_CGROUPS
            +1
#endif
#ifdef USE_UTMPX
            +1
#endif
#if USE_INITGROUPS
            +1
#endif
#if SUPPORT_CAPABILITIES
            +1
#endif
#if SUPPORT_IOPRIO
            +1
#endif
#if SUPPORT_OOM_ADJ
            +1
#endif
            ;
    if (feature_count != 0) {
        std::cout << "Supported features:"
#ifdef SUPPORT_CGROUPS
                " cgroups"
#endif
#ifdef USE_UTMPX
                " utmp"
#endif
#if USE_INITGROUPS
                " supplemental-groups"
#endif
#if SUPPORT_CAPABILITIES
                " capabilities"
#endif
#if SUPPORT_IOPRIO
                " io-priority"
#endif
#if SUPPORT_OOM_ADJ
                " oom-score-adjust"
#endif
                "\n";
    }
}

/* handle SIGINT signal (generated by Linux kernel when ctrl+alt+del pressed) */
static void sigint_reboot_cb(eventloop_t &eloop) noexcept
{
    services->stop_all_services(shutdown_type_t::REBOOT);
}

/* handle SIGQUIT (if we are system init) */
static void sigquit_cb(eventloop_t &eloop) noexcept
{
    // This performs an immediate shutdown, without service rollback.
    close_control_socket();
    execl(shutdown_exec.c_str(), shutdown_exec.c_str(), "--system", (char *) 0);
    log(loglevel_t::ERROR, error_exec_sd, strerror(errno));
    sync(); // since a hard poweroff might be required at this point...
}

/* handle SIGTERM/SIGQUIT(non-system-daemon) - stop all services and shut down */
static void sigterm_cb(eventloop_t &eloop) noexcept
{
    services->stop_all_services();
}
