#include <cstdio>
#include <cstddef>
#include <cstring>
#include <string>
#include <iostream>
#include <fstream>
#include <system_error>
#include <memory>
#include <algorithm>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>
#include <pwd.h>

#include "control-cmds.h"
#include "service-constants.h"
#include "cpbuffer.h"
#include "dinit-client.h"
#include "load-service.h"
#include "dinit-util.h"
#include "mconfig.h"

// dinitctl:  utility to control the Dinit daemon, including starting and stopping of services.

// This utility communicates with the dinit daemon via a unix stream socket (as specified in
// SYSCONTROLSOCKET, or $HOME/.dinitctl).

static constexpr uint16_t min_cp_version = 1;
static constexpr uint16_t max_cp_version = 1;

enum class command_t;

static int issue_load_service(int socknum, const char *service_name, bool find_only = false);
static int check_load_reply(int socknum, cpbuffer_t &, handle_t *handle_p, service_state_t *state_p,
        bool write_error=true);
static int start_stop_service(int socknum, cpbuffer_t &, const char *service_name, command_t command,
        bool do_pin, bool do_force, bool wait_for_service, bool ignore_unstarted, bool verbose);
static int unpin_service(int socknum, cpbuffer_t &, const char *service_name, bool verbose);
static int unload_service(int socknum, cpbuffer_t &, const char *service_name, bool verbose);
static int reload_service(int socknum, cpbuffer_t &, const char *service_name, bool verbose);
static int list_services(int socknum, cpbuffer_t &);
static int service_status(int socknum, cpbuffer_t &rbuffer, const char *service_name, command_t command, bool verbose);
static int shutdown_dinit(int soclknum, cpbuffer_t &, bool verbose);
static int add_remove_dependency(int socknum, cpbuffer_t &rbuffer, bool add, const char *service_from,
        const char *service_to, dependency_type dep_type, bool verbose);
static int enable_disable_service(int socknum, cpbuffer_t &rbuffer, const char *from, const char *to,
        bool enable, bool verbose);
static int do_setenv(int socknum, cpbuffer_t &rbuffer, std::vector<const char *> &env_names);
static int trigger_service(int socknum, cpbuffer_t &rbuffer, const char *service_name, bool trigger_value);
static int cat_service_log(int socknum, cpbuffer_t &rbuffer, const char *service_name);

static const char * describeState(bool stopped)
{
    return stopped ? "stopped" : "started";
}

static const char * describeVerb(bool stop)
{
    return stop ? "stop" : "start";
}

enum class command_t {
    NONE,
    START_SERVICE,
    WAKE_SERVICE,
    STOP_SERVICE,
    RESTART_SERVICE,
    RELEASE_SERVICE,
    UNPIN_SERVICE,
    UNLOAD_SERVICE,
    RELOAD_SERVICE,
    LIST_SERVICES,
    SERVICE_STATUS,
    SHUTDOWN,
    ADD_DEPENDENCY,
    RM_DEPENDENCY,
    ENABLE_SERVICE,
    DISABLE_SERVICE,
    SETENV,
    SET_TRIGGER,
    UNSET_TRIGGER,
    CAT_LOG,
    IS_ACTIVE,
    IS_FAILED,
};

class dinit_protocol_error
{
    // no body
};

// Entry point.
int dinitctl_main(int argc, char **argv)
{
    using namespace std;
    
    bool show_help = argc < 2;
    const char *service_name = nullptr;
    const char *to_service_name = nullptr;
    dependency_type dep_type = dependency_type::AFTER; // avoid maybe-uninitialised warning
    bool dep_type_set = false;
    
    std::string control_socket_str;
    const char * control_socket_path = nullptr;
    
    bool verbose = true;
    bool user_dinit = (getuid() != 0);  // communicate with user daemon
    bool wait_for_service = true;
    bool do_pin = false;
    bool do_force = false;
    bool ignore_unstarted = false;
    bool use_passed_cfd = false;
    
    command_t command = command_t::NONE;

    std::vector<const char *> cmd_args;
        
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "--help") == 0) {
                show_help = true;
                break;
            }
            else if (strcmp(argv[i], "--version") == 0) {
                cout << "Dinit version " << DINIT_VERSION << ".\n";
                return 0;
            }
            else if (strcmp(argv[i], "--no-wait") == 0) {
                wait_for_service = false;
            }
            else if (strcmp(argv[i], "--ignore-unstarted") == 0) {
                ignore_unstarted = true;
            }
            else if (strcmp(argv[i], "--quiet") == 0) {
                verbose = false;
            }
            else if (strcmp(argv[i], "--system") == 0 || strcmp(argv[i], "-s") == 0) {
                user_dinit = false;
            }
            else if (strcmp(argv[i], "--user") == 0 || strcmp(argv[i], "-u") == 0) {
                user_dinit = true;
            }
            else if (strcmp(argv[i], "--pin") == 0) {
                do_pin = true;
            }
            else if (strcmp(argv[i], "--socket-path") == 0 || strcmp(argv[i], "-p") == 0) {
                ++i;
                if (i == argc) {
                    cerr << "dinitctl: --socket-path/-p should be followed by socket path" << std::endl;
                    return 1;
                }
                control_socket_str = argv[i];
            }
            else if (strcmp(argv[i], "--use-passed-cfd") == 0) {
                use_passed_cfd = true;
            }
            else if ((command == command_t::ENABLE_SERVICE || command == command_t::DISABLE_SERVICE)
                    && strcmp(argv[i], "--from") == 0) {
                ++i;
                if (i == argc) {
                    cerr << "dinitctl: --from should be followed by a service name" << std::endl;
                    return 1;
                }
                service_name = argv[i];
            }
            else if ((command == command_t::STOP_SERVICE || command == command_t::RESTART_SERVICE)
                    && (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0)) {
                do_force = true;
            }
            else {
                cerr << "dinitctl: unrecognized/invalid option: " << argv[i] << " (use --help for help)\n";
                return 1;
            }
        }
        else if (command == command_t::NONE) {
            if (strcmp(argv[i], "start") == 0) {
                command = command_t::START_SERVICE; 
            }
            else if (strcmp(argv[i], "wake") == 0) {
                command = command_t::WAKE_SERVICE;
            }
            else if (strcmp(argv[i], "stop") == 0) {
                command = command_t::STOP_SERVICE;
            }
            else if (strcmp(argv[i], "restart") == 0) {
                command = command_t::RESTART_SERVICE;
            }
            else if (strcmp(argv[i], "release") == 0) {
                command = command_t::RELEASE_SERVICE;
            }
            else if (strcmp(argv[i], "unpin") == 0) {
                command = command_t::UNPIN_SERVICE;
            }
            else if (strcmp(argv[i], "unload") == 0) {
                command = command_t::UNLOAD_SERVICE;
            }
            else if (strcmp(argv[i], "reload") == 0) {
                command = command_t::RELOAD_SERVICE;
            }
            else if (strcmp(argv[i], "list") == 0) {
                command = command_t::LIST_SERVICES;
            }
            else if (strcmp(argv[i], "status") == 0) {
                command = command_t::SERVICE_STATUS;
            }
            else if (strcmp(argv[i], "is-active") == 0) {
                command = command_t::IS_ACTIVE;
            }
            else if (strcmp(argv[i], "is-failed") == 0) {
                command = command_t::IS_FAILED;
            }
            else if (strcmp(argv[i], "shutdown") == 0) {
                command = command_t::SHUTDOWN;
            }
            else if (strcmp(argv[i], "add-dep") == 0) {
                command = command_t::ADD_DEPENDENCY;
            }
            else if (strcmp(argv[i], "rm-dep") == 0) {
                command = command_t::RM_DEPENDENCY;
            }
            else if (strcmp(argv[i], "enable") == 0) {
                command = command_t::ENABLE_SERVICE;
            }
            else if (strcmp(argv[i], "disable") == 0) {
                command = command_t::DISABLE_SERVICE;
            }
            else if (strcmp(argv[i], "setenv") == 0) {
                command = command_t::SETENV;
            }
            else if (strcmp(argv[i], "trigger") == 0) {
                command = command_t::SET_TRIGGER;
            }
            else if (strcmp(argv[i], "untrigger") == 0) {
                command = command_t::UNSET_TRIGGER;
            }
            else if (strcmp(argv[i], "catlog") == 0) {
            	command = command_t::CAT_LOG;
            }
            else {
                cerr << "dinitctl: unrecognized command: " << argv[i] << " (use --help for help)\n";
                return 1;
            }
        }
        else {
            // service name / other non-option
            if (command == command_t::ADD_DEPENDENCY || command == command_t::RM_DEPENDENCY) {
                if (! dep_type_set) {
                    if (strcmp(argv[i], "regular") == 0) {
                    	dep_type = dependency_type::REGULAR;
                    }
                    else if (strcmp(argv[i], "milestone") == 0) {
                    	dep_type = dependency_type::MILESTONE;
                    }
                    else if (strcmp(argv[i], "waits-for") == 0) {
                    	dep_type = dependency_type::WAITS_FOR;
                    }
                    else {
                    	show_help = true;
                    	break;
                    }
                    dep_type_set = true;
                }
                else if (service_name == nullptr) {
                    service_name = argv[i];
                }
                else if (to_service_name == nullptr) {
                    to_service_name = argv[i];
                }
                else {
                    show_help = true;
                    break;
                }
            }
            else if (command == command_t::ENABLE_SERVICE || command == command_t::DISABLE_SERVICE) {
                if (to_service_name != nullptr) {
                    show_help = true;
                    break;
                }
                to_service_name = argv[i];
            }
            else {
                cmd_args.push_back(argv[i]);
            }
        }
    }
    
    // Additional argument checks for various commands:

    if (command == command_t::NONE) {
        show_help = true;
    }
    else if (command == command_t::ENABLE_SERVICE || command == command_t::DISABLE_SERVICE) {
        show_help |= (to_service_name == nullptr);
    }
    else if (command == command_t::SETENV) {
        // Handle SETENV specially, since it needs arguments but they are not service names
        if (cmd_args.empty()) {
            show_help = true;
        }
    }
    else {
        bool no_service_cmd = (command == command_t::LIST_SERVICES || command == command_t::SHUTDOWN);
        if (no_service_cmd) {
            if (!cmd_args.empty()) {
                show_help = true;
            }
        }
        else {
            if (command == command_t::ADD_DEPENDENCY || command == command_t::RM_DEPENDENCY) {
                if (! dep_type_set || service_name == nullptr || to_service_name == nullptr) {
                    show_help = true;
                }
            }
            else if (cmd_args.empty()) {
                show_help = true;
            }
            else {
                // No command can currently accept more than one service argument:
                if (cmd_args.size() > 1) {
                    show_help = true;
                }
                service_name = cmd_args.front();
            }
        }
    }

    if (show_help) {
        cout << "dinitctl:   control Dinit services\n"
          "\n"
          "Usage:\n"
          "    dinitctl [options] status <service-name>\n"
          "    dinitctl [options] is-active <service-name>\n"
          "    dinitctl [options] is-failed <service-name>\n"
          "    dinitctl [options] start [options] <service-name>\n"
          "    dinitctl [options] stop [options] <service-name>\n"
          "    dinitctl [options] restart [options] <service-name>\n"
          "    dinitctl [options] wake [options] <service-name>\n"
          "    dinitctl [options] release [options] <service-name>\n"
          "    dinitctl [options] unpin <service-name>\n"
          "    dinitctl [options] unload <service-name>\n"
          "    dinitctl [options] reload <service-name>\n"
          "    dinitctl [options] list\n"
          "    dinitctl [options] shutdown\n"
          "    dinitctl [options] add-dep <type> <from-service> <to-service>\n"
          "    dinitctl [options] rm-dep <type> <from-service> <to-service>\n"
          "    dinitctl [options] enable [--from <from-service>] <to-service>\n"
          "    dinitctl [options] disable [--from <from-service>] <to-service>\n"
          "    dinitctl [options] trigger <service-name>\n"
          "    dinitctl [options] untrigger <service-name>\n"
          "    dinitctl [options] setenv [name[=value] ...]\n"
          "    dinitctl [options] catlog <service-name>\n"
          "\n"
          "Note: An activated service continues running when its dependents stop.\n"
          "\n"
          "General options:\n"
          "  --help           : show this help\n"
          "  -s, --system     : control system daemon (default if run as root)\n"
          "  -u, --user       : control user daemon\n"
          "  --quiet          : suppress output (except errors)\n"
          "  --socket-path <path>, -p <path>\n"
          "                   : specify socket for communication with daemon\n"
          "  --use-passed-cfd : use the socket file descriptor identified by the DINIT_CS_FD\n"
          "                     environment variable to communicate with the dinit daemon.\n"
          "\n"
          "Command options:\n"
          "  --no-wait        : don't wait for service startup/shutdown to complete\n"
          "  --pin            : pin the service in the requested state\n"
          "  --force          : force stop even if dependents will be affected\n";
        return 1;
    }
    
    // Begin the real work: connect to dinit

    signal(SIGPIPE, SIG_IGN);
    
    int socknum = -1;

    if (use_passed_cfd) {
        socknum = get_passed_cfd();
        if (socknum == -1) {
            use_passed_cfd = false;
        }
    }

    if (!use_passed_cfd) {
        // Locate control socket
        if (!control_socket_str.empty()) {
            control_socket_path = control_socket_str.c_str();
        }
        else {
            control_socket_path = get_default_socket_path(control_socket_str, user_dinit);
            if (control_socket_path == nullptr) {
                cerr << "dinitctl: cannot locate user home directory (set XDG_RUNTIME_DIR, HOME, check /etc/passwd file, or "
                        "specify socket path via -p)" << endl;
                return 1;
            }
        }
    }
    
    try {
        if (!use_passed_cfd) {
            socknum = connect_to_daemon(control_socket_path);
        }

        // Start by querying protocol version:
        cpbuffer_t rbuffer;
        uint16_t daemon_protocol_ver = check_protocol_version(min_cp_version, max_cp_version, rbuffer, socknum);

        if (command == command_t::UNPIN_SERVICE) {
            return unpin_service(socknum, rbuffer, service_name, verbose);
        }
        else if (command == command_t::UNLOAD_SERVICE) {
            return unload_service(socknum, rbuffer, service_name, verbose);
        }
        else if (command == command_t::RELOAD_SERVICE) {
            return reload_service(socknum, rbuffer, service_name, verbose);
        }
        else if (command == command_t::LIST_SERVICES) {
            return list_services(socknum, rbuffer);
        }
        else if (command == command_t::SERVICE_STATUS || command == command_t::IS_ACTIVE || command == command_t::IS_FAILED) {
            return service_status(socknum, rbuffer, service_name, command, verbose);
        }
        else if (command == command_t::SHUTDOWN) {
            return shutdown_dinit(socknum, rbuffer, verbose);
        }
        else if (command == command_t::ADD_DEPENDENCY || command == command_t::RM_DEPENDENCY) {
            return add_remove_dependency(socknum, rbuffer, command == command_t::ADD_DEPENDENCY,
                    service_name, to_service_name, dep_type, verbose);
        }
        else if (command == command_t::ENABLE_SERVICE || command == command_t::DISABLE_SERVICE) {
            // If only one service specified, assume that we enable for 'boot' service:
            if (service_name == nullptr) {
                service_name = "boot";
            }
            return enable_disable_service(socknum, rbuffer, service_name, to_service_name,
                    command == command_t::ENABLE_SERVICE, verbose);
        }
        else if (command == command_t::SETENV) {
            return do_setenv(socknum, rbuffer, cmd_args);
        }
        else if (command == command_t::SET_TRIGGER || command == command_t::UNSET_TRIGGER) {
            if (daemon_protocol_ver < 2) {
                throw cp_old_server_exception();
            }
            return trigger_service(socknum, rbuffer, service_name, (command == command_t::SET_TRIGGER));
        }
        else if (command == command_t::CAT_LOG) {
            if (daemon_protocol_ver < 2) {
                throw cp_old_server_exception();
            }
            return cat_service_log(socknum, rbuffer, service_name);
        }
        else {
            return start_stop_service(socknum, rbuffer, service_name, command, do_pin, do_force,
                    wait_for_service, ignore_unstarted, verbose);
        }
    }
    catch (cp_old_client_exception &e) {
        std::cerr << "dinitctl: too old (server reports newer protocol version)" << std::endl;
    }
    catch (cp_old_server_exception &e) {
        std::cerr << "dinitctl: server too old or protocol error" << std::endl;
    }
    catch (cp_read_exception &e) {
        cerr << "dinitctl: control socket read failure or protocol error" << endl;
    }
    catch (cp_write_exception &e) {
        cerr << "dinitctl: control socket write error: " << std::strerror(e.errcode) << endl;
    }
    catch (dinit_protocol_error &e) {
        cerr << "dinitctl: protocol error" << endl;
    }
    catch (general_error &ge) {
        std::cerr << "dinit-client";
        if (ge.get_action() != nullptr) {
            std::cerr << ": " << ge.get_action();
            std::string &arg = ge.get_arg();
            if (!arg.empty()) {
                std::cerr << " " << arg;
            }
        }
        if (ge.get_err() != 0) {
            std::cerr << ": " << strerror(ge.get_err());
        }
        std::cerr << '\n';
    }
    return 1;
}

int main(int argc, char **argv)
{
    try {
        return dinitctl_main(argc, argv);
    }
    catch (std::bad_alloc &e) {
        std::cerr << "dinitctl: out of memory\n";
    }
    return 1;
}

// Size of service status info (in various packets)
constexpr static unsigned STATUS_BUFFER_SIZE = 6 + ((sizeof(pid_t) > sizeof(int)) ? sizeof(pid_t) : sizeof(int));

// Extract/read a string of specified length from the buffer/socket. The string is consumed
// from the buffer.
static std::string read_string(int socknum, cpbuffer_t &rbuffer, uint32_t length)
{
    int rb_len = rbuffer.get_length();
    if (uint32_t(rb_len) >= length) {
        std::string r = rbuffer.extract_string(0, length);
        rbuffer.consume(length);
        return r;
    }

    std::string r = rbuffer.extract_string(0, rb_len);
    uint32_t rlen = length - rb_len;
    uint32_t clen;
    do {
        rbuffer.reset();
        fill_some(rbuffer, socknum);
        char *bptr = rbuffer.get_ptr(0);
        clen = rbuffer.get_length();
        clen = std::min(clen, rlen);
        r.append(bptr, clen);
        rlen -= clen;
    } while (rlen > 0);

    rbuffer.consume(clen);

    return r;
}

// Load a service: issue load command, wait for reply. Return true on success, display error message
// and return false on failure.
//      socknum  - the socket fd to communicate via
//      rbuffer  - the buffer for communication
//      name     - the name of the service to load
//      handle   - where to store the handle of the loaded service
//      state    - where to store the state of the loaded service (may be null).
//      write_error - whether to write an error message if the service can't be loaded
static bool load_service(int socknum, cpbuffer_t &rbuffer, const char *name, handle_t *handle,
        service_state_t *state, bool write_error=true)
{
    // Load 'to' service:
    if (issue_load_service(socknum, name)) {
        return false;
    }

    wait_for_reply(rbuffer, socknum);

    if (check_load_reply(socknum, rbuffer, handle, state, write_error) != 0) {
        return false;
    }

    return true;
}

// Get the service name for a given handle, by querying the daemon.
static std::string get_service_name(int socknum, cpbuffer_t &rbuffer, handle_t handle)
{
    auto m = membuf()
            .append((char) DINIT_CP_QUERYSERVICENAME)
            .append((char) 0)
            .append(handle);
    write_all_x(socknum, m);

    wait_for_reply(rbuffer, socknum);

    if (rbuffer[0] != DINIT_RP_SERVICENAME) {
        throw cp_read_exception{0};
    }

    // 1 byte reserved
    // uint16_t size
    fill_buffer_to(rbuffer, socknum, 2 + sizeof(uint16_t));
    uint16_t namesize;
    rbuffer.extract(&namesize, 2, sizeof(uint16_t));
    rbuffer.consume(2 + sizeof(uint16_t));

    std::string name;

    do {
        if (rbuffer.get_length() == 0) {
            fill_some(rbuffer, socknum);
        }

        size_t to_extract = std::min(size_t(rbuffer.get_length()), namesize - name.length());
        size_t contiguous_len = rbuffer.get_contiguous_length(rbuffer.get_ptr(0));
        if (contiguous_len <= to_extract) {
            name.append(rbuffer.get_ptr(0), contiguous_len);
            rbuffer.consume(contiguous_len);
            name.append(rbuffer.get_ptr(0), to_extract - contiguous_len);
            rbuffer.consume(to_extract - contiguous_len);
        }
        else {
            name.append(rbuffer.get_ptr(0), to_extract);
            rbuffer.consume(to_extract);
            break;
        }

    } while (name.length() < namesize);

    return name;
}

static void print_termination_details(int exit_status)
{
    using namespace std;

    if (WIFSIGNALED(exit_status)) {
        cout << "signalled - signal ";
        cout << WTERMSIG(exit_status);
    }
    else if (WIFEXITED(exit_status)) {
        cout << "exited - status ";
        cout << WEXITSTATUS(exit_status);
    }
    else {
        cout << "unknown reason";
    }
}

// Wait for a service to reached stopped (do_stop == true) or started (do_stop == false) state.
// Returns 0 if the service started/stopped, 1 if start/stop was cancelled or failed.
static int wait_service_state(int socknum, cpbuffer_t &rbuffer, handle_t handle,
        const std::string &service_name, bool do_stop, bool verbose)
{
    using std::cout;
    using std::cerr;
    using std::endl;

    service_event_t completionEvent;
    service_event_t cancelledEvent;

    if (do_stop) {
        completionEvent = service_event_t::STOPPED;
        cancelledEvent = service_event_t::STOPCANCELLED;
    }
    else {
        completionEvent = service_event_t::STARTED;
        cancelledEvent = service_event_t::STARTCANCELLED;
    }

    // Wait until service started:
    int r = rbuffer.fill_to(socknum, 2);
    while (r > 0) {
        if (rbuffer[0] >= 100) {
            unsigned pktlen = (unsigned char) rbuffer[1];
            fill_buffer_to(rbuffer, socknum, pktlen);

            if (rbuffer[0] == DINIT_IP_SERVICEEVENT) {
                // earlier versions do not include status info, the size in that case is base_pkt_size:
                constexpr unsigned base_pkt_size = 2 + sizeof(handle_t) + 1;
                if (pktlen < base_pkt_size) {
                    throw dinit_protocol_error();
                }
                handle_t ev_handle;
                rbuffer.extract((char *) &ev_handle, 2, sizeof(ev_handle));
                service_event_t event = static_cast<service_event_t>(rbuffer[2 + sizeof(ev_handle)]);
                if (ev_handle == handle) {
                    if (event == completionEvent) {
                        if (verbose) {
                            cout << "Service '" << service_name << "' " << describeState(do_stop) << ".\n";
                        }
                        return 0;
                    }
                    else if (event == cancelledEvent) {
                        if (verbose) {
                            cout << "Service '" << service_name << "' " << describeVerb(do_stop) << " cancelled.\n";
                        }
                        return 1;
                    }
                    else if (!do_stop && event == service_event_t::FAILEDSTART) {
                        if (verbose) {
                            cout << "Service '" << service_name << "' failed to start.\n";
                            if (pktlen >= base_pkt_size + STATUS_BUFFER_SIZE) {
                                stopped_reason_t stop_reason =
                                        static_cast<stopped_reason_t>(rbuffer[base_pkt_size + 3]);
                                int exit_status;
                                rbuffer.extract((char *)&exit_status, base_pkt_size + 6, sizeof(exit_status));

                                switch (stop_reason) {
                                case stopped_reason_t::DEPFAILED:
                                    cout << "Reason: a dependency of the service failed to start. Check dinit log.\n";
                                    break;
                                case stopped_reason_t::TIMEDOUT:
                                    cout << "Reason: start timed out.\n";
                                    break;
                                case stopped_reason_t::EXECFAILED:
                                    cout << "Reason: execution of service process failed:\n";
                                    uint16_t launch_stage;
                                    rbuffer.extract((char *)&launch_stage, base_pkt_size + 4, sizeof(uint16_t));
                                    cout << "        Stage: " << exec_stage_descriptions[launch_stage] << "\n";
                                    cout << "        Error: " << strerror(exit_status) << "\n";
                                    break;
                                case stopped_reason_t::FAILED:
                                    cout << "Reason: service process terminated before ready: ";
                                    print_termination_details(exit_status);
                                    cout << "\n";
                                    break;
                                default:
                                    cout << "Reason unknown/unrecognised. Check dinit log.\n";
                                }
                            }
                        }
                        return 1;
                    }
                }
            }

            rbuffer.consume(pktlen);
            r = rbuffer.fill_to(socknum, 2);
        }
        else {
            // Not an information packet?
            throw dinit_protocol_error();
        }
    }

    if (r == -1) {
        perror("dinitctl: read");
    }
    else {
        throw dinit_protocol_error();
    }

    return 1;
}

// Start/stop a service
static int start_stop_service(int socknum, cpbuffer_t &rbuffer, const char *service_name,
        command_t command, bool do_pin, bool do_force, bool wait_for_service, bool ignore_unstarted,
        bool verbose)
{
    using namespace std;

    bool do_stop = (command == command_t::STOP_SERVICE || command == command_t::RELEASE_SERVICE);

    service_state_t state;
    handle_t handle;
    
    if (command != command_t::RESTART_SERVICE && command != command_t::STOP_SERVICE
            && command != command_t::RELEASE_SERVICE) {
        ignore_unstarted = false;
    }

    if (!load_service(socknum, rbuffer, service_name, &handle, &state, !ignore_unstarted)) {
        return ignore_unstarted ? 0 : 1;
    }

    service_state_t wanted_state = do_stop ? service_state_t::STOPPED : service_state_t::STARTED;
    int pcommand = 0;
    switch (command) {
        case command_t::STOP_SERVICE:
        case command_t::RESTART_SERVICE:  // stop, and then start
            pcommand = DINIT_CP_STOPSERVICE;
            break;
        case command_t::RELEASE_SERVICE:
            pcommand = DINIT_CP_RELEASESERVICE;
            break;
        case command_t::START_SERVICE:
            pcommand = DINIT_CP_STARTSERVICE;
            break;
        case command_t::WAKE_SERVICE:
            pcommand = DINIT_CP_WAKESERVICE;
            break;
        default: ;
    }

    // Need to issue STOPSERVICE/STARTSERVICE
    // We'll do this regardless of the current service state / target state, since issuing
    // start/stop also sets or clears the "explicitly started" flag on the service.
    {
        char flags = (do_pin ? 1 : 0) | ((pcommand == DINIT_CP_STOPSERVICE && !do_force) ? 2 : 0);
        if (command == command_t::RESTART_SERVICE) {
            flags |= 4;
        }

        auto m = membuf()
                .append((char) pcommand)
                .append(flags)
                .append(handle);
        write_all_x(socknum, m);

        wait_for_reply(rbuffer, socknum);
        auto reply_pkt_h = rbuffer[0];
        rbuffer.consume(1); // consume header
        if (reply_pkt_h == DINIT_RP_ALREADYSS) {
            bool already = (state == wanted_state);
            if (verbose) {
                cout << "Service " << (already ? "(already) " : "")
                        << describeState(do_stop) << "." << endl;
            }
            return 0; // success!
        }
        if (reply_pkt_h == DINIT_RP_PINNEDSTARTED) {
            cerr << "dinitctl: cannot stop service '" << service_name << "' as it is pinned started\n";
            return 1;
        }
        if (reply_pkt_h == DINIT_RP_PINNEDSTOPPED) {
            cerr << "dinitctl: cannot start service '" << service_name << "' as it is pinned stopped\n";
            return 1;
        }
        if (reply_pkt_h == DINIT_RP_DEPENDENTS && pcommand == DINIT_CP_STOPSERVICE) {
            cerr << "dinitctl: cannot stop service '" << service_name << "' due to the following dependents:\n";
            if (command != command_t::RESTART_SERVICE) {
                cerr << "(only direct dependents are listed. Exercise caution before using '--force' !!)\n";
            }
            // size_t number, N * handle_t handles
            size_t number;
            rbuffer.fill_to(socknum, sizeof(number));
            rbuffer.extract(&number, 0, sizeof(number));
            rbuffer.consume(sizeof(number));
            std::vector<handle_t> handles;
            handles.reserve(number);
            for (size_t i = 0; i < number; i++) {
                handle_t handle;
                rbuffer.fill_to(socknum, sizeof(handle_t));
                rbuffer.extract(&handle, 0, sizeof(handle));
                handles.push_back(handle);
                rbuffer.consume(sizeof(handle));
            }
            // Print the directly affected dependents:
            cerr << " ";
            for (handle_t handle : handles) {
                cerr << " " << get_service_name(socknum, rbuffer, handle);
            }
            cerr << "\n";
            return 1;
        }
        if (reply_pkt_h == DINIT_RP_NAK && command == command_t::RESTART_SERVICE) {
            if (ignore_unstarted) {
                if (verbose) {
                    cout << "Service '" << service_name << "' is not currently started.\n";
                }
                return 0;
            }
            cerr << "dinitctl: cannot restart service; service not started.\n";
            return 1;
        }
        if (reply_pkt_h == DINIT_RP_NAK && command == command_t::WAKE_SERVICE) {
            cerr << "dinitctl: service has no active dependents, cannot wake.\n";
            return 1;
        }
        if (reply_pkt_h == DINIT_RP_SHUTTINGDOWN) {
            cerr << "dinitctl: cannot start/restart/wake service, shutdown is in progress.\n";
            return 1;
        }
        if (reply_pkt_h != DINIT_RP_ACK && reply_pkt_h != DINIT_RP_ALREADYSS) {
            cerr << "dinitctl: protocol error." << endl;
            return 1;
        }
    }

    if (! wait_for_service) {
        if (verbose) {
            cout << "Issued " << describeVerb(do_stop) << " command successfully for service '"
                    <<  service_name <<  "'." << endl;
        }
        return 0;
    }

    return wait_service_state(socknum, rbuffer, handle, service_name, do_stop, verbose);
}

// Issue a "load service" command (DINIT_CP_LOADSERVICE), without waiting for
// a response. Returns 1 on failure (with error logged), 0 on success.
static int issue_load_service(int socknum, const char *service_name, bool find_only)
{
    // Build buffer;
    uint16_t sname_len = strlen(service_name);
    int bufsize = 3 + sname_len;
    
    std::unique_ptr<char[]> ubuf(new char[bufsize]);
    auto buf = ubuf.get();

    buf[0] = find_only ? DINIT_CP_FINDSERVICE : DINIT_CP_LOADSERVICE;
    memcpy(buf + 1, &sname_len, 2);
    memcpy(buf + 3, service_name, sname_len);

    write_all_x(socknum, buf, bufsize);
    
    return 0;
}

// Check that a "load service" reply was received, and that the requested service was found.
//   state_p may be null.
static int check_load_reply(int socknum, cpbuffer_t &rbuffer, handle_t *handle_p, service_state_t *state_p, bool write_error)
{
    using namespace std;
    
    if (rbuffer[0] == DINIT_RP_SERVICERECORD) {
        fill_buffer_to(rbuffer, socknum, 2 + sizeof(*handle_p));
        rbuffer.extract((char *) handle_p, 2, sizeof(*handle_p));
        if (state_p) *state_p = static_cast<service_state_t>(rbuffer[1]);
        //target_state = static_cast<service_state_t>(rbuffer[2 + sizeof(handle)]);
        rbuffer.consume(3 + sizeof(*handle_p));
        return 0;
    }
    else if (rbuffer[0] == DINIT_RP_NOSERVICE) {
        if (write_error) {
            cerr << "dinitctl: failed to find service description.\n";
            cerr << "dinitctl: check service description file exists / service name spelling.\n";
        }
        return 1;
    }
    else if (rbuffer[0] == DINIT_RP_SERVICE_DESC_ERR) {
        if (write_error) {
            cerr << "dinitctl: error in service description.\n";
            cerr << "dinitctl: try 'dinitcheck <service-name>' or check log for more information.\n";
        }
        return 1;
    }
    else if (rbuffer[0] == DINIT_RP_SERVICE_LOAD_ERR) {
        if (write_error) {
            cerr << "dinitctl: error loading service (or dependency of service).\n";
            cerr << "dinitctl: try 'dinitcheck <service-name>' or check log for more information.\n";
        }
        return 1;
    }
    else {
        throw dinit_protocol_error();
    }
}

static int unpin_service(int socknum, cpbuffer_t &rbuffer, const char *service_name, bool verbose)
{
    using namespace std;

    handle_t handle;
    
    // Build buffer;
    if (! load_service(socknum, rbuffer, service_name, &handle, nullptr)) {
        return 1;
    }
    
    // Issue UNPIN command.
    {
        auto m = membuf()
                .append<char>(DINIT_CP_UNPINSERVICE)
                .append(handle);
        write_all_x(socknum, m);
        
        wait_for_reply(rbuffer, socknum);
        if (rbuffer[0] != DINIT_RP_ACK) {
            cerr << "dinitctl: protocol error." << endl;
            return 1;
        }
        rbuffer.consume(1);
    }

    if (verbose) {
        cout << "Service '" << service_name << "' unpinned." << endl;
    }
    return 0;
}

static int unload_service(int socknum, cpbuffer_t &rbuffer, const char *service_name, bool verbose)
{
    using namespace std;

    if (issue_load_service(socknum, service_name, true) == 1) {
        return 1;
    }

    wait_for_reply(rbuffer, socknum);

    handle_t handle;

    if (rbuffer[0] == DINIT_RP_NOSERVICE) {
        cerr << "dinitctl: service not loaded." << endl;
        return 1;
    }

    if (check_load_reply(socknum, rbuffer, &handle, nullptr) != 0) {
        return 1;
    }

    // Issue UNLOAD command.
    {
        auto m = membuf()
                .append<char>(DINIT_CP_UNLOADSERVICE)
                .append(handle);
        write_all_x(socknum, m);

        wait_for_reply(rbuffer, socknum);
        if (rbuffer[0] == DINIT_RP_NAK) {
            cerr << "dinitctl: could not unload service; service not stopped, or is a dependency of "
                    "other service." << endl;
            return 1;
        }
        if (rbuffer[0] != DINIT_RP_ACK) {
            cerr << "dinitctl: protocol error." << endl;
            return 1;
        }
        rbuffer.consume(1);
    }

    if (verbose) {
        cout << "Service '" << service_name << "' unloaded." << endl;
    }
    return 0;
}

static int reload_service(int socknum, cpbuffer_t &rbuffer, const char *service_name, bool verbose)
{
    using namespace std;

    if (issue_load_service(socknum, service_name, true) == 1) {
        return 1;
    }

    wait_for_reply(rbuffer, socknum);

    handle_t handle;

    if (rbuffer[0] == DINIT_RP_NOSERVICE) {
        cerr << "dinitctl: service not loaded." << endl;
        return 1;
    }

    if (check_load_reply(socknum, rbuffer, &handle, nullptr) != 0) {
        return 1;
    }

    // Issue RELOAD command.
    {
        auto m = membuf()
                .append<char>(DINIT_CP_RELOADSERVICE)
                .append(handle);
        write_all_x(socknum, m);

        wait_for_reply(rbuffer, socknum);
        if (rbuffer[0] == DINIT_RP_NAK) {
            cerr << "dinitctl: could not reload service; service in wrong state, incompatible change, "
                    "or bad service description." << endl;
            return 1;
        }
        if (rbuffer[0] != DINIT_RP_ACK) {
            cerr << "dinitctl: protocol error." << endl;
            return 1;
        }
        rbuffer.consume(1);
    }

    if (verbose) {
        cout << "Service '" << service_name << "' reloaded." << endl;
    }
    return 0;
}

static int list_services(int socknum, cpbuffer_t &rbuffer)
{
    using namespace std;
    
    char cmdbuf[] = { (char)DINIT_CP_LISTSERVICES };
    write_all_x(socknum, cmdbuf, 1);

    wait_for_reply(rbuffer, socknum);
    while (rbuffer[0] == DINIT_RP_SVCINFO) {
        int hdrsize = 8 + std::max(sizeof(int), sizeof(pid_t));
        fill_buffer_to(rbuffer, socknum, hdrsize);
        unsigned name_len = (unsigned char)rbuffer[1];
        service_state_t current = static_cast<service_state_t>(rbuffer[2]);
        service_state_t target = static_cast<service_state_t>(rbuffer[3]);

        int console_flags = rbuffer[4];
        bool has_console = (console_flags & 2) != 0;
        bool waiting_console = (console_flags & 1) != 0;
        bool was_skipped = (console_flags & 4) != 0;
        bool marked_active = (console_flags & 8) != 0;
        bool has_pid = (console_flags & 16) != 0;

        stopped_reason_t stop_reason = static_cast<stopped_reason_t>(rbuffer[5]);

        pid_t service_pid;
        int exit_status;
        if (has_pid) {
            rbuffer.extract((char *)&service_pid, 8, sizeof(service_pid));
        }
        else {
        	rbuffer.extract((char *)&exit_status, 8, sizeof(exit_status));
        }

        fill_buffer_to(rbuffer, socknum, name_len + hdrsize);

        char *name_ptr = rbuffer.get_ptr(hdrsize);
        unsigned clength = std::min(rbuffer.get_contiguous_length(name_ptr), name_len);

        string name = string(name_ptr, clength);
        name.append(rbuffer.get_buf_base(), name_len - clength);

        cout << "[";

        // [ ] if marked active; otherwise, { } if target state is STARTED
        //  +  if started, 's' if skipped, space otherwise
        char lbracket = target == service_state_t::STARTED ? '{' : ' ';
        char rbracket = target == service_state_t::STARTED ? '}' : ' ';
        cout << (marked_active ? '[' : lbracket);
        if (current == service_state_t::STARTED) {
            cout << (was_skipped ? 's' : '+');
        }
        else {
            cout << ' ';
        }
        cout << (marked_active ? ']' : rbracket);
        
        if (current == service_state_t::STARTING) {
            cout << "<<";
        }
        else if (current == service_state_t::STOPPING) {
            cout << ">>";
        }
        else {
            cout << "  ";
        }
        
        cout << (target == service_state_t::STOPPED ? '{' : ' ');
        if (current == service_state_t::STOPPED) {
            bool did_fail = false;
            if (stop_reason == stopped_reason_t::TERMINATED) {
                if (!WIFEXITED(exit_status) || WEXITSTATUS(exit_status) != 0) {
                    did_fail = true;
                }
            }
            else did_fail = (stop_reason != stopped_reason_t::NORMAL);

            cout << (did_fail ? 'X' : '-');
        }
        else {
        	cout << ' ';
        }
        cout << (target == service_state_t::STOPPED ? '}' : ' ');

        cout << "] " << name;

        if (current != service_state_t::STOPPED && has_pid) {
        	cout << " (pid: " << service_pid << ")";
        }
        
        if (current == service_state_t::STOPPED && stop_reason == stopped_reason_t::TERMINATED) {
            if (WIFEXITED(exit_status)) {
                cout << " (exit status: " << WEXITSTATUS(exit_status) << ")";
            }
            else if (WIFSIGNALED(exit_status)) {
                cout << " (signal: " << WTERMSIG(exit_status) << ")";
            }
        }

        if (has_console) {
        	cout << " (has console)";
        }
        else if (waiting_console) {
        	cout << " (waiting for console)";
        }

        cout << endl;

        rbuffer.consume(hdrsize + name_len);
        wait_for_reply(rbuffer, socknum);
    }

    if (rbuffer[0] != DINIT_RP_LISTDONE) {
        cerr << "dinitctl: control socket protocol error" << endl;
        return 1;
    }

    return 0;
}

static int service_status(int socknum, cpbuffer_t &rbuffer, const char *service_name, command_t command, bool verbose)
{
    using namespace std;

    bool is_status = command == command_t::SERVICE_STATUS;

    if (issue_load_service(socknum, service_name, true) == 1) {
        return 1;
    }

    wait_for_reply(rbuffer, socknum);

    handle_t handle;

    if (rbuffer[0] == DINIT_RP_NOSERVICE) {
        if (is_status) {
            cerr << "dinitctl: service not loaded." << endl;
        }
        return 1;
    }

    if (check_load_reply(socknum, rbuffer, &handle, nullptr, is_status) != 0) {
        return 1;
    }

    // Issue STATUS request
    {
        auto m = membuf()
                .append<char>(DINIT_CP_SERVICESTATUS)
                .append(handle);
        write_all_x(socknum, m);

        wait_for_reply(rbuffer, socknum);
        if (rbuffer[0] != DINIT_RP_SERVICESTATUS) {
            cerr << "dinitctl: protocol error." << endl;
            return 1;
        }
        rbuffer.consume(1);

        fill_buffer_to(rbuffer, socknum, STATUS_BUFFER_SIZE + 1 /* reserved */);
        rbuffer.consume(1);

        service_state_t current = static_cast<service_state_t>(rbuffer[0]);
        service_state_t target = static_cast<service_state_t>(rbuffer[1]);

        int console_flags = rbuffer[2];
        bool has_console = (console_flags & 2) != 0;
        bool waiting_console = (console_flags & 1) != 0;
        bool was_skipped = (console_flags & 4) != 0;
        bool marked_active = (console_flags & 8) != 0;
        bool has_pid = (console_flags & 16) != 0;

        stopped_reason_t stop_reason = static_cast<stopped_reason_t>(rbuffer[3]);

        pid_t service_pid = -1;
        int exit_status = 0;
        if (has_pid) {
            rbuffer.extract((char *)&service_pid, 6, sizeof(service_pid));
        }
        else {
            rbuffer.extract((char *)&exit_status, 6, sizeof(exit_status));
        }

        switch (command) {
        case command_t::IS_ACTIVE:
        case command_t::IS_FAILED:
            if (verbose) {
                switch (current) {
                case service_state_t::STOPPED:
                    cout << "STOPPED" << endl;
                    break;
                case service_state_t::STARTING:
                    cout << "STARTING" << endl;
                    break;
                case service_state_t::STARTED:
                    cout << "STARTED" << endl;
                    break;
                case service_state_t::STOPPING:
                    cout << "STOPPING" << endl;
                }
            }
            /* return 0 (success) for started */
            if (command == command_t::IS_ACTIVE) {
                return current != service_state_t::STARTED;
            }
            /* return 0 (success) for specific stopped reasons */
            if (current == service_state_t::STOPPED) {
                switch (stop_reason) {
                case stopped_reason_t::DEPFAILED:
                case stopped_reason_t::FAILED:
                case stopped_reason_t::EXECFAILED:
                case stopped_reason_t::TIMEDOUT:
                    return 0;
                default:
                    break;
                }
            }
            return 1;
        default:
            /* status */
            break;
        }

        cout << "Service: " << service_name << "\n"
                "    State: ";

        switch (current) {
        case service_state_t::STOPPED:
            cout << "STOPPED";
            switch (stop_reason) {
            case stopped_reason_t::DEPRESTART:
                cout << " (dependency restarted)";
                break;
            case stopped_reason_t::DEPFAILED:
                cout << " (dependency failed/terminated)";
                break;
            case stopped_reason_t::FAILED:
                cout << " (failed to start";
                if (exit_status != 0) {
                    cout << "; ";
                    print_termination_details(exit_status);
                }
                cout << ")";
                break;
            case stopped_reason_t::EXECFAILED:
                uint16_t launch_stage;
                rbuffer.extract((char *)&launch_stage, 4, 2);
                cout << " (could not be launched)\n";
                cout << "        Stage: " << exec_stage_descriptions[launch_stage] << "\n";
                cout << "        Error: " << strerror(exit_status);
                break;
            case stopped_reason_t::TERMINATED:
                cout << " (terminated";
                if (exit_status != 0) {
                    cout << "; ";
                    print_termination_details(exit_status);
                }
                cout << ")";
                break;
            case stopped_reason_t::TIMEDOUT:
                cout << " (start timed out)";
                break;
            case stopped_reason_t::NORMAL:
                break;
            }
            break;
        case service_state_t::STARTING:
            cout << "STARTING";
            if (target == service_state_t::STOPPED) {
                cout << " (target state: STOPPED)";
            }
            break;
        case service_state_t::STARTED:
            cout << "STARTED";
            if (was_skipped) {
                cout << " (startup skipped)";
            }
            break;
        case service_state_t::STOPPING:
            cout << "STOPPING";
            if (target == service_state_t::STARTED) {
                cout << " (target state: STARTED)";
            }
            if (exit_status != 0) {
                cout << "(terminated ;";
                print_termination_details(exit_status);
                cout << ")";
            }
        }
        if (has_console) {
            cout << " (holding console)";
        }
        if (waiting_console) {
            cout << " (waiting for console)";
        }
        cout << "\n";

        if (target == service_state_t::STARTED) {
            cout << "    Activation: ";
            if (marked_active) {
                cout << "explicitly started\n";
            }
            else {
                cout << "start due to dependent(s)\n";
            }
        }

        if (service_pid != -1) {
            cout << "    Process ID: " << service_pid << "\n";
        }
    }

    return 0;
}

static int add_remove_dependency(int socknum, cpbuffer_t &rbuffer, bool add,
        const char *service_from, const char *service_to, dependency_type dep_type, bool verbose)
{
    using namespace std;

    handle_t from_handle;
    handle_t to_handle;

    if (! load_service(socknum, rbuffer, service_from, &from_handle, nullptr)
            || ! load_service(socknum, rbuffer, service_to, &to_handle, nullptr)) {
        return 1;
    }

    if (from_handle == to_handle) {
        cerr << "dinitctl: can not add/remove a dependency from a service to itself" << endl;
        return 1;
    }

    auto m = membuf()
            .append<char>(add ? (char)DINIT_CP_ADD_DEP : (char)DINIT_CP_REM_DEP)
            .append(static_cast<char>(dep_type))
            .append(from_handle)
            .append(to_handle);
    write_all_x(socknum, m);

    wait_for_reply(rbuffer, socknum);

    // check reply
    if (rbuffer[0] == DINIT_RP_NAK) {
        if (add) {
            cerr << "dinitctl: could not add dependency: circular dependency or wrong state" << endl;
        }
        else {
            cerr << "dinitctl: no such dependency to remove" << endl;
        }
        return 1;
    }
    if (rbuffer[0] != DINIT_RP_ACK) {
        cerr << "dinitctl: control socket protocol error" << endl;
        return 1;
    }

    if (verbose) {
        std::cout << "Service '" << service_from << "': dependency '" << service_to << "' " << (add ? "added" : "removed") << endl;
    }

    return 0;
}

static int shutdown_dinit(int socknum, cpbuffer_t &rbuffer, bool verbose)
{
    // TODO support no-wait option.
    using namespace std;

    auto m = membuf()
            .append<char>(DINIT_CP_SHUTDOWN)
            .append(static_cast<char>(shutdown_type_t::HALT));
    write_all_x(socknum, m);

    wait_for_reply(rbuffer, socknum);

    if (rbuffer[0] != DINIT_RP_ACK) {
        cerr << "dinitctl: control socket protocol error" << endl;
        return 1;
    }

    if (verbose) {
        std::cout << "Shutting down dinit..." << std::endl;
    }

    // Now wait for rollback complete, by waiting for the connection to close:
    try {
        while (true) {
            wait_for_info(rbuffer, socknum);
            rbuffer.consume(rbuffer[1]);
        }
    }
    catch (cp_read_exception &exc) {
        // Assume that the connection closed.
    }

    if (verbose) {
        std::cout << "Connection closed." << std::endl;
    }

    return 0;
}

// exception for cancelling a service operation
class service_op_cancel { };

static int enable_disable_service(int socknum, cpbuffer_t &rbuffer, const char *from, const char *to,
        bool enable, bool verbose)
{
    using namespace std;

    service_state_t from_state = service_state_t::STARTED;
    handle_t from_handle;

    handle_t to_handle;

    if (!load_service(socknum, rbuffer, from, &from_handle, &from_state)
            || !load_service(socknum, rbuffer, to, &to_handle, nullptr)) {
        return 1;
    }

    // Get service load path
    char buf[1] = { DINIT_CP_QUERY_LOAD_MECH };
    write_all_x(socknum, buf, 1);

    wait_for_reply(rbuffer, socknum);

    if (rbuffer[0] != DINIT_RP_LOADER_MECH) {
        cerr << "dinitctl: control socket protocol error" << endl;
        return 1;
    }

    // Packet type, load mechanism type, packet size:
    fill_buffer_to(rbuffer, socknum, 2 + sizeof(uint32_t));

    if (rbuffer[1] != SSET_TYPE_DIRLOAD) {
        cerr << "dinitctl: unknown configuration, unable to load service descriptions" << endl;
        return 1;
    }

    vector<string> paths;

    uint32_t pktsize;
    rbuffer.extract(&pktsize, 2, sizeof(uint32_t));

    fill_buffer_to(rbuffer, socknum, 2 + sizeof(uint32_t) * 3); // path entries, cwd length

    uint32_t path_entries;  // number of service directories
    rbuffer.extract(&path_entries, 2 + sizeof(uint32_t), sizeof(uint32_t));

    uint32_t cwd_len;
    rbuffer.extract(&cwd_len, 2 + sizeof(uint32_t) * 2, sizeof(uint32_t));
    rbuffer.consume(2 + sizeof(uint32_t) * 3);
    pktsize -= 2 + sizeof(uint32_t) * 3;

    // Read current working directory of daemon:
    std::string dinit_cwd = read_string(socknum, rbuffer, cwd_len);

    // dinit daemon base directory against which service paths are resolved is in dinit_cwd

    for (uint32_t i = 0; i < path_entries; ++i) {
        uint32_t plen;
        fill_buffer_to(rbuffer, socknum, sizeof(uint32_t));
        rbuffer.extract(&plen, 0, sizeof(uint32_t));
        rbuffer.consume(sizeof(uint32_t));
        paths.push_back(read_string(socknum, rbuffer, plen));
    }

    // all service directories are now in the 'paths' vector
    // Load/read service description for 'from' service:

    ifstream service_file;
    string service_file_path;

    for (std::string path : paths) {
        string test_path = combine_paths(combine_paths(dinit_cwd, path.c_str()), from);

        service_file.open(test_path.c_str(), ios::in);
        if (service_file) {
            service_file_path = test_path;
            break;
        }
    }

    if (! service_file) {
        cerr << "dinitctl: could not locate service file for service '" << from << "'" << endl;
        return 1;
    }

    // We now need to read the service file, identify the waits-for.d directory (bail out if more than one),
    // make sure the service is not listed as a dependency individually.

    string waits_for_d;

    try {
        process_service_file(from, service_file, [&](string &line, unsigned line_num, string &setting,
                dinit_load::string_iterator i, dinit_load::string_iterator end) -> void {
            if (setting == "waits-for" || setting == "depends-on" || setting == "depends-ms") {
                string dname = dinit_load::read_setting_value(line_num, i, end);
                if (dname == to) {
                    // There is already a dependency
                    cerr << "dinitctl: there is a fixed dependency to service '" << to
                            << "' in the service description of '" << from << "'." << endl;
                    throw service_op_cancel();
                }
            }
            else if (setting == "waits-for.d") {
                string dname = dinit_load::read_setting_value(line_num, i, end);
                if (! waits_for_d.empty()) {
                    cerr << "dinitctl: service '" << from << "' has multiple waits-for.d directories "
                            << "specified in service description" << endl;
                    throw service_op_cancel();
                }
                waits_for_d = std::move(dname);
            }
        });
    }
    catch (const service_op_cancel &cexc) {
        return 1;
    }

    // If the from service has no waits-for.d specified, we can't continue
    if (waits_for_d.empty()) {
        cerr << "dinitctl: service '" << from << "' has no waits-for.d directory specified" << endl;
        return 1;
    }

    // The waits-for.d path is relative to the service file path, combine:
    string waits_for_d_full = combine_paths(parent_path(service_file_path), waits_for_d.c_str());

    // check if dependency already exists
    string dep_link_path = combine_paths(waits_for_d_full, to);
    struct stat stat_buf;
    if (lstat(dep_link_path.c_str(), &stat_buf) == -1) {
        if (errno != ENOENT) {
            cerr << "dinitctl: checking for existing dependency link: " << dep_link_path << ": "
                    << strerror(errno) << endl;
            return 1;
        }
    }
    else {
        // dependency already exists
        if (enable) {
            cerr << "dinitctl: service already enabled." << endl;
            return 1;
        }
    }

    // warn if 'from' service is not started
    if (enable && from_state != service_state_t::STARTED) {
        cerr << "dinitctl: warning: enabling dependency for non-started service" << endl;
    }

    // add/remove dependency
    constexpr int enable_pktsize = 2 + sizeof(handle_t) * 2;
    char cmdbuf[enable_pktsize] = { char(enable ? DINIT_CP_ENABLESERVICE : DINIT_CP_REM_DEP),
            char(dependency_type::WAITS_FOR)};
    memcpy(cmdbuf + 2, &from_handle, sizeof(from_handle));
    memcpy(cmdbuf + 2 + sizeof(from_handle), &to_handle, sizeof(to_handle));
    write_all_x(socknum, cmdbuf, enable_pktsize);

    wait_for_reply(rbuffer, socknum);

    // check reply
    if (rbuffer[0] == DINIT_RP_NAK) {
        if (enable) {
            cerr << "dinitctl: could not enable service: possible circular dependency" << endl;
        }
        else {
            cerr << "dinitctl: service not currently enabled" << endl;
        }
        return 1;
    }
    if (rbuffer[0] != DINIT_RP_ACK) {
        cerr << "dinitctl: control socket protocol error" << endl;
        return 1;
    }
    rbuffer.consume(1);

    // create link
    if (enable) {
        if (symlink((string("../") + to).c_str(), dep_link_path.c_str()) == -1) {
            cerr << "dinitctl: could not create symlink at " << dep_link_path << ": " << strerror(errno)
                    << "\n" "dinitctl: note: service was enabled for now; persistent enable failed."
                    << endl;
            return 1;
        }
    }
    else {
        if (unlink(dep_link_path.c_str()) == -1) {
            cerr << "dinitctl: could not unlink dependency entry " << dep_link_path << ": "
                    << strerror(errno) << "\n"
                    "dinitctl: note: service was disabled for now; persistent disable failed." << endl;
            return 1;
        }
    }

    // Check status of the service now
    auto m = membuf()
            .append<char>(DINIT_CP_SERVICESTATUS)
            .append(to_handle);
    write_all_x(socknum, m);

    wait_for_reply(rbuffer, socknum);
    if (rbuffer[0] != DINIT_RP_SERVICESTATUS) {
        cerr << "dinitctl: protocol error." << endl;
        return 1;
    }
    rbuffer.consume(1);

    int statussize = 6 + std::max(sizeof(pid_t), sizeof(int));;
    fill_buffer_to(rbuffer, socknum, statussize + 1 /* reserved */);
    rbuffer.consume(1);
    service_state_t current = static_cast<service_state_t>(rbuffer[0]);
    service_state_t target = static_cast<service_state_t>(rbuffer[1]);
    rbuffer.consume(statussize);

    if (verbose) {
        cout << "Service '" << to << "' has been " << (enable ? "enabled" : "disabled") << "." << endl;
    }

    if (enable) {
        if (current != service_state_t::STARTED) {
            wait_service_state(socknum, rbuffer, to_handle, to, false /* start */, verbose);
        }
    }
    else {
        if (target != service_state_t::STOPPED) {
            std::cerr << "dinitctl: note: disabled service may have other dependents\n";
        }
    }

    return 0;
}

static int do_setenv(int socknum, cpbuffer_t &rbuffer, std::vector<const char *> &env_names)
{
    using namespace std;

    string buf;

    for (const char *envp : env_names) {
        buf.clear();
        buf.reserve(6);
        // protocol message and size space
        buf.push_back(DINIT_CP_SETENV);
        buf.append(2, 0);
        const unsigned hdr_len = 3;
        // either full var or name
        auto elen = strlen(envp);
        buf.append(envp, elen);
        // if '=' not found, get value from environment
        if (!memchr(envp, '=', elen)) {
            buf.push_back('=');
            auto *envv = getenv(envp);
            if (envv) {
                buf.append(envv);
            }
        }
        uint16_t bufs = buf.size() - hdr_len;
        // sanitize length early on
        if (buf.size() > cpbuffer_t::get_size()) {
            auto eq = buf.find('=', hdr_len);
            auto name = buf.substr(hdr_len, eq - hdr_len);
            cerr << "dinitctl: environment variable '" << name << "' too long." << endl;
            return 1;
        }
        // set size in protocol message
        memcpy(&buf[1], &bufs, 2);
        // send
        write_all_x(socknum, buf.data(), buf.size());
        wait_for_reply(rbuffer, socknum);
        if (rbuffer[0] == DINIT_RP_BADREQ) {
            cerr << "dinitctl: failed to export environment." << endl;
            return 1;
        } else if (rbuffer[0] != DINIT_RP_ACK) {
            throw dinit_protocol_error();
        }
        rbuffer.consume(1);
    }

    return 0;
}

static int trigger_service(int socknum, cpbuffer_t &rbuffer, const char *service_name, bool trigger_value)
{
    using namespace std;

    handle_t handle;
    if (!load_service(socknum, rbuffer, service_name, &handle, nullptr, true)) {
        return 1;
    }

    // Issue SET_TRIGGER command.
    {
        auto m = membuf()
                .append<char>(DINIT_CP_SETTRIGGER)
                .append(handle)
                .append<char>(trigger_value);
        write_all_x(socknum, m);

        wait_for_reply(rbuffer, socknum);
        if (rbuffer[0] == DINIT_RP_NAK) {
            cerr << "dinitctl: cannot trigger a service that is not of 'triggered' type.\n";
            return 1;
        }
        if (rbuffer[0] != DINIT_RP_ACK) {
            cerr << "dinitctl: protocol error.\n";
            return 1;
        }
        rbuffer.consume(1);
    }

    return 0;
}

static int cat_service_log(int socknum, cpbuffer_t &rbuffer, const char *service_name)
{
    using namespace std;

    handle_t handle;
    if (!load_service(socknum, rbuffer, service_name, &handle, nullptr, true)) {
        return 1;
    }

    // Issue CATLOG
    auto m = membuf()
    		 .append<char>(DINIT_CP_CATLOG)
			 .append<char>(0)
			 .append(handle);
    write_all_x(socknum, m);

    wait_for_reply(rbuffer, socknum);
    if (rbuffer[0] == DINIT_RP_NAK) {
        cerr << "dinitctl: cannot cat log for service not configured to buffer output.\n";
        return 1;
    }
    if (rbuffer[0] != DINIT_RP_SERVICE_LOG) {
        cerr << "dinitctl: protocol error.\n";
        return 1;
    }

    fill_buffer_to(rbuffer, socknum, 1 + sizeof(unsigned));
    unsigned bufsize;
    rbuffer.extract(&bufsize, 1, sizeof(unsigned));
    rbuffer.consume(1 + sizeof(unsigned));

    // output the log
    if (bufsize > 0) {
		cout << flush;

		bool trailing_nl = false;
		char output_buf[rbuffer.get_size()];
		while (bufsize > 0) {
			unsigned l = rbuffer.get_length();
			if (l == 0) {
				fill_buffer_to(rbuffer, socknum, 1);
			}
			l = std::min(rbuffer.get_length(), bufsize);
			rbuffer.extract(output_buf, 0, l);
			write(STDOUT_FILENO, output_buf, l);
			rbuffer.consume(l);
			bufsize -= l;
			trailing_nl = (output_buf[l - 1] == '\n');
		}

		if (!trailing_nl) {
			cout << "\n(last line is truncated or incomplete)\n";
		}
    }

    return 0;
}
