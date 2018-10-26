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
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/klog.h>
#include <sys/reboot.h>
#endif
#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/procctl.h>
#endif

#include "dinit.h"
#include "dasynq.h"
#include "service.h"
#include "control.h"
#include "dinit-log.h"
#include "dinit-socket.h"
#include "static-string.h"

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

eventloop_t event_loop;

static void sigint_reboot_cb(eventloop_t &eloop) noexcept;
static void sigquit_cb(eventloop_t &eloop) noexcept;
static void sigterm_cb(eventloop_t &eloop) noexcept;
static void close_control_socket() noexcept;
static void wait_for_user_input() noexcept;
static void read_env_file(const char *);

static void control_socket_cb(eventloop_t *loop, int fd);


// Variables

static dirload_service_set *services;

static bool am_pid_one = false;     // true if we are PID 1
static bool am_system_init = false; // true if we are the system init process

static bool control_socket_open = false;
static bool external_log_open = false;
int active_control_conns = 0;

// Control socket path. We maintain a string (control_socket_str) in case we need
// to allocate storage, but control_socket_path is the authoritative value.
static const char *control_socket_path = SYSCONTROLSOCKET;
static std::string control_socket_str;

static const char *env_file_path = "/etc/dinit/environment";

static const char *log_path = "/dev/log";
static bool log_is_syslog = true; // if false, log is a file

static const char *user_home_path = nullptr;


// Get user home (and set user_home_path). (The return may become invalid after
// changing the evironment (HOME variable) or using the getpwuid() function).
const char * get_user_home()
{
    if (user_home_path == nullptr) {
        user_home_path = getenv("HOME");
        if (user_home_path == nullptr) {
            struct passwd * pwuid_p = getpwuid(getuid());
            if (pwuid_p != nullptr) {
                user_home_path = pwuid_p->pw_dir;
            }
        }
    }
    return user_home_path;
}


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

    // Simple timer used to limit the amount of time waiting for the log flush to complete (at shutdown)
    class log_flush_timer_t : public eventloop_t::timer_impl<log_flush_timer_t>
    {
        using rearm = dasynq::rearm;

        bool expired = false;

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
    };

    control_socket_watcher control_socket_io;
    log_flush_timer_t log_flush_timer;
}

int dinit_main(int argc, char **argv)
{
    using namespace std;
    
    am_pid_one = (getpid() == 1);
    am_system_init = (getuid() == 0);
    const char * service_dir = nullptr;
    bool service_dir_dynamic = false; // service_dir dynamically allocated?
    const char * env_file = nullptr;
    bool control_socket_path_set = false;
    bool env_file_set = false;

    // list of services to start
    list<const char *> services_to_start;
    
    // Arguments, if given, specify a list of services to start.
    // If we are running as init (PID=1), the Linux kernel gives us any command line arguments it was given
    // but didn't recognize, including "single" (usually for "boot to single user mode" aka just start the
    // shell). We can treat them as service names. In the worst case we can't find any of the named
    // services, and so we'll start the "boot" service by default.
    if (argc > 1) {
      for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            // An option...
            if (strcmp(argv[i], "--env-file") == 0 || strcmp(argv[i], "-e") == 0) {
                if (++i < argc) {
                    env_file_set = true;
                    env_file = argv[i];
                }
                else {
                    cerr << "dinit: '--env-file' (-e) requires an argument" << endl;
                }
            }
            else if (strcmp(argv[i], "--services-dir") == 0 || strcmp(argv[i], "-d") == 0) {
                if (++i < argc) {
                    service_dir = argv[i];
                }
                else {
                    cerr << "dinit: '--services-dir' (-d) requires an argument" << endl;
                    return 1;
                }
            }
            else if (strcmp(argv[i], "--system") == 0 || strcmp(argv[i], "-s") == 0) {
                am_system_init = true;
            }
            else if (strcmp(argv[i], "--socket-path") == 0 || strcmp(argv[i], "-p") == 0) {
                if (++i < argc) {
                    control_socket_path = argv[i];
                    control_socket_path_set = true;
                }
                else {
                    cerr << "dinit: '--socket-path' (-p) requires an argument" << endl;
                    return 1;
                }
            }
            else if (strcmp(argv[i], "--log-file") == 0 || strcmp(argv[i], "-l") == 0) {
                if (++i < argc) {
                    log_path = argv[i];
                    log_is_syslog = false;
                }
                else {
                    cerr << "dinit: '--log-file' (-l) requires an argument" << endl;
                    return 1;
                }
            }
            else if (strcmp(argv[i], "--help") == 0) {
                cout << "dinit, an init with dependency management\n"
                        " --help                       display help\n"
                        " --env-file <file>, -e <file>\n"
                        "                              environment variable initialisation file\n"
                        " --services-dir <dir>, -d <dir>\n"
                        "                              set base directory for service description\n"
                        "                              files (-d <dir>)\n"
                        " --system, -s                 run as the system init process\n"
                        " --socket-path <path>, -p <path>\n"
                        "                              path to control socket\n"
                        " <service-name>               start service with name <service-name>\n";
                return 0;
            }
            else {
                // unrecognized
                if (! am_system_init) {
                    cerr << "dinit: Unrecognized option: " << argv[i] << endl;
                    return 1;
                }
            }
        }
        else {
#ifdef __linux__
            // LILO puts "auto" on the kernel command line for unattended boots; we'll filter it.
            if (! am_system_init || strcmp(argv[i], "auto") != 0) {
                services_to_start.push_back(argv[i]);
            }
#else
            services_to_start.push_back(argv[i]);
#endif
        }
      }
    }
    
    if (am_system_init) {
        // setup STDIN, STDOUT, STDERR so that we can use them
        int onefd = open("/dev/console", O_RDONLY, 0);
        dup2(onefd, 0);
        int twofd = open("/dev/console", O_RDWR, 0);
        dup2(twofd, 1);
        dup2(twofd, 2);
        
        if (onefd > 2) close(onefd);
        if (twofd > 2) close(twofd);

        if (! env_file_set) {
            env_file = env_file_path;
        }
    }

    /* Set up signal handlers etc */
    /* SIG_CHILD is ignored by default: good */
    sigset_t sigwait_set;
    sigemptyset(&sigwait_set);
    sigaddset(&sigwait_set, SIGCHLD);
    sigaddset(&sigwait_set, SIGINT);
    sigaddset(&sigwait_set, SIGTERM);
    if (am_pid_one) sigaddset(&sigwait_set, SIGQUIT);
    sigprocmask(SIG_BLOCK, &sigwait_set, NULL);

    // Terminal access control signals - we block these so that dinit can't be
    // suspended if it writes to the terminal after some other process has claimed
    // ownership of it.
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    
    signal(SIGPIPE, SIG_IGN);
    
    if (! am_system_init && ! control_socket_path_set) {
        const char * userhome = get_user_home();
        if (userhome != nullptr) {
            control_socket_str = userhome;
            control_socket_str += "/.dinitctl";
            control_socket_path = control_socket_str.c_str();
        }
    }
    
    /* service directory name */
    if (service_dir == nullptr && ! am_system_init) {
        const char * userhome = get_user_home();
        if (userhome != nullptr) {
            const char * user_home = get_user_home();
            size_t user_home_len = strlen(user_home);
            size_t dinit_d_len = strlen("/dinit.d");
            size_t full_len = user_home_len + dinit_d_len + 1;
            char *service_dir_w = new char[full_len];
            std::memcpy(service_dir_w, user_home, user_home_len);
            std::memcpy(service_dir_w + user_home_len, "/dinit.d", dinit_d_len);
            service_dir_w[full_len - 1] = 0;

            service_dir = service_dir_w;
            service_dir_dynamic = true;
        }
    }
    
    if (services_to_start.empty()) {
        services_to_start.push_back("boot");
    }

    // Set up signal handlers
    callback_signal_handler sigterm_watcher {sigterm_cb};
    callback_signal_handler sigint_watcher;
    callback_signal_handler sigquit_watcher;

    if (am_pid_one) {
        sigint_watcher.set_cb_func(sigint_reboot_cb);
        sigquit_watcher.set_cb_func(sigquit_cb);
    }
    else {
        sigint_watcher.set_cb_func(sigterm_cb);
    }

    sigint_watcher.add_watch(event_loop, SIGINT);
    sigterm_watcher.add_watch(event_loop, SIGTERM);
    
    if (am_pid_one) {
        // PID 1: SIGQUIT exec's shutdown
        sigquit_watcher.add_watch(event_loop, SIGQUIT);
        // As a user process, we instead just let SIGQUIT perform the default action.
    }

    // Try to open control socket (may fail due to readonly filesystem)
    open_control_socket(false);
    
#ifdef __linux__
    if (am_system_init) {
        // Disable non-critical kernel output to console
        klogctl(6 /* SYSLOG_ACTION_CONSOLE_OFF */, nullptr, 0);
        // Make ctrl+alt+del combination send SIGINT to PID 1 (this process)
        reboot(RB_DISABLE_CAD);
    }

    // Mark ourselves as a subreaper. This means that if a process we start double-forks, the
    // orphaned child will re-parent to us rather than to PID 1 (although that could be us too).
    prctl(PR_SET_CHILD_SUBREAPER, 1);
#elif defined(__FreeBSD__) || defined(__DragonFly__)
    // Documentation (man page) for this kind of sucks. PROC_REAP_ACQUIRE "acquires the reaper status for
    // the current process" but does that mean the first two arguments still need valid values to be
    // supplied? We'll play it safe and explicitly target our own process:
    procctl(P_PID, getpid(), PROC_REAP_ACQUIRE, NULL);
#endif
    
    log_flush_timer.add_timer(event_loop, dasynq::clock_type::MONOTONIC);

    bool add_all_service_dirs = false;
    if (service_dir == nullptr) {
        service_dir = "/etc/dinit.d";
        add_all_service_dirs = true;
    }

    /* start requested services */
    services = new dirload_service_set(service_dir, service_dir_dynamic);
    if (add_all_service_dirs) {
        services->add_service_dir("/usr/local/lib/dinit.d", false);
        services->add_service_dir("/lib/dinit.d", false);
    }
    
    init_log(services, log_is_syslog);
    if (am_system_init) {
        log(loglevel_t::INFO, false, "starting system");
    }
    
    // Only try to set up the external log now if we aren't the system init. (If we are the
    // system init, wait until the log service starts).
    if (! am_system_init) setup_external_log();

    if (env_file != nullptr) {
        read_env_file(env_file);
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
            log(loglevel_t::ERROR, snf.serviceName, ": Could not find service description.");
        }
        catch (service_load_exc &sle) {
            log(loglevel_t::ERROR, sle.serviceName, ": ", sle.excDescription);
        }
        catch (std::bad_alloc &badalloce) {
            log(loglevel_t::ERROR, "Out of memory when trying to start service: ", svc, ".");
            break;
        }
    }
    
    event_loop:
    
    // Process events until all services have terminated.
    while (services->count_active_services() != 0) {
        event_loop.run();
    }

    shutdown_type_t shutdown_type = services->get_shutdown_type();
    
    if (am_pid_one) {
        log_msg_begin(loglevel_t::INFO, "No more active services.");
        
        if (shutdown_type == shutdown_type_t::REBOOT) {
            log_msg_end(" Will reboot.");
        }
        else if (shutdown_type == shutdown_type_t::HALT) {
            log_msg_end(" Will halt.");
        }
        else if (shutdown_type == shutdown_type_t::POWEROFF) {
            log_msg_end(" Will power down.");
        }
        else {
            log_msg_end(" Re-initiating boot sequence.");
        }
    }
    
    log_flush_timer.arm_timer_rel(event_loop, timespec{5,0}); // 5 seconds
    while (! is_log_flushed() && ! log_flush_timer.has_expired()) {
        event_loop.run();
    }
    
    close_control_socket();
    
    if (am_pid_one) {
        if (shutdown_type == shutdown_type_t::CONTINUE) {
            // It could be that we started in single user mode, and the
            // user has now exited the shell. We'll try and re-start the
            // boot process...
            try {
                services->start_service("boot");
                goto event_loop; // yes, the "evil" goto
            }
            catch (...) {
                // Now what do we do? try to reboot, but wait for user ack to avoid boot loop.
                log(loglevel_t::ERROR, "Could not start 'boot' service. Will attempt reboot.");
                wait_for_user_input();
                shutdown_type = shutdown_type_t::REBOOT;
            }
        }
        
        const char * cmd_arg;
        if (shutdown_type == shutdown_type_t::HALT) {
            cmd_arg = "-h";
        }
        else if (shutdown_type == shutdown_type_t::REBOOT) {
            cmd_arg = "-r";
        }
        else {
            // power off.
            cmd_arg = "-p";
        }
        
        // Fork and execute dinit-reboot.
        constexpr auto shutdown_exec = literal(SBINDIR) + "/shutdown";
        execl(shutdown_exec.c_str(), shutdown_exec.c_str(), "--system", cmd_arg, nullptr);
        log(loglevel_t::ERROR, (literal("Could not execute ") + SBINDIR + "/shutdown: ").c_str(),
                strerror(errno));
        
        // PID 1 must not actually exit, although we should never reach this point:
        while (true) {
            event_loop.run();
        }
    }
    else if (shutdown_type == shutdown_type_t::REBOOT) {
        // Non-system-process. If we got SIGINT, let's die due to it:
        sigset_t sigwait_set;
        sigemptyset(&sigwait_set);
        sigaddset(&sigwait_set, SIGINT);
        raise(SIGINT);
        sigprocmask(SIG_UNBLOCK, &sigwait_set, NULL);
    }
    
    return 0;
}

// Log a parse error when reading the environment file.
static void log_bad_env(int linenum)
{
    log(loglevel_t::ERROR, "invalid environment variable setting in environment file (line ", linenum, ")");
}

// Read and set environment variables from a file.
static void read_env_file(const char *env_file_path)
{
    // Note that we can't use the log in this function; it hasn't been initialised yet.

    std::ifstream env_file(env_file_path);
    if (! env_file) return;

    env_file.exceptions(std::ios::badbit);

    auto &clocale = std::locale::classic();
    std::string line;
    int linenum = 0;

    while (std::getline(env_file, line)) {
        linenum++;
        auto lpos = line.begin();
        auto lend = line.end();
        while (lpos != lend && std::isspace(*lpos, clocale)) {
            ++lpos;
        }

        if (lpos != lend) {
            if (*lpos != '#') {
                if (*lpos == '=') {
                    log_bad_env(linenum);
                    continue;
                }
                auto name_begin = lpos++;
                // skip until '=' or whitespace:
                while (lpos != lend && *lpos != '=' && ! std::isspace(*lpos, clocale)) ++lpos;
                auto name_end = lpos;
                //  skip whitespace:
                while (lpos != lend && std::isspace(*lpos, clocale)) ++lpos;
                if (lpos == lend) {
                    log_bad_env(linenum);
                    continue;
                }

                ++lpos;
                auto val_begin = lpos;
                while (lpos != lend && *lpos != '\n') ++lpos;
                auto val_end = lpos;

                std::string name = line.substr(name_begin - line.begin(), name_end - name_begin);
                std::string value = line.substr(val_begin - line.begin(), val_end - val_begin);
                if (setenv(name.c_str(), value.c_str(), true) == -1) {
                    throw std::system_error(errno, std::system_category());
                }
            }
        }
    }
}

// In exception situations we want user confirmation before proceeding (eg on critical boot failure
// we wait before rebooting to avoid a reboot loop).
static void wait_for_user_input() noexcept
{
    std::cout << "Press Enter to continue." << std::endl;
    char buf[1];
    read(STDIN_FILENO, buf, 1);
}

// Callback for control socket
static void control_socket_cb(eventloop_t *loop, int sockfd)
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
            log(loglevel_t::ERROR, "Accepting control connection: ", exc.what());
            close(newfd);
        }
    }
}

// Open/create the control socket, normally /dev/dinitctl, used to allow client programs to connect
// and issue service orders and shutdown commands etc. This can safely be called multiple times;
// once the socket has been successfully opened, further calls have no effect.
void open_control_socket(bool report_ro_failure) noexcept
{
    if (! control_socket_open) {
        const char * saddrname = control_socket_path;
        size_t saddrname_len = strlen(saddrname);
        uint sockaddr_size = offsetof(struct sockaddr_un, sun_path) + saddrname_len + 1;
        
        struct sockaddr_un * name = static_cast<sockaddr_un *>(malloc(sockaddr_size));
        if (name == nullptr) {
            log(loglevel_t::ERROR, "Opening control socket: out of memory");
            return;
        }

        if (am_system_init) {
            // Unlink any stale control socket file, but only if we are system init, since otherwise
            // the 'stale' file may not be stale at all:
            unlink(saddrname);
        }

        name->sun_family = AF_UNIX;
        memcpy(name->sun_path, saddrname, saddrname_len + 1);

        int sockfd = dinit_socket(AF_UNIX, SOCK_STREAM, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (sockfd == -1) {
            log(loglevel_t::ERROR, "Error creating control socket: ", strerror(errno));
            free(name);
            return;
        }

        if (bind(sockfd, (struct sockaddr *) name, sockaddr_size) == -1) {
            if (errno != EROFS || report_ro_failure) {
                log(loglevel_t::ERROR, "Error binding control socket: ", strerror(errno));
            }
            close(sockfd);
            free(name);
            return;
        }
        
        free(name);

        // No connections can be made until we listen, so it is fine to change the permissions now
        // (and anyway there is no way to atomically create the socket and set permissions):
        if (chmod(saddrname, S_IRUSR | S_IWUSR) == -1) {
            log(loglevel_t::ERROR, "Error setting control socket permissions: ", strerror(errno));
            close(sockfd);
            return;
        }

        if (listen(sockfd, 10) == -1) {
            log(loglevel_t::ERROR, "Error listening on control socket: ", strerror(errno));
            close(sockfd);
            return;
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
}

static void close_control_socket() noexcept
{
    if (control_socket_open) {
        int fd = control_socket_io.get_watched_fd();
        control_socket_io.deregister(event_loop);
        close(fd);
        
        // Unlink the socket:
        unlink(control_socket_path);
    }
}

void setup_external_log() noexcept
{
    if (! external_log_open) {
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
                // log failure to log? It makes more sense than first appears, because we also log to console:
                log(loglevel_t::ERROR, "Setting up log failed: ", strerror(errno));
            }
        }
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
    constexpr auto shutdown_exec = literal(SBINDIR) + "/shutdown";
    execl(shutdown_exec.c_str(), shutdown_exec.c_str(), "--system", (char *) 0);
    log(loglevel_t::ERROR, literal("Error executing ") + SBINDIR + "/sbin/shutdown: ", strerror(errno));
    sync(); // since a hard poweroff might be required at this point...
}

/* handle SIGTERM/SIGQUIT(non-system-daemon) - stop all services and shut down */
static void sigterm_cb(eventloop_t &eloop) noexcept
{
    services->stop_all_services();
}
