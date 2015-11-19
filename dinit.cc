#include <iostream>
#include <list>
#include <cstring>
#include <csignal>
#include <cstddef>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include "service.h"
#include "ev++.h"
#include "control.h"
#include "dinit-log.h"

#ifdef __linux__
#include <sys/klog.h>
#endif

/* TODO: prevent services from respawning too quickly */
/* TODO: optional automatic restart of services */

/*
 * "simpleinit" from util-linux package handles signals as follows:
 * SIGTSTP - spawn no more gettys (in preparation for shutdown etc).
 *          In dinit terms this should probably mean "no more auto restarts"
 *          (for any service). (Actually the signal acts as a toggle, if
 *          respawn is disabled it will be re-enabled and init will
 *          act as if SIGHUP had also been sent)
 * SIGTERM - kill spawned gettys (which are still alive)
 *          Interestingly, simpleinit just sends a SIGTERM to the gettys.
 *          "shutdown" however has already sent SIGTERM to every process...
 * "/sbin/initctl -r" - rollback services (ran by "shutdown"/halt etc)
 *           shouldn't return until all services have been stopped.
 *           shutdown calls this *after* sending SIGTERM to all processes.
 *           I guess this allows user processes, if any are still around,
 *           to die before (or just as) the services fall out from underneath
 *           them. On the other hand it largely subverts the ordered service
 *           shutdown that init provides.
 * SIGQUIT - init will exec() shutdown. shutdown will detect that it is
 *           running as pid 1 and will just loop and reap child processes.
 *           This is used by shutdown so that init will not hang on to its
 *           inode, allowing the filesystem to be re-mounted readonly
 *           (this is only an issue if the init binary has been unlinked,
 *           since it's then holding an inode which can't be maintained
 *           when the filesystem is unmounted).
 *
 * Not sent by shutdown:
 * SIGHUP -  re-read inittab and spawn any new getty entries
 * SIGINT - (ctrl+alt+del handler) - fork & exec "reboot"
 * 
 * On the contrary dinit currently uses:
 * SIGTERM - roll back services and then fork/exec /sbin/halt
 * SIGINT - roll back services and then fork/exec /sbin/reboot
 * SIGQUIT - exec() /sbin/shutdown as per above.
 *
 * It's an open question about whether dinit should roll back services *before*
 * running halt/reboot, since those commands should prompt rollback of services
 * anyway. But it seems safe to do so.
 */


static bool got_sigterm = false;

static ServiceSet *service_set;

static bool am_system_init = false; // true if we are the system init process
static bool do_reboot = false; // whether to reboot (instead of halting)

static bool control_socket_open = false;
int active_control_conns = 0;

static void sigint_reboot_cb(struct ev_loop *loop, ev_signal *w, int revents);
static void sigquit_cb(struct ev_loop *loop, ev_signal *w, int revents);
static void sigterm_cb(struct ev_loop *loop, ev_signal *w, int revents);

void open_control_socket(struct ev_loop *loop);

struct ev_io control_socket_io;


int main(int argc, char **argv)
{
    using namespace std;
    
    am_system_init = (getpid() == 1);
    
    if (am_system_init) {
        // setup STDIN, STDOUT, STDERR so that we can use them
        int onefd = open("/dev/console", O_RDONLY, 0);
        dup2(onefd, 0);
        int twofd = open("/dev/console", O_RDWR, 0);
        dup2(twofd, 1);
        dup2(twofd, 2);
    }
    
    /* Set up signal handlers etc */
    /* SIG_CHILD is ignored by default: good */
    /* sigemptyset(&sigwait_set); */
    /* sigaddset(&sigwait_set, SIGCHLD); */
    /* sigaddset(&sigwait_set, SIGINT); */
    /* sigaddset(&sigwait_set, SIGTERM); */
    /* sigprocmask(SIG_BLOCK, &sigwait_set, NULL); */
    
    /* list of services to start */
    list<const char *> services_to_start;
    
    /* service directory name */
    const char * service_dir = "/etc/dinit.d";
    
    // Arguments, if given, specify a list of services to start.
    // If we are running as init (PID=1), the kernel gives us any command line
    // arguments it was given but didn't recognize, including "single" (usually
    // for "boot to single user mode" aka just start the shell). We can treat
    // them as service names. In the worst case we can't find any of the named
    // services, and so we'll start the "boot" service by default.
    if (argc > 1) {
      for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            // An option...
            if (strcmp(argv[i], "--services-dir") == 0 ||
                    strcmp(argv[i], "-d") == 0) {
                ++i;
                if (i < argc) {
                    service_dir = argv[i];
                }
                else {
                    // error TODO
                }
            }
            else if (strcmp(argv[i], "--help") == 0) {
                cout << "dinit, an init with dependency management" << endl;
                cout << " --help                         : display help" << endl;
                cout << " --services-dir <dir>, -d <dir> : set base directory for service description files (-d <dir>)" << endl;
                cout << " <service-name>                 : start service with name <service-name>" << endl;
                return 0;
            }
            else {
                // unrecognized
                if (! am_system_init) {
                    cerr << "Unrecognized option: " << argv[i] << endl;
                    return 1;
                }
            }
        }
        else {
            // LILO puts "auto" on the kernel command line for unattended boots; we'll filter it.
            if (! am_system_init || strcmp(argv[i], "auto") != 0) {
                services_to_start.push_back(argv[i]);
            }
        }
      }
    }
    
    if (services_to_start.empty()) {
        services_to_start.push_back("boot");
    }

    // Set up signal handlers
    ev_signal sigint_ev_signal;
    if (am_system_init) {
      ev_signal_init(&sigint_ev_signal, sigint_reboot_cb, SIGINT);
    }
    else {
      ev_signal_init(&sigint_ev_signal, sigterm_cb, SIGINT);
    }
    
    ev_signal sigquit_ev_signal;
    if (am_system_init) {
        // PID 1: SIGQUIT exec's shutdown
        ev_signal_init(&sigquit_ev_signal, sigquit_cb, SIGQUIT);
    }
    else {
        // Otherwise: SIGQUIT terminates dinit
        ev_signal_init(&sigquit_ev_signal, sigterm_cb, SIGQUIT);
    }
    
    ev_signal sigterm_ev_signal;
    ev_signal_init(&sigterm_ev_signal, sigterm_cb, SIGTERM);
    
    /* Set up libev */
    struct ev_loop *loop = ev_default_loop(EVFLAG_AUTO /* | EVFLAG_SIGNALFD */);
    ev_signal_start(loop, &sigint_ev_signal);
    ev_signal_start(loop, &sigquit_ev_signal);
    ev_signal_start(loop, &sigterm_ev_signal);

    // Try to open control socket (may fail due to readonly filesystem)
    open_control_socket(loop);
    
#ifdef __linux__
    if (am_system_init) {
        // Disable non-critical kernel output to console
        klogctl(6 /* SYSLOG_ACTION_CONSOLE_OFF */, nullptr, 0);
    }
#endif
    
    /* start requested services */
    service_set = new ServiceSet(service_dir);
    for (list<const char *>::iterator i = services_to_start.begin();
            i != services_to_start.end();
            ++i) {
        try {
            service_set->startService(*i);
        }
        catch (ServiceNotFound &snf) {
            log(LogLevel::ERROR, "Could not find service description: ", snf.serviceName);
        }
        catch (ServiceLoadExc &sle) {
            log(LogLevel::ERROR, "Problem loading service description: ", sle.serviceName);
        }
    }
    
    event_loop:
    
    // Process events until all services have terminated.
    while (service_set->count_active_services() != 0 || active_control_conns != 0) {
        ev_loop(loop, EVLOOP_ONESHOT);
    }
    
    if (am_system_init) {
        logMsgBegin(LogLevel::INFO, "No more active services.");
        if (do_reboot) {
            logMsgEnd(" Will reboot.");
        }
        else if (got_sigterm) {
            logMsgEnd(" Will halt.");
        }
        else {
            logMsgEnd(" Re-initiating boot sequence.");
        }
    }
    
    if (am_system_init) {
        if (do_reboot) {
            // TODO log error from fork
            if (fork() == 0) {
                execl("/sbin/reboot", "/sbin/reboot", (char *) 0);
            }
        }
        else if (got_sigterm) {
            // TODO log error from fork
            if (fork() == 0) {
                execl("/sbin/halt", "/sbin/halt", (char *) 0);
            }
        }
        else {
            // Hmmmmmm.
            // It could be that we started in single user mode, and the
            // user has now exited the shell. We'll try and re-start the
            // boot process...
            try {
                service_set->startService("boot");
                goto event_loop; // yes, the "evil" goto
            }
            catch (...) {
                // Now WTF do we do? try to reboot
                log(LogLevel::ERROR, "Could not start 'boot' service; rebooting.");
                if (fork() == 0) {
                    execl("/sbin/reboot", "/sbin/reboot", (char *) 0);
                }
            }
        }
        
        // PID 1 should never exit:
        while (true) {
            pause();
        }
    }
    
    return 0;
}

// Callback for control socket
static void control_socket_cb(struct ev_loop *loop, ev_io *w, int revents)
{
    // TODO limit the number of active connections. Keep a tally, and disable the
    // control connection listening socket watcher if it gets high, and re-enable
    // it once it falls below the maximum.

    // Accept a connection
    int sockfd = w->fd;

    int newfd = accept4(sockfd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (newfd != -1) {
        try {
            new ControlConn(loop, service_set, newfd);  // will delete itself when it's finished
        }
        catch (std::bad_alloc &bad_alloc_exc) {
            log(LogLevel::ERROR, "Accepting control connection: Out of memory");
            close(newfd);
        }
    }
}

void open_control_socket(struct ev_loop *loop)
{
    if (! control_socket_open) {
        // TODO make this use a per-user address if PID != 1, and make the address
        // overridable from the command line

        const char * saddrname = "/dev/dinitctl";
        struct sockaddr_un name;

        if (am_system_init) {
            unlink(saddrname);
        }

        name.sun_family = AF_UNIX;
        strcpy(name.sun_path, saddrname); // TODO make this safe for long names
        int namelen = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(saddrname);

        int sockfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (sockfd == -1) {
            log(LogLevel::ERROR, "Error creating control socket: ", strerror(errno));
            return;
        }

        if (bind(sockfd, (struct sockaddr *) &name, namelen) == -1) {
            log(LogLevel::ERROR, "Error binding control socket: ", strerror(errno));
            close(sockfd);
            return;
        }

        // No connections can be made until we listen, so it is fine to change the permissions now
        // (and anyway there is no way to atomically create the socket and set permissions):
        if (chmod(saddrname, S_IRUSR | S_IWUSR) == -1) {
            log(LogLevel::ERROR, "Error setting control socket permissions: ", strerror(errno));
            close(sockfd);
            return;
        }

        if (listen(sockfd, 10) == -1) {
            log(LogLevel::ERROR, "Error listening on control socket: ", strerror(errno));
            close(sockfd);
            return;
        }

        control_socket_open = true;
        ev_io_init(&control_socket_io, control_socket_cb, sockfd, EV_READ);
        ev_io_start(loop, &control_socket_io);
    }
}

/* handle SIGINT signal (generated by kernel when ctrl+alt+del pressed) */
static void sigint_reboot_cb(struct ev_loop *loop, ev_signal *w, int revents)
{
    do_reboot = true;
    log_to_console = true;
    service_set->stop_all_services();
}

/* handle SIGQUIT (if we are system init) */
static void sigquit_cb(struct ev_loop *loop, ev_signal *w, int revents)
{
    // This allows remounting the filesystem read-only if the dinit binary has been
    // unlinked. In that case the kernel holds the binary open, so that it can't be
    // properly removed.
    execl("/sbin/shutdown", "/sbin/shutdown", (char *) 0);
    log(LogLevel::ERROR, "Error executing /sbin/shutdown: ", strerror(errno));
}

/* handle SIGTERM - stop all services */
static void sigterm_cb(struct ev_loop *loop, ev_signal *w, int revents)
{
    got_sigterm = true;
    log_to_console = true;
    service_set->stop_all_services();
}
