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
#include "control-datatypes.h"
#include "cpbuffer.h"
#include "dinit-client.h"
#include "dinit-util.h"
#include "dinit-iostream.h"
#include "file-input-stack.h"
#include "load-service.h"
#include "options-processing.h"
#include "service-constants.h"
#include "mconfig.h"

// dinitctl:  utility to control the Dinit daemon, including starting and stopping of services.

#define DINIT_CHECK_APPNAME "dinit-check"
#define DINITCTL_APPNAME "dinitctl"

// This utility communicates with the dinit daemon via a unix stream socket (as specified in
// SYSCONTROLSOCKET, or $HOME/.dinitctl).

// common communication datatypes
using namespace dinit_cptypes;

// minimum and maximum protocol verions we can speak
static constexpr uint16_t min_cp_version = 1;
static constexpr uint16_t max_cp_version = 5;

enum class ctl_cmd;

static int issue_load_service(int socknum, const char *service_name, bool find_only = false);
static int check_load_reply(int socknum, cpbuffer_t &, handle_t *handle_p, service_state_t *state_p,
        bool write_error=true);
static int start_stop_service(int socknum, cpbuffer_t &, const char *service_name, ctl_cmd command,
        bool do_pin, bool do_force, bool wait_for_service, bool ignore_unstarted, bool verbose);
static int unpin_service(int socknum, cpbuffer_t &, const char *service_name, bool verbose);
static int unload_service(int socknum, cpbuffer_t &, const char *service_name, bool verbose);
static int reload_service(int socknum, cpbuffer_t &, const char *service_name, bool verbose);
static int list_services(int socknum, cpbuffer_t &, uint16_t proto_version);
static int service_status(int socknum, cpbuffer_t &rbuffer, const char *service_name,
        ctl_cmd command, uint16_t proto_version, bool verbose);
static int shutdown_dinit(int soclknum, cpbuffer_t &, bool verbose);
static int add_remove_dependency(int socknum, cpbuffer_t &rbuffer, bool add, const char *service_from,
        const char *service_to, dependency_type dep_type, bool verbose);
static int enable_disable_service(int socknum, cpbuffer_t &rbuffer, service_dir_opt &service_dir_opts,
        const char *from, const char *to, bool enable, const char *environment_file, bool verbose,
        uint16_t proto_version);
static int do_setenv(int socknum, cpbuffer_t &rbuffer, std::vector<const char *> &env_names, bool unset);
static int trigger_service(int socknum, cpbuffer_t &rbuffer, const char *service_name, bool trigger_value);
static int cat_service_log(int socknum, cpbuffer_t &rbuffer, const char *service_name, bool do_clear);
static int signal_send(int socknum, cpbuffer_t &rbuffer, const char *service_name, sig_num_t sig_num);
static int signal_list();

enum class ctl_cmd {
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
    UNSETENV,
    SET_TRIGGER,
    UNSET_TRIGGER,
    CAT_LOG,
    SIG_SEND,
    SIG_LIST,
    IS_STARTED,
    IS_FAILED,
};

// Entry point.
int dinitctl_main(int argc, char **argv)
{
    using std::cout;
    using std::cerr;

    // general options
    bool cmdline_error = false;
    bool show_help = argc < 2; // show help if no arguments
    std::string control_socket_str;
    const char *control_socket_path = nullptr;
    bool verbose = true;
    bool user_dinit = (getuid() != 0);  // communicate with user daemon
    service_dir_opt service_dir_opts;
    bool offline = false;
    const char *env_file = nullptr;

    // general command options
    ctl_cmd command = ctl_cmd::NONE;
    std::vector<const char *> cmd_args;

    // specific command options
    const char *service_name = nullptr;
    const char *to_service_name = nullptr;
    dependency_type dep_type = dependency_type::AFTER; // avoid maybe-uninitialised warning
    bool dep_type_set = false;
    bool catlog_clear = false;
    bool wait_for_service = true;
    bool do_pin = false;
    bool do_force = false;
    bool ignore_unstarted = false;
    bool use_passed_cfd = false;
    bool show_siglist = false;
    std::string sigstr;
    sig_num_t sig_num = -1;

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
                if (i == argc || argv[i][0] == '\0') {
                    cerr << DINITCTL_APPNAME ": --socket-path/-p should be followed by socket path\n";
                    return 1;
                }
                control_socket_str = argv[i];
            }
            else if (strcmp(argv[i], "--use-passed-cfd") == 0) {
                use_passed_cfd = true;
            }
            else if (strcmp(argv[i], "--from") == 0) {
                if (command == ctl_cmd::ENABLE_SERVICE || command == ctl_cmd::DISABLE_SERVICE) {
                    ++i;
                    if (i == argc || argv[i][0] == '\0') {
                        cerr << DINITCTL_APPNAME ": --from should be followed by a service name\n";
                        return 1;
                    }
                    service_name = argv[i];
                }
                else {
                    cmdline_error = true;
                    break;
                }
            }
            else if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0) {
                if (command == ctl_cmd::STOP_SERVICE || command == ctl_cmd::RESTART_SERVICE) {
                    do_force = true;
                }
                else {
                    cmdline_error = true;
                    break;
                }
            }
            else if (strcmp(argv[i], "--clear") == 0) {
                if (command == ctl_cmd::CAT_LOG) {
                    catlog_clear = true;
                }
                else {
                    cmdline_error = true;
                    break;
                }
            }
            else if (strcmp(argv[i], "--list") == 0 || strcmp(argv[i], "-l") == 0) {
                if (command == ctl_cmd::SIG_SEND) {
                    show_siglist = true;
                }
                else {
                    cmdline_error = true;
                    break;
                }
            }
            else if (strcmp(argv[i], "--services-dir") == 0 || strcmp(argv[i], "-d") == 0) {
                if (++i < argc && argv[i][0] != '\0') {
                    service_dir_opts.add_specified_service_dir(argv[i]);
                }
                else {
                    cerr << DINITCTL_APPNAME ": '--services-dir' (-d) requires an argument\n";
                    return 1;
                }
            }
            else if (strcmp(argv[i], "--offline") == 0 || strcmp(argv[i], "-o") == 0) {
                offline = true;
            }
            else {
                cerr << DINITCTL_APPNAME ": unrecognized/invalid option: " << argv[i] << " (use --help for help)\n";
                return 1;
            }
        }
        else if (command == ctl_cmd::NONE) {
            if (strcmp(argv[i], "start") == 0) {
                command = ctl_cmd::START_SERVICE; 
            }
            else if (strcmp(argv[i], "wake") == 0) {
                command = ctl_cmd::WAKE_SERVICE;
            }
            else if (strcmp(argv[i], "stop") == 0) {
                command = ctl_cmd::STOP_SERVICE;
            }
            else if (strcmp(argv[i], "restart") == 0) {
                command = ctl_cmd::RESTART_SERVICE;
            }
            else if (strcmp(argv[i], "release") == 0) {
                command = ctl_cmd::RELEASE_SERVICE;
            }
            else if (strcmp(argv[i], "unpin") == 0) {
                command = ctl_cmd::UNPIN_SERVICE;
            }
            else if (strcmp(argv[i], "unload") == 0) {
                command = ctl_cmd::UNLOAD_SERVICE;
            }
            else if (strcmp(argv[i], "reload") == 0) {
                command = ctl_cmd::RELOAD_SERVICE;
            }
            else if (strcmp(argv[i], "list") == 0) {
                command = ctl_cmd::LIST_SERVICES;
            }
            else if (strcmp(argv[i], "status") == 0) {
                command = ctl_cmd::SERVICE_STATUS;
            }
            else if (strcmp(argv[i], "is-started") == 0) {
                command = ctl_cmd::IS_STARTED;
            }
            else if (strcmp(argv[i], "is-failed") == 0) {
                command = ctl_cmd::IS_FAILED;
            }
            else if (strcmp(argv[i], "shutdown") == 0) {
                command = ctl_cmd::SHUTDOWN;
            }
            else if (strcmp(argv[i], "add-dep") == 0) {
                command = ctl_cmd::ADD_DEPENDENCY;
            }
            else if (strcmp(argv[i], "rm-dep") == 0) {
                command = ctl_cmd::RM_DEPENDENCY;
            }
            else if (strcmp(argv[i], "enable") == 0) {
                command = ctl_cmd::ENABLE_SERVICE;
            }
            else if (strcmp(argv[i], "disable") == 0) {
                command = ctl_cmd::DISABLE_SERVICE;
            }
            else if (strcmp(argv[i], "setenv") == 0) {
                command = ctl_cmd::SETENV;
            }
            else if (strcmp(argv[i], "unsetenv") == 0) {
                command = ctl_cmd::UNSETENV;
            }
            else if (strcmp(argv[i], "trigger") == 0) {
                command = ctl_cmd::SET_TRIGGER;
            }
            else if (strcmp(argv[i], "untrigger") == 0) {
                command = ctl_cmd::UNSET_TRIGGER;
            }
            else if (strcmp(argv[i], "catlog") == 0) {
                command = ctl_cmd::CAT_LOG;
            }
            else if (strcmp(argv[i], "signal") == 0) {
                command = ctl_cmd::SIG_SEND;
            }
            else {
                cerr << DINITCTL_APPNAME ": unrecognized command: " << argv[i] << " (use --help for help)\n";
                return 1;
            }
        }
        else {
            // service name / other non-option
            if (argv[i][0] == '\0') {
                cerr << DINITCTL_APPNAME ": Invalid empty argument\n";
                return 1;
            }
            if (command == ctl_cmd::ADD_DEPENDENCY || command == ctl_cmd::RM_DEPENDENCY) {
                if (!dep_type_set) {
                    if (strcmp(argv[i], "need") == 0 || strcmp(argv[i], "regular") == 0) {
                        dep_type = dependency_type::REGULAR;
                    }
                    else if (strcmp(argv[i], "milestone") == 0) {
                        dep_type = dependency_type::MILESTONE;
                    }
                    else if (strcmp(argv[i], "waits-for") == 0) {
                        dep_type = dependency_type::WAITS_FOR;
                    }
                    else {
                        cmdline_error = true;
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
                    cmdline_error = true;
                    break;
                }
            }
            else if (command == ctl_cmd::ENABLE_SERVICE || command == ctl_cmd::DISABLE_SERVICE) {
                if (to_service_name != nullptr) {
                    cmdline_error = true;
                    break;
                }
                to_service_name = argv[i];
            }
            else if (command == ctl_cmd::SIG_SEND) {
                if (!show_siglist) {
                    if (sigstr.empty()) {
                        sigstr = argv[i];
                    }
                    else if (service_name == nullptr) {
                        service_name = argv[i];
                    }
                    else {
                        cmdline_error = true;
                    }
                }
                else {
                    cmdline_error = true;
                }
            }
            else {
                cmd_args.push_back(argv[i]);
            }
        }
    }
    
    // Additional argument checks/processing for various commands:

    if (command == ctl_cmd::NONE && !show_help) {
        cmdline_error = true;
    }
    else if (command == ctl_cmd::ENABLE_SERVICE || command == ctl_cmd::DISABLE_SERVICE) {
        cmdline_error |= (to_service_name == nullptr);
    }
    else if (command == ctl_cmd::SETENV || command == ctl_cmd::UNSETENV) {
        // Handle (UN)SETENV specially, since it needs arguments but they are not service names
        if (cmd_args.empty()) {
            cmdline_error = true;
        }
    }
    else if (command == ctl_cmd::SIG_SEND) {
        if (show_siglist) {
            if (sigstr.empty()) {
                command = ctl_cmd::SIG_LIST;
            }
            else {
                cmdline_error = true;
            }
        }
        else {
            if (sigstr.empty()) {
                cerr << DINITCTL_APPNAME ": signal number/name must be specified\n";
                return 1;
            }
            if (service_name == nullptr) {
                cerr << DINITCTL_APPNAME ": service name must be specified\n";
                return 1;
            }
            sig_num = dinit_load::signal_name_to_number(sigstr);
            if (sig_num <= 0) {
                // (0 actually means "none"/"NONE", but we'll just fall through and error out when
                // it fails to parse as an integer)
                try {
                    size_t pos;
                    sig_num = std::stoi(sigstr, &pos);
                    if (sigstr.size() != pos) {
                        throw std::invalid_argument("");
                    }
                }
                catch (std::exception &) { // invalid_argument, out_of_range
                    cerr << DINITCTL_APPNAME ": '" << sigstr
                            << "' is not a valid signal name/number" << std::endl;
                    return 1;
                }
            }
        }
    }
    else {
        bool no_service_cmd = (command == ctl_cmd::LIST_SERVICES
                              || command == ctl_cmd::SHUTDOWN
                              || command == ctl_cmd::SIG_LIST);
        if (no_service_cmd) {
            if (!cmd_args.empty()) {
                cmdline_error = true;
            }
        }
        else {
            if (command == ctl_cmd::ADD_DEPENDENCY || command == ctl_cmd::RM_DEPENDENCY) {
                if (!dep_type_set || service_name == nullptr || to_service_name == nullptr) {
                    cmdline_error = true;
                }
            }
            else if (cmd_args.empty()) {
                cmdline_error = true;
            }
            else {
                // No command can currently accept more than one service argument:
                if (cmd_args.size() > 1) {
                    cmdline_error = true;
                }
                service_name = cmd_args.front();
            }
        }
    }

    if (show_help) {
        cout << DINITCTL_APPNAME ":   control Dinit services\n"
          "\n"
          "Usage:\n"
          "    " DINITCTL_APPNAME " [options] status <service-name>\n"
          "    " DINITCTL_APPNAME " [options] is-started <service-name>\n"
          "    " DINITCTL_APPNAME " [options] is-failed <service-name>\n"
          "    " DINITCTL_APPNAME " [options] start [options] <service-name>\n"
          "    " DINITCTL_APPNAME " [options] stop [options] <service-name>\n"
          "    " DINITCTL_APPNAME " [options] restart [options] <service-name>\n"
          "    " DINITCTL_APPNAME " [options] wake [options] <service-name>\n"
          "    " DINITCTL_APPNAME " [options] release [options] <service-name>\n"
          "    " DINITCTL_APPNAME " [options] unpin <service-name>\n"
          "    " DINITCTL_APPNAME " [options] unload <service-name>\n"
          "    " DINITCTL_APPNAME " [options] reload <service-name>\n"
          "    " DINITCTL_APPNAME " [options] list\n"
          "    " DINITCTL_APPNAME " [options] shutdown\n"
          "    " DINITCTL_APPNAME " [options] add-dep <type> <from-service> <to-service>\n"
          "    " DINITCTL_APPNAME " [options] rm-dep <type> <from-service> <to-service>\n"
          "    " DINITCTL_APPNAME " [options] enable [--from <from-service>] <to-service>\n"
          "    " DINITCTL_APPNAME " [options] disable [--from <from-service>] <to-service>\n"
          "    " DINITCTL_APPNAME " [options] trigger <service-name>\n"
          "    " DINITCTL_APPNAME " [options] untrigger <service-name>\n"
          "    " DINITCTL_APPNAME " [options] setenv [name[=value] ...]\n"
          "    " DINITCTL_APPNAME " [options] unsetenv [name ...]\n"
          "    " DINITCTL_APPNAME " [options] catlog <service-name>\n"
          "    " DINITCTL_APPNAME " [options] signal <signal> <service-name>\n"
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
          "                     environment variable to communicate with the dinit daemon\n"
          "  -o, --offline    : do not contact running dinit daemon\n"
          "  -d, --services-dir <dir>\n"
          "                   : specify directory for service definitions (offline mode)\n"
          "\n"
          "Command options:\n"
          "  --no-wait        : don't wait for service startup/shutdown to complete\n"
          "  --pin            : pin the service in the requested state\n"
          "  --force          : force stop even if dependents will be affected\n"
          "  -l, --list       : (signal) list supported signals\n";
        return 0;
    }

    if (cmdline_error) {
        cerr << DINITCTL_APPNAME ": Invalid command line.\n"
                "Try '" DINITCTL_APPNAME " --help' for more information.\n";
        return 1;
    }
    
    // SIG_LIST doesn't need a control socket connection so handle it specially.
    if (command == ctl_cmd::SIG_LIST) {
        return signal_list();
    }

    cpbuffer_t rbuffer;

    if (offline) {
        if (command != ctl_cmd::ENABLE_SERVICE && command != ctl_cmd::DISABLE_SERVICE) {
            cerr << DINITCTL_APPNAME ": offline mode (--offline/-o) not supported for this command\n";
            return 1;
        }

        service_dir_opts.build_paths(!user_dinit);
        if (env_file == nullptr && !user_dinit) {
            env_file = "/etc/dinit/environment";
        }

        if (command == ctl_cmd::ENABLE_SERVICE || command == ctl_cmd::DISABLE_SERVICE) {
            return enable_disable_service(-1, rbuffer, service_dir_opts, service_name, to_service_name,
                    command == ctl_cmd::ENABLE_SERVICE, env_file, verbose, 0);
        }
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

    bool user_specified_cs_path = false;

    if (!use_passed_cfd) {
        // Locate control socket
        if (!control_socket_str.empty()) {
            control_socket_path = control_socket_str.c_str();
            user_specified_cs_path = true;
        }
        else {
            control_socket_path = get_default_socket_path(control_socket_str, user_dinit);
            if (control_socket_path == nullptr) {
                cerr << DINITCTL_APPNAME ": cannot determine control socket directory (set "
                        "XDG_RUNTIME_DIR or HOME, check /etc/passwd file, or specify socket path "
                        "via -p)\n";
                return 1;
            }
        }
    }
    
    try {
        if (!use_passed_cfd) {
            socknum = connect_to_daemon(control_socket_path);
        }

        // Start by querying protocol version:
        uint16_t daemon_protocol_ver = check_protocol_version(min_cp_version, max_cp_version, rbuffer, socknum);

        if (command == ctl_cmd::UNPIN_SERVICE) {
            return unpin_service(socknum, rbuffer, service_name, verbose);
        }
        else if (command == ctl_cmd::UNLOAD_SERVICE) {
            return unload_service(socknum, rbuffer, service_name, verbose);
        }
        else if (command == ctl_cmd::RELOAD_SERVICE) {
            return reload_service(socknum, rbuffer, service_name, verbose);
        }
        else if (command == ctl_cmd::LIST_SERVICES) {
            return list_services(socknum, rbuffer, daemon_protocol_ver);
        }
        else if (command == ctl_cmd::SERVICE_STATUS || command == ctl_cmd::IS_STARTED
                || command == ctl_cmd::IS_FAILED) {
            return service_status(socknum, rbuffer, service_name, command, daemon_protocol_ver, verbose);
        }
        else if (command == ctl_cmd::SHUTDOWN) {
            return shutdown_dinit(socknum, rbuffer, verbose);
        }
        else if (command == ctl_cmd::ADD_DEPENDENCY || command == ctl_cmd::RM_DEPENDENCY) {
            return add_remove_dependency(socknum, rbuffer, command == ctl_cmd::ADD_DEPENDENCY,
                    service_name, to_service_name, dep_type, verbose);
        }
        else if (command == ctl_cmd::ENABLE_SERVICE || command == ctl_cmd::DISABLE_SERVICE) {
            // If only one service specified, assume that we enable for 'boot' service:
            if (daemon_protocol_ver < 3) {
                // We need QUERYSERVICEDSCDIR
                throw cp_old_server_exception();
            }
            return enable_disable_service(socknum, rbuffer, service_dir_opts, service_name, to_service_name,
                    command == ctl_cmd::ENABLE_SERVICE, env_file, verbose, daemon_protocol_ver);
        }
        else if (command == ctl_cmd::SETENV || command == ctl_cmd::UNSETENV) {
            return do_setenv(socknum, rbuffer, cmd_args, command == ctl_cmd::UNSETENV);
        }
        else if (command == ctl_cmd::SET_TRIGGER || command == ctl_cmd::UNSET_TRIGGER) {
            if (daemon_protocol_ver < 2) {
                throw cp_old_server_exception();
            }
            return trigger_service(socknum, rbuffer, service_name, (command == ctl_cmd::SET_TRIGGER));
        }
        else if (command == ctl_cmd::CAT_LOG) {
            if (daemon_protocol_ver < 2) {
                throw cp_old_server_exception();
            }
            return cat_service_log(socknum, rbuffer, service_name, catlog_clear);
        }
        else if (command == ctl_cmd::SIG_SEND) {
            if (daemon_protocol_ver < 2) {
                throw cp_old_server_exception();
            }
            return signal_send(socknum, rbuffer, service_name, sig_num);
        }
        else {
            return start_stop_service(socknum, rbuffer, service_name, command, do_pin, do_force,
                    wait_for_service, ignore_unstarted, verbose);
        }
    }
    catch (cp_old_client_exception &e) {
        cerr << DINITCTL_APPNAME ": too old (daemon reports newer protocol version)\n";
    }
    catch (cp_old_server_exception &e) {
        cerr << DINITCTL_APPNAME ": daemon too old or protocol error\n";
    }
    catch (cp_read_exception &e) {
        cerr << DINITCTL_APPNAME ": control socket read failure or protocol error\n";
    }
    catch (cp_write_exception &e) {
        cerr << DINITCTL_APPNAME ": control socket write error: " << std::strerror(e.errcode) << "\n";
    }
    catch (dinit_protocol_error &e) {
        cerr << DINITCTL_APPNAME ": protocol error\n";
    }
    catch (control_sock_conn_err &ce) {
        cerr << DINITCTL_APPNAME ": " << ce.get_action() << ": " << ce.get_arg() << ": " << strerror(ce.get_err()) << "\n";
        if (user_dinit && ce.get_err() == ENOENT && !user_specified_cs_path) {
            // It is common enough that users don't realise they need to have a user instance
            // running in order to control it, so elaborate a little:
            cerr << DINITCTL_APPNAME ": perhaps no user instance is running?\n";
        }
    }
    catch (general_error &ge) {
        std::cerr << DINITCTL_APPNAME;
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
        std::cerr << DINITCTL_APPNAME ": out of memory\n";
    }
    return 1;
}

static const char *describe_state(bool stopped)
{
    return stopped ? "stopped" : "started";
}

static const char *describe_verb(bool stop)
{
    return stop ? "stop" : "start";
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
            .append((char) cp_cmd::QUERYSERVICENAME)
            .append((char) 0)
            .append(handle);
    write_all_x(socknum, m);

    wait_for_reply(rbuffer, socknum);

    if (rbuffer[0] != (char)cp_rply::SERVICENAME) {
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
    using std::cout;

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

static void print_termination_details(int exit_si_code, int exit_si_status)
{
    using std::cout;

    if (exit_si_code == CLD_KILLED) {
        cout << "signalled - signal ";
        cout << exit_si_status;
    }
    else if (exit_si_code == CLD_EXITED) {
        cout << "exited - status ";
        cout << exit_si_status;
    }
    else {
        cout << "unknown reason";
    }
}

// Print reason for start failure.
static void print_failure_details(stopped_reason_t stop_reason, uint16_t launch_stage,
        int exit_status, int exit_si_code, int exit_si_status)
{
    using std::cout;

    switch (stop_reason) {
        case stopped_reason_t::DEPFAILED:
            cout << "Reason: a dependency of the service failed to start. "
                    "Check dinit log.\n";
            break;
        case stopped_reason_t::TIMEDOUT:
            cout << "Reason: start timed out.\n";
            break;
        case stopped_reason_t::EXECFAILED:
            cout << "Reason: execution of service process failed:\n";
            cout << "        Stage: " << exec_stage_descriptions[launch_stage] << "\n";
            cout << "        Error: " << strerror(exit_status) << "\n";
            break;
        case stopped_reason_t::FAILED:
            cout << "Reason: service process terminated before ready: ";
            if (exit_si_code != 0 || exit_si_status != 0) {
                print_termination_details(exit_si_code, exit_si_status);
            }
            else {
                print_termination_details(exit_status);
            }
            cout << "\n";
            break;
        default:
            cout << "Reason unknown/unrecognised. Check dinit log.\n";
    }
}

// Process a SERVICEEVENT[5] packet if it is related to the specified service handle, and
// optionally report the service status to the user (verbose == true). The caller must ensure that
// a complete packet of type SERVICEEVENT[5] is present in the buffer before calling. The size of
// the packet should be provided as pktlen.
//
// Returns 0 if the service started (do_stop == false) or stopped (do_stop == true), 1 if
// start/stop was cancelled or failed, -1 when the service event is not related to given service
// handle or does not correspond to a start (or stop) or failure.
static int process_service_event(cpbuffer_t &rbuffer, unsigned pktlen, handle_t handle,
        const std::string &service_name, bool do_stop, bool verbose)
{
    using std::cout;

    // earlier versions do not include status info, the size in that case is
    // base_pkt_size:
    constexpr unsigned base_pkt_size = 2 + sizeof(handle_t) + 1;
    if (pktlen < base_pkt_size) {
        throw dinit_protocol_error();
    }

    // version 5 packets include extended staus info:
    if (rbuffer[0] == (char)cp_info::SERVICEEVENT5 && pktlen < (base_pkt_size + STATUS_BUFFER5_SIZE)) {
        throw dinit_protocol_error();
    }

    service_event_t completion_event;
    service_event_t cancelled_event;
    if (do_stop) {
        completion_event = service_event_t::STOPPED;
        cancelled_event = service_event_t::STOPCANCELLED;
    }
    else {
        completion_event = service_event_t::STARTED;
        cancelled_event = service_event_t::STARTCANCELLED;
    }

    handle_t ev_handle;
    rbuffer.extract((char *)&ev_handle, 2, sizeof(ev_handle));
    service_event_t event = static_cast<service_event_t>(rbuffer[2 + sizeof(ev_handle)]);
    if (ev_handle == handle) {
        if (event == completion_event) {
            if (verbose) {
                cout << "Service '" << service_name << "' " << describe_state(do_stop)
                        << ".\n";
            }
            rbuffer.consume(pktlen);
            return 0;
        }
        else if (event == cancelled_event) {
            if (verbose) {
                cout << "Service '" << service_name << "' " << describe_verb(do_stop)
                        << " cancelled.\n";
            }
            rbuffer.consume(pktlen);
            return 1;
        }
        else if (!do_stop && event == service_event_t::FAILEDSTART) {
            if (verbose) {
                cout << "Service '" << service_name << "' failed to start.\n";
                if (pktlen >= base_pkt_size + STATUS_BUFFER_SIZE) {
                    uint16_t launch_stage;
                    rbuffer.extract((char *)&launch_stage, base_pkt_size + 4,
                                    sizeof(uint16_t));

                    stopped_reason_t stop_reason = static_cast<stopped_reason_t>(rbuffer[base_pkt_size + 3]);
                    int exit_status;
                    int exit_si_code = 0;
                    int exit_si_status = 0;
                    rbuffer.extract((char *)&exit_status, base_pkt_size + 6, sizeof(exit_status));
                    if (rbuffer[0] == (char)cp_info::SERVICEEVENT5) {
                        exit_si_code = exit_status;
                        rbuffer.extract((char *)&exit_si_status,
                                base_pkt_size + 6 + sizeof(exit_si_code), sizeof(exit_si_status));
                    }

                    print_failure_details(stop_reason, launch_stage, exit_status, exit_si_code, exit_si_status);
                }
            }
            rbuffer.consume(pktlen);
            return 1;
        }
    }
    rbuffer.consume(pktlen);
    return -1;
}

// Wait for a service to reached stopped (do_stop == true) or started (do_stop == false) state.
// Returns 0 if the service started/stopped, 1 if start/stop was cancelled or failed.
static int wait_service_state(int socknum, cpbuffer_t &rbuffer, handle_t handle,
        const std::string &service_name, bool do_stop, bool verbose)
{
    // Wait until service started:
    int r = rbuffer.fill_to(socknum, 2);
    while (r > 0) {
        if (rbuffer[0] >= 100) {
            unsigned pktlen = (unsigned char) rbuffer[1];
            fill_buffer_to(rbuffer, socknum, pktlen);

            if (value((cp_info)rbuffer[0]).is_in(cp_info::SERVICEEVENT, cp_info::SERVICEEVENT5)) {
                int ret = process_service_event(rbuffer, pktlen, handle, service_name, do_stop, verbose);
                if (ret >= 0) {
                    return ret;
                }
            }
            else {
                rbuffer.consume(pktlen);
            }

            r = rbuffer.fill_to(socknum, 2);
        }
        else {
            // Not an information packet?
            throw dinit_protocol_error();
        }
    }

    if (r == -1) {
        perror(DINITCTL_APPNAME ": read");
    }
    else {
        throw dinit_protocol_error();
    }

    return 1;
}

// Start/stop/restart a service
static int start_stop_service(int socknum, cpbuffer_t &rbuffer, const char *service_name,
        ctl_cmd command, bool do_pin, bool do_force, bool wait_for_service, bool ignore_unstarted,
        bool verbose)
{
    using std::cout;
    using std::cerr;

    bool do_stop = (command == ctl_cmd::STOP_SERVICE || command == ctl_cmd::RELEASE_SERVICE);

    service_state_t state;
    handle_t handle;
    
    if (command != ctl_cmd::RESTART_SERVICE && command != ctl_cmd::STOP_SERVICE
            && command != ctl_cmd::RELEASE_SERVICE) {
        ignore_unstarted = false;
    }

    if (!load_service(socknum, rbuffer, service_name, &handle, &state, !ignore_unstarted)) {
        return ignore_unstarted ? 0 : 1;
    }

    service_state_t wanted_state = do_stop ? service_state_t::STOPPED : service_state_t::STARTED;
    cp_cmd pcommand;
    switch (command) {
        case ctl_cmd::STOP_SERVICE:
        case ctl_cmd::RESTART_SERVICE:  // stop, and then start
            pcommand = cp_cmd::STOPSERVICE;
            break;
        case ctl_cmd::RELEASE_SERVICE:
            pcommand = cp_cmd::RELEASESERVICE;
            break;
        case ctl_cmd::START_SERVICE:
            pcommand = cp_cmd::STARTSERVICE;
            break;
        case ctl_cmd::WAKE_SERVICE:
            pcommand = cp_cmd::WAKESERVICE;
            break;
        default:
            // can't get here (hopefully)
            pcommand = cp_cmd::STOPSERVICE;
    }

    observed_states_t seen_states;

    // Need to issue command (eg STOPSERVICE/STARTSERVICE)
    // We'll do this regardless of the current service state / target state, since issuing
    // start/stop also sets or clears the "explicitly started" flag on the service.
    // (For other commands we could potentially avoid sending the command if we had more
    // information about the service state, but it doesn't seem worth the effort to implement
    // that optimisation).
    {
        char flags = (do_pin ? 1 : 0) | ((pcommand == cp_cmd::STOPSERVICE && !do_force) ? 2 : 0);
        if (command == ctl_cmd::RESTART_SERVICE) {
            flags |= (4 | 128); // restart, pre-ack
        }

        auto m = membuf()
                .append((char) pcommand)
                .append(flags)
                .append(handle);
        write_all_x(socknum, m);

        wait_for_reply(rbuffer, socknum, handle, &seen_states);
        cp_rply reply_pkt_h = (cp_rply)rbuffer[0];
        rbuffer.consume(1); // consume header

        if (reply_pkt_h == cp_rply::PREACK) {
            // We should consider state changes seen only after the PREACK (i.e. between the
            // PREACK and the main reply):
            seen_states.started = false;
            seen_states.stopped = false;
            seen_states.failed_start = false;

            // PREACK will be followed by a 2nd reply, get that now:
            wait_for_reply(rbuffer, socknum, handle, &seen_states);
            reply_pkt_h = (cp_rply)rbuffer[0];
            rbuffer.consume(1);
        }

        if (reply_pkt_h == cp_rply::ALREADYSS) {
            bool already = (state == wanted_state);
            if (verbose) {
                if (command == ctl_cmd::RESTART_SERVICE) {
                    cout << "Service restarted.\n";
                }
                else {
                    cout << "Service " << (already ? "(already) " : "")
                            << describe_state(do_stop) << ".\n";
                }
            }
            return 0; // success!
        }
        if (reply_pkt_h == cp_rply::PINNEDSTARTED) {
            cerr << DINITCTL_APPNAME ": cannot stop service '" << service_name
                    << "' as it is pinned started\n";
            return 1;
        }
        if (reply_pkt_h == cp_rply::PINNEDSTOPPED) {
            cerr << DINITCTL_APPNAME ": cannot start service '" << service_name
                    << "' as it is pinned stopped\n";
            return 1;
        }
        if (reply_pkt_h == cp_rply::DEPENDENTS && pcommand == cp_cmd::STOPSERVICE) {
            cerr << DINITCTL_APPNAME ": cannot stop service '" << service_name
                    << "' due to the following dependents:\n";
            if (command != ctl_cmd::RESTART_SERVICE) {
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
        if (reply_pkt_h == cp_rply::NAK && command == ctl_cmd::RESTART_SERVICE) {
            if (ignore_unstarted) {
                if (verbose) {
                    cout << "Service '" << service_name << "' is not currently started.\n";
                }
                return 0;
            }
            cerr << DINITCTL_APPNAME ": cannot restart service; service not started.\n";
            return 1;
        }
        if (reply_pkt_h == cp_rply::NAK && command == ctl_cmd::WAKE_SERVICE) {
            cerr << DINITCTL_APPNAME ": service has no active dependents, cannot wake.\n";
            return 1;
        }
        if (reply_pkt_h == cp_rply::SHUTTINGDOWN) {
            cerr << DINITCTL_APPNAME ": cannot start/restart/wake service, shutdown is in progress.\n";
            return 1;
        }
        if (reply_pkt_h != cp_rply::ACK) {
            cerr << DINITCTL_APPNAME ": protocol error.\n";
            return 1;
        }
    }

    if (!wait_for_service) {
        if (verbose) {
            cout << "Issued " << describe_verb(do_stop) << " command successfully for service '"
                    <<  service_name <<  "'.\n";
        }
        return 0;
    }

    if (command == ctl_cmd::RESTART_SERVICE) {
        // for restart we want to display both "stopped" and "started" statuses
        if (seen_states.stopped) {
            if (verbose) {
                cout << "Service '" << service_name << "' stopped.\n";
            }
        }
        else if (wait_service_state(socknum, rbuffer, handle, service_name, true, verbose) != 0) {
            return EXIT_FAILURE;
        }
    }

    if (seen_states.started) {
        if (verbose) {
            cout << "Service '" << service_name << "' started.\n";
        }
    }
    else if (seen_states.failed_start) {
        if (verbose) {
            cout << "Service '" << service_name << "' failed to start.\n";
            print_failure_details(seen_states.stop_reason, 0 /* not applicable */,
                    seen_states.exit_status, seen_states.exit_si_code, seen_states.exit_si_status);
        }
    }
    return wait_service_state(socknum, rbuffer, handle, service_name, do_stop, verbose);
}

// Issue a "load service" command (LOADSERVICE), without waiting for
// a response. Returns 1 on failure (with error logged), 0 on success.
static int issue_load_service(int socknum, const char *service_name, bool find_only)
{
    // Build buffer;
    srvname_len_t srvname_len = strlen(service_name);
    int bufsize = 3 + srvname_len;
    
    std::unique_ptr<char[]> ubuf(new char[bufsize]);
    auto buf = ubuf.get();

    buf[0] = (char)(find_only ? cp_cmd::FINDSERVICE : cp_cmd::LOADSERVICE);
    memcpy(buf + 1, &srvname_len, sizeof(srvname_len));
    memcpy(buf + 3, service_name, srvname_len);

    write_all_x(socknum, buf, bufsize);
    
    return 0;
}

// Check that a "load service" reply was received, and that the requested service was found.
//   state_p may be null.
static int check_load_reply(int socknum, cpbuffer_t &rbuffer, handle_t *handle_p, service_state_t *state_p, bool write_error)
{
    using std::cout;
    using std::cerr;
    
    cp_rply reply_pkt_h = (cp_rply)rbuffer[0];
    if (reply_pkt_h == cp_rply::SERVICERECORD) {
        fill_buffer_to(rbuffer, socknum, 2 + sizeof(*handle_p));
        rbuffer.extract((char *) handle_p, 2, sizeof(*handle_p));
        if (state_p) *state_p = static_cast<service_state_t>(rbuffer[1]);
        //target_state = static_cast<service_state_t>(rbuffer[2 + sizeof(handle)]);
        rbuffer.consume(3 + sizeof(*handle_p));
        return 0;
    }
    else if (reply_pkt_h == cp_rply::NOSERVICE) {
        if (write_error) {
            cerr << DINITCTL_APPNAME ": failed to find service description.\n";
            cerr << DINITCTL_APPNAME ": check service description file exists / service name spelling.\n";
        }
        return 1;
    }
    else if (reply_pkt_h == cp_rply::SERVICE_DESC_ERR) {
        if (write_error) {
            cerr << DINITCTL_APPNAME ": error in service description.\n";
            cerr << DINITCTL_APPNAME ": try '" DINIT_CHECK_APPNAME " <service-name>' or check log"
                    " for more information.\n";
        }
        return 1;
    }
    else if (reply_pkt_h == cp_rply::SERVICE_LOAD_ERR) {
        if (write_error) {
            cerr << DINITCTL_APPNAME ": error loading service (or dependency of service).\n";
            cerr << DINITCTL_APPNAME ": try '" DINIT_CHECK_APPNAME " <service-name>' or check log"
                    " for more information.\n";
        }
        return 1;
    }
    else {
        throw dinit_protocol_error();
    }
}

static int unpin_service(int socknum, cpbuffer_t &rbuffer, const char *service_name, bool verbose)
{
    using std::cout;
    using std::cerr;

    handle_t handle;
    
    // Build buffer;
    if (!load_service(socknum, rbuffer, service_name, &handle, nullptr)) {
        return 1;
    }
    
    // Issue UNPIN command.
    {
        auto m = membuf()
                .append((char)cp_cmd::UNPINSERVICE)
                .append(handle);
        write_all_x(socknum, m);
        
        wait_for_reply(rbuffer, socknum);
        if (rbuffer[0] != (char)cp_rply::ACK) {
            cerr << DINITCTL_APPNAME ": protocol error.\n";
            return 1;
        }
        rbuffer.consume(1);
    }

    if (verbose) {
        cout << "Service '" << service_name << "' unpinned.\n";
    }
    return 0;
}

static int unload_service(int socknum, cpbuffer_t &rbuffer, const char *service_name, bool verbose)
{
    using std::cout;
    using std::cerr;

    if (issue_load_service(socknum, service_name, true) == 1) {
        return 1;
    }

    wait_for_reply(rbuffer, socknum);

    handle_t handle;

    if (rbuffer[0] == (char)cp_rply::NOSERVICE) {
        cerr << DINITCTL_APPNAME ": service not loaded.\n";
        return 1;
    }

    if (check_load_reply(socknum, rbuffer, &handle, nullptr) != 0) {
        return 1;
    }

    // Issue UNLOAD command.
    {
        auto m = membuf()
                .append((char)cp_cmd::UNLOADSERVICE)
                .append(handle);
        write_all_x(socknum, m);

        wait_for_reply(rbuffer, socknum);
        if (rbuffer[0] == (char)cp_rply::NAK) {
            cerr << DINITCTL_APPNAME ": could not unload service; service not stopped, or is a"
                    " dependency of another service.\n";
            return 1;
        }
        if (rbuffer[0] != (char)cp_rply::ACK) {
            cerr << DINITCTL_APPNAME ": protocol error.\n";
            return 1;
        }
        rbuffer.consume(1);
    }

    if (verbose) {
        cout << "Service '" << service_name << "' unloaded.\n";
    }
    return 0;
}

static int reload_service(int socknum, cpbuffer_t &rbuffer, const char *service_name, bool verbose)
{
    using std::cout;
    using std::cerr;

    if (issue_load_service(socknum, service_name, true) == 1) {
        return 1;
    }

    wait_for_reply(rbuffer, socknum);

    handle_t handle;

    if (rbuffer[0] == (char)cp_rply::NOSERVICE) {
        rbuffer.consume(1);
        // If the service isn't loaded yet at all, just do a basic load:
        if (issue_load_service(socknum, service_name, false) == 1) {
            return 1;
        }

        wait_for_reply(rbuffer, socknum);

        if (check_load_reply(socknum, rbuffer, &handle, nullptr) != 0) {
            return 1;
        }

        if (verbose) {
            cout << "Service '" << service_name << "' reloaded.\n";
        }
        return 0;
    }

    if (check_load_reply(socknum, rbuffer, &handle, nullptr) != 0) {
        return 1;
    }

    // Issue RELOAD command.
    {
        auto m = membuf()
                .append((char)cp_cmd::RELOADSERVICE)
                .append(handle);
        write_all_x(socknum, m);

        wait_for_reply(rbuffer, socknum);
        if (rbuffer[0] == (char)cp_rply::NAK) {
            cerr << DINITCTL_APPNAME ": could not reload service; service in wrong state,"
                    " incompatible change, or bad service description.\n";
            return 1;
        }
        if (rbuffer[0] != (char)cp_rply::ACK) {
            cerr << DINITCTL_APPNAME ": protocol error.\n";
            return 1;
        }
        rbuffer.consume(1);
    }

    if (verbose) {
        cout << "Service '" << service_name << "' reloaded.\n";
    }
    return 0;
}

static int list_services(int socknum, cpbuffer_t &rbuffer, uint16_t proto_version)
{
    using std::cout;
    using std::cerr;
    
    char cmdbuf[] = { (char)cp_cmd::LISTSERVICES };
    if (proto_version >= 5) {
        cmdbuf[0] = (char)cp_cmd::LISTSERVICES5;
    }
    write_all_x(socknum, cmdbuf, 1);

    unsigned status_buffer_size = proto_version < 5 ? STATUS_BUFFER_SIZE : STATUS_BUFFER5_SIZE;

    wait_for_reply(rbuffer, socknum);
    while (rbuffer[0] == (char)cp_rply::SVCINFO) {
        // Packet: SVCINFO (1), name length (1), status buffer (STATUS_BUFFER_SIZE), name (N)
        int hdrsize = 2 + status_buffer_size;
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
        int exit_si_code;
        int exit_si_status;
        if (has_pid) {
            // 8 = SVCINFO (1) + name length (1)
            //                 + current state (1) + target state (1) + flags (1)
            //                 + stop reason (1) + exec failure stage (2)
            rbuffer.extract((char *)&service_pid, 8, sizeof(service_pid));
        }
        else {
            if (proto_version < 5) {
                rbuffer.extract((char *)&exit_status, 8, sizeof(exit_status));
            }
            else {
                rbuffer.extract((char *)&exit_si_code, 8, sizeof(exit_si_code));
                rbuffer.extract((char *)&exit_si_status, 8 + sizeof(exit_si_code), sizeof(exit_si_status));
            }
        }

        fill_buffer_to(rbuffer, socknum, name_len + hdrsize);

        char *name_ptr = rbuffer.get_ptr(hdrsize);
        unsigned clength = std::min(rbuffer.get_contiguous_length(name_ptr), name_len);

        std::string name {name_ptr, clength};
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
                if (proto_version < 5) {
                    if (!WIFEXITED(exit_status) || WEXITSTATUS(exit_status) != 0) {
                        did_fail = true;
                    }
                }
                else {
                    did_fail = (exit_si_code != CLD_EXITED) || (exit_si_status != 0);
                }
            }
            else {
                did_fail = (stop_reason != stopped_reason_t::NORMAL);
            }

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
            if (proto_version < 5) {
                if (WIFEXITED(exit_status)) {
                    cout << " (exit status: " << WEXITSTATUS(exit_status) << ")";
                }
                else if (WIFSIGNALED(exit_status)) {
                    cout << " (signal: " << WTERMSIG(exit_status) << ")";
                }
            }
            else {
                if (exit_si_code == CLD_EXITED) {
                    cout << " (exit status: " << exit_si_status << ")";
                }
                else if (exit_si_code == CLD_KILLED) {
                    cout << " (signal: " << exit_si_status << ")";
                }
            }
        }

        if (has_console) {
            cout << " (has console)";
        }
        else if (waiting_console) {
            cout << " (waiting for console)";
        }

        cout << "\n";

        rbuffer.consume(hdrsize + name_len);
        wait_for_reply(rbuffer, socknum);
    }

    if (rbuffer[0] != (char)cp_rply::LISTDONE) {
        cerr << DINITCTL_APPNAME ": control socket protocol error\n";
        return 1;
    }

    return 0;
}

static int service_status(int socknum, cpbuffer_t &rbuffer, const char *service_name,
        ctl_cmd command, uint16_t proto_version, bool verbose)
{
    using std::cout;
    using std::cerr;

    bool is_status = command == ctl_cmd::SERVICE_STATUS;

    if (issue_load_service(socknum, service_name, true) == 1) {
        return 1;
    }

    wait_for_reply(rbuffer, socknum);

    handle_t handle;

    if (rbuffer[0] == (char)cp_rply::NOSERVICE) {
        if (is_status) {
            cerr << DINITCTL_APPNAME ": service not loaded.\n";
        }
        return 1;
    }

    if (check_load_reply(socknum, rbuffer, &handle, nullptr, is_status) != 0) {
        return 1;
    }

    // Issue STATUS request
    {
        char status_req_id = proto_version < 5 ? (char)cp_cmd::SERVICESTATUS : (char)cp_cmd::SERVICESTATUS5;
        unsigned status_buf_size = proto_version < 5 ? STATUS_BUFFER_SIZE : STATUS_BUFFER5_SIZE;

        auto m = membuf()
                .append(status_req_id)
                .append(handle);
        write_all_x(socknum, m);

        wait_for_reply(rbuffer, socknum);
        if (rbuffer[0] != (char)cp_rply::SERVICESTATUS) {
            cerr << DINITCTL_APPNAME ": protocol error.\n";
            return 1;
        }
        rbuffer.consume(1);

        fill_buffer_to(rbuffer, socknum, status_buf_size + 1 /* reserved */);
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
        int exit_si_code = 0;
        int exit_si_status = 0;
        if (has_pid) {
            // 6 = current state (1) + target state (1) + flags (1)
            //                       + stop reason (1) + exec failure stage (2)
            rbuffer.extract((char *)&service_pid, 6, sizeof(service_pid));
        }
        else {
            if (proto_version < 5) {
                rbuffer.extract((char *)&exit_status, 6, sizeof(exit_status));
            }
            else {
                rbuffer.extract((char *)&exit_si_code, 6, sizeof(exit_si_code));
                rbuffer.extract((char *)&exit_si_status, 6 + sizeof(int), sizeof(exit_si_status));
            }
        }

        switch (command) {
        case ctl_cmd::IS_STARTED:
        case ctl_cmd::IS_FAILED:
            if (verbose) {
                switch (current) {
                case service_state_t::STOPPED:
                    cout << "STOPPED\n";
                    break;
                case service_state_t::STARTING:
                    cout << "STARTING\n";
                    break;
                case service_state_t::STARTED:
                    cout << "STARTED\n";
                    break;
                case service_state_t::STOPPING:
                    cout << "STOPPING\n";
                }
            }
            if (command == ctl_cmd::IS_STARTED) {
                // return 0 (success) for started
                return current != service_state_t::STARTED;
            }
            // IS_FAILED:
            // return 0 (success) for specific stopped reasons
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
                if (proto_version < 5) {
                    if (exit_status != 0) {
                        cout << "; ";
                        print_termination_details(exit_status);
                    }
                }
                else {
                    if (exit_si_status != 0) {
                        cout << "; ";
                        print_termination_details(exit_si_code, exit_si_status);
                    }
                }
                cout << ")";
                break;
            case stopped_reason_t::EXECFAILED:
                uint16_t launch_stage;
                rbuffer.extract((char *)&launch_stage, 4, 2);
                if (exit_status == 0) {
                    // (Protocol version 5+)
                    exit_status = exit_si_code;
                }
                cout << " (could not be launched)\n";
                cout << "        Stage: " << exec_stage_descriptions[launch_stage] << "\n";
                cout << "        Error: " << strerror(exit_status);
                break;
            case stopped_reason_t::TERMINATED:
                cout << " (terminated";
                if (proto_version < 5) {
                    if (exit_status != 0) {
                        cout << "; ";
                        print_termination_details(exit_status);
                    }
                }
                else {
                    if (exit_si_status != 0) {
                        cout << "; ";
                        print_termination_details(exit_si_code, exit_si_status);
                    }
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
            if (proto_version < 5) {
                if (exit_status != 0) {
                    cout << "(terminated ;";
                    print_termination_details(exit_status);
                    cout << ")";
                }
            }
            else {
                if (exit_si_status != 0) {
                    cout << "(terminated ;";
                    print_termination_details(exit_si_code, exit_si_status);
                    cout << ")";
                }
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
    using std::cout;
    using std::cerr;

    handle_t from_handle;
    handle_t to_handle;

    if (!load_service(socknum, rbuffer, service_from, &from_handle, nullptr)
            || !load_service(socknum, rbuffer, service_to, &to_handle, nullptr)) {
        return 1;
    }

    if (from_handle == to_handle) {
        cerr << DINITCTL_APPNAME ": can not add/remove a dependency from a service to itself\n";
        return 1;
    }

    auto m = membuf()
            .append<char>(add ? (char)cp_cmd::ADD_DEP : (char)cp_cmd::REM_DEP)
            .append(static_cast<char>(dep_type))
            .append(from_handle)
            .append(to_handle);
    write_all_x(socknum, m);

    wait_for_reply(rbuffer, socknum);

    // check reply
    if (rbuffer[0] == (char)cp_rply::NAK) {
        if (add) {
            cerr << DINITCTL_APPNAME ": could not add dependency: circular dependency or wrong state\n";
        }
        else {
            cerr << DINITCTL_APPNAME ": no such dependency to remove\n";
        }
        return 1;
    }
    if (rbuffer[0] != (char)cp_rply::ACK) {
        cerr << DINITCTL_APPNAME ": control socket protocol error\n";
        return 1;
    }

    if (verbose) {
        std::cout << "Service '" << service_from << "': dependency '" << service_to << "' "
                << (add ? "added" : "removed") << "\n";
    }

    return 0;
}

static int shutdown_dinit(int socknum, cpbuffer_t &rbuffer, bool verbose)
{
    // TODO support no-wait option.
    using std::cout;
    using std::cerr;

    auto m = membuf()
            .append((char)cp_cmd::SHUTDOWN)
            .append(static_cast<char>(shutdown_type_t::HALT));
    write_all_x(socknum, m);

    wait_for_reply(rbuffer, socknum);

    if (rbuffer[0] != (char)cp_rply::ACK) {
        cerr << DINITCTL_APPNAME ": control socket protocol error\n";
        return 1;
    }

    if (verbose) {
        std::cout << "Shutting down dinit...\n";
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

// Get the service description directory for a loaded service
static std::string get_service_description_dir(int socknum, cpbuffer_t &rbuffer, handle_t service_handle)
{
    auto m = membuf()
            .append((char)cp_cmd::QUERYSERVICEDSCDIR)
            .append<char>(0)
            .append(service_handle);

    write_all_x(socknum, m);
    wait_for_reply(rbuffer, socknum);

    if (rbuffer[0] != (char)cp_rply::SVCDSCDIR) throw dinit_protocol_error();

    rbuffer.consume(1); // consume reply type (SVCDSCDIR)

    uint32_t sdir_len;
    fill_buffer_to(rbuffer, socknum, sizeof(uint32_t));
    rbuffer.extract(&sdir_len, 0, sizeof(uint32_t));
    rbuffer.consume(sizeof(uint32_t));

    std::string result_str;
    static_assert(sizeof(unsigned) >= sizeof(uint32_t), "");
    unsigned needed = sdir_len;

    while (needed > 0) {
        unsigned available = rbuffer.get_length();

        if (available == 0) {
            fill_some(rbuffer, socknum);
            available = rbuffer.get_length();
        }

        unsigned to_use = std::min(available, needed);
        size_t orig_len = result_str.length();
        result_str.resize(orig_len + to_use);
        rbuffer.extract(&result_str[orig_len], 0, to_use);
        rbuffer.consume(to_use);
        needed -= to_use;
    }

    if (result_str.empty()) throw dinit_protocol_error();

    return result_str;
}

// Strip any service argument ('@xxx') from the end of a service name, and return a pointer just-
// past-the-end of the service name.
static const char *strip_service_arg(const char *service_name)
{
    const char *at_ptr = service_name;
    while (*at_ptr != '\0' && *at_ptr != '@') ++at_ptr;
    return at_ptr;
}

// Find (and open) a service description file in a set of paths, together with the directory in
// which it was located.
// Parameters:
//   svc_name - the name of the service (possibly with '@<argument>' suffix).
//   paths - the list of paths to search for the service description file.
//   service_file_path (out) - contains the service description file path on return.
// Returns:
//   - The pair {-1, errno} if there was an error opening the service description file, or
//   - The pair {dir_fd, sdf_fd} on success, where 'dir_fd' is a file descriptor for the parent
//     directory and 'sdf_fd' is a descriptor for the file itself.
//   If an SDF is found (whether opened successfully or not) its path will be stored in
//   'service_file_path'.
static std::pair<int,int> find_service_desc(const char *svc_name, const std::vector<std::string> &paths,
        std::string &service_file_path)
{
    using std::cout;
    using std::cerr;

    // We use this to store a copy of the service name without argument, if needed
    std::string svc_name_store;

    // Check for service argument which must be stripped
    auto at_ptr = svc_name;
    while (*at_ptr != '\0' && *at_ptr != '@') ++at_ptr;
    if (*at_ptr == '@') {
        svc_name_store = std::string(svc_name, at_ptr - svc_name);
        svc_name = svc_name_store.c_str();
    }

    for (std::string path : paths) {
        std::string test_path = combine_paths(path, ::string_view(svc_name, at_ptr - svc_name));

        auto sdf_fds = open_with_dir(path.c_str(), svc_name);
        if (sdf_fds.first != -1 || sdf_fds.second != ENOENT) {
            service_file_path = test_path;
            return sdf_fds;
        }
    }

    return {-1, ENOENT};
}

// Scan a service description for the '@meta enable-via' directive, specifying that the service
// should be enabled "from" another particular service.
// Parameters:
//   service_name - the name of the service for which the description is to be scanned.
//   sd_file_name - the filename of the service description.
//   sd_fd - an file descriptor for reading from the service description file (i.e. sd_file_name).
//   parent_dir_fd - a file descriptor referring to the parent directory of the service
//                   description file,
// Returns:
//   The name of the service specified via the 'enable-via' directive, if any, or an empty string
//   otherwise
// Throws:
//   std::system_error, other service load exceptions
template <typename resolve_var_t>
static std::string get_enable_via(const char *service_name, const std::string &sd_file_name,
        int sd_fd, int parent_dir_fd, resolve_var_t resolve_var)
{
    using std::string;

    dio::istream sd_in(sd_fd);

    file_input_stack input_stack;
    input_stack.push(sd_file_name, std::move(sd_in), parent_dir_fd);

    string enable_via_name;

    try {
        process_service_file(service_name, input_stack, [&](string &line, file_pos_ref fpr,
                string &setting, dinit_load::setting_op_t op,
                dinit_load::string_iterator i, dinit_load::string_iterator end) -> void {
                    // Nothing to do
                },
                nullptr /* service arg */, resolve_var,
                [&enable_via_name, service_name](string::iterator i, string::iterator e) -> void {
                    i = dinit_load::skip_ws(i, e);
                    std::string meta_cmd = dinit_load::read_config_name(i, e);
                    if (meta_cmd == "enable-via") {
                        i = dinit_load::skip_ws(i, e);
                        // TODO should use read_setting_value rather than read_config_name here.
                        enable_via_name = dinit_load::read_config_name(i, e);
                        i = dinit_load::skip_ws(i, e);
                        if (enable_via_name.empty() || i != e) {
                            // TODO give filename/line number
                            throw service_load_exc(service_name, "'@meta enable-via' argument missing or malformed");
                        }
                    }
                }
            );
    }
    catch (std::system_error &err) {
        throw service_load_exc(service_name, input_stack.current_file_name() + ": "
                + strerror(errno) + "\n");
    }

    return enable_via_name;
}

// exception for cancelling a service operation
class service_op_cancel { };

static int enable_disable_service(int socknum, cpbuffer_t &rbuffer, service_dir_opt &service_dir_opts,
        const char *from, const char *to, bool enable, const char *environment_file, bool verbose,
        uint16_t proto_version)
{
    using std::cout;
    using std::cerr;
    using std::string;

    service_state_t from_state = service_state_t::STARTED;
    handle_t from_handle;
    string from_name_str; // used as backing for 'from' in some cases

    handle_t to_handle;

    std::vector<string> service_dir_paths;

    string service_file_path;
    string to_service_file_path;
    dio::istream service_file;
    fd_holder parent_dir_fd;

    if (from != nullptr && strchr(from, '@') != nullptr) {
        cerr << DINITCTL_APPNAME ": cannot enable/disable from a service with argument (service@arg).\n";
        return EXIT_FAILURE;
    }

    // Read the dinit environment, either via the running instance or from environment file
    environment dinit_env;
    dinit_env.clear_no_inherit();
    if (socknum != -1) {
        get_remote_env(socknum, rbuffer, dinit_env);
    }
    else {
        if (environment_file != nullptr && *environment_file != 0) {
            try {
                auto log_bad_setting_or_cmd = [](int line_num){};
                read_env_file_inline(environment_file, AT_FDCWD, true, dinit_env, false,
                    log_bad_setting_or_cmd, log_bad_setting_or_cmd);
            }
            catch (std::system_error &se) {
                cerr << DINITCTL_APPNAME ": cannot read environment file '" << environment_file
                        << "' :" << strerror(errno) << "\n";
                return EXIT_FAILURE;
            }
        }
    }

    auto resolve_var = [&dinit_env](const std::string &name) -> const char * {
        string_view var_and_val = dinit_env.get(name);
        if (var_and_val.empty()) return nullptr;
        auto eq_pos = std::find(var_and_val.begin(), var_and_val.end(), '=');
        if (*eq_pos == 0) return nullptr;
        return (eq_pos + 1);
    };

    if (socknum >= 0) {
        // Find 'to' service
        if (!load_service(socknum, rbuffer, to, &to_handle, nullptr)) {
            return 1;
        }

        std::string to_sdf_dir = get_service_description_dir(socknum, rbuffer, to_handle);

        to_service_file_path = to_sdf_dir;
        if (*to_service_file_path.rbegin() != '/') to_service_file_path += '/';
        const char *to_service_name_end = strip_service_arg(to);
        to_service_file_path.append(to, to_service_name_end);

        if (from == nullptr) {
            // If "from" service wasn't specified, check '@meta enable-via' in to's service
            // description, with fallback to "boot"

            auto to_sdf_fds = open_with_dir(to_sdf_dir.c_str(), to);
            if (to_sdf_fds.first == -1) {
                cerr << DINITCTL_APPNAME ": could not open service description file '"
                        << to_service_file_path << "': " << strerror(to_sdf_fds.second) << "\n";
                return EXIT_FAILURE;
            }

            int to_parent_dir_fd = to_sdf_fds.first;
            int to_sdf_fd = to_sdf_fds.second;

            try {
                from_name_str = get_enable_via(to, to_service_file_path, to_sdf_fd,
                        to_parent_dir_fd, resolve_var);
            }
            catch (service_load_exc &sle) {
                std::cerr << DINITCTL_APPNAME ": error loading " << sle.service_name << ": "
                        << sle.exc_description << "\n";
                return EXIT_FAILURE;
            }

            if (!from_name_str.empty()) {
                from = from_name_str.c_str();
            }
            else {
                from = "boot";
            }
        }

        if (!load_service(socknum, rbuffer, from, &from_handle, &from_state)) {
            return 1;
        }

        try {
            service_dir_paths = get_service_description_dirs(socknum, rbuffer);
        }
        catch (dinit_protocol_error &) {
            cerr << DINITCTL_APPNAME ": unknown configuration or protocol error, unable to load "
                    "service descriptions\n";
        }

        std::string from_sdf_dir = get_service_description_dir(socknum, rbuffer, from_handle);

        service_file_path = from_sdf_dir;
        if (*service_file_path.rbegin() != '/') service_file_path += '/';
        service_file_path += from;

        auto sdf_fds = open_with_dir(from_sdf_dir.c_str(), from);
        if (sdf_fds.first == -1) {
            cerr << DINITCTL_APPNAME ": could not open service description file '"
                    << service_file_path << "': " << strerror(sdf_fds.second) << "\n";
            return EXIT_FAILURE;
        }

        parent_dir_fd = sdf_fds.first;
        service_file.set_fd(sdf_fds.second);
    }
    else {
        // offline case
        const auto &path_list = service_dir_opts.get_paths();
        for (auto &path : path_list) {
            service_dir_paths.emplace_back(path.get_dir());
        }

        auto to_sdf_fds = find_service_desc(to, service_dir_paths, to_service_file_path);
        if (to_sdf_fds.first == -1 && to_sdf_fds.second == ENOENT) {
            cerr << DINITCTL_APPNAME ": could not locate service file for target service '" << to << "'\n";
            return EXIT_FAILURE;
        }

        if (from == nullptr) {
            if (to_sdf_fds.first == -1) {
                cerr << DINITCTL_APPNAME ": could not read service description file '"
                        << to_service_file_path << "': " << strerror(to_sdf_fds.second) << "\n";
                return EXIT_FAILURE;
            }
            try {
                from_name_str = get_enable_via(to, to_service_file_path, to_sdf_fds.second,
                        to_sdf_fds.first, resolve_var);
            }
            catch (service_load_exc &sle) {
                std::cerr << DINITCTL_APPNAME ": error loading " << sle.service_name << ": "
                        << sle.exc_description << "\n";
                return EXIT_FAILURE;
            }

            if (!from_name_str.empty()) {
                from = from_name_str.c_str();
            }
            else {
                from = "boot";
            }
        }
        else {
            if (to_sdf_fds.first != -1) {
                close(to_sdf_fds.first);
                close(to_sdf_fds.second);
            }
        }

        auto sdf_fds = find_service_desc(from, service_dir_paths, service_file_path);
        if (sdf_fds.first == -1) {
            if (sdf_fds.second == ENOENT) {
                cerr << DINITCTL_APPNAME ": could not locate service file for service '" << from << "'\n";
            }
            else {
                cerr << DINITCTL_APPNAME ": could not open service description file '"
                        << service_file_path << "': " << strerror(sdf_fds.second)
                        << "\n";
            }
            return EXIT_FAILURE;
        }

        parent_dir_fd = sdf_fds.first;
        service_file.set_fd(sdf_fds.second);
    }

    // We now need to read the service file, identify the waits-for.d directory (bail out if more than one),
    // make sure the service is not listed as a dependency individually.

    string waits_for_d;
    fd_holder waits_for_d_fd;

    file_input_stack input_stack;
    input_stack.push(service_file_path, std::move(service_file), parent_dir_fd.release());

    try {
        auto is_setting = [](const std::string &setting, dinit_load::setting_id_t setting_id) {
            return (setting == dinit_load::all_settings[(size_t)setting_id].setting_str);
        };

        process_service_file(from, input_stack, [&](string &line, file_pos_ref fpr,
                string &setting, dinit_load::setting_op_t op,
                dinit_load::string_iterator i, dinit_load::string_iterator end) -> void {
                    using namespace dinit_load;

                    if (is_setting(setting, setting_id_t::WAITS_FOR)
                            || is_setting(setting, setting_id_t::DEPENDS_ON)
                            || is_setting(setting, setting_id_t::DEPENDS_MS)) {
                        string dname = dinit_load::read_setting_value(fpr, i, end);
                        if (dname == to) {
                            // There is already a dependency
                            cerr << DINITCTL_APPNAME ": there is a fixed dependency to service '"
                                    << to << "' in the service description of '" << from
                                    << "'.\n";
                            throw service_op_cancel();
                        }
                    }
                    else if (is_setting(setting, setting_id_t::WAITS_FOR_D)) {
                        string dname = dinit_load::read_setting_value(fpr, i, end);
                        if (!waits_for_d.empty()) {
                            cerr << DINITCTL_APPNAME ": service '" << from << "' has multiple "
                                    "waits-for.d directories specified in service description\n";
                            throw service_op_cancel();
                        }
                        waits_for_d = std::move(dname);
                    }
                },
                nullptr /* service arg */, resolve_var);
    }
    catch (const service_op_cancel &cexc) {
        return 1;
    }
    catch (std::system_error &err) {
        cerr << DINITCTL_APPNAME ": " << input_stack.current_file_name() << ": " << strerror(errno) << "\n";
        return 1;
    }

    // If the from service has no waits-for.d specified, we can't continue
    if (waits_for_d.empty()) {
        cerr << DINITCTL_APPNAME ": service '" << from
                << "' has no waits-for.d directory specified" << "\n";
        return 1;
    }

    // The waits-for.d path is relative to the service file path, combine:
    // XXX should use parent dir fd
    string waits_for_d_full = combine_paths(parent_path(service_file_path), waits_for_d.c_str());

    // check if dependency already exists
    string dep_link_path = combine_paths(waits_for_d_full, to);
    struct stat stat_buf;
    if (lstat(dep_link_path.c_str(), &stat_buf) == -1) {
        if (errno != ENOENT) {
            cerr << DINITCTL_APPNAME ": checking for existing dependency link: "
                    << dep_link_path << ": " << strerror(errno) << "\n";
            return 1;
        }
    }
    else {
        // dependency already exists
        if (enable) {
            cerr << DINITCTL_APPNAME ": service already enabled.\n";
            return 1;
        }
    }

    if (socknum >= 0) {
        // warn if 'from' service is not started
        if (enable && from_state != service_state_t::STARTED) {
            cerr << DINITCTL_APPNAME ": warning: enabling dependency for non-started service\n";
        }

        // add/remove dependency
        constexpr int enable_pktsize = 2 + sizeof(handle_t) * 2;
        char cmdbuf[enable_pktsize] = { char(enable ? cp_cmd::ENABLESERVICE : cp_cmd::REM_DEP),
                char(dependency_type::WAITS_FOR)};
        memcpy(cmdbuf + 2, &from_handle, sizeof(from_handle));
        memcpy(cmdbuf + 2 + sizeof(from_handle), &to_handle, sizeof(to_handle));
        write_all_x(socknum, cmdbuf, enable_pktsize);

        wait_for_reply(rbuffer, socknum);

        // check reply
        if (rbuffer[0] == (char)cp_rply::NAK) {
            if (enable) {
                cerr << DINITCTL_APPNAME ": could not enable service: possible circular dependency\n";
            }
            else {
                cerr << DINITCTL_APPNAME ": service not currently enabled\n";
            }
            return 1;
        }
        if (rbuffer[0] != (char)cp_rply::ACK) {
            cerr << DINITCTL_APPNAME ": control socket protocol error\n";
            return 1;
        }
        rbuffer.consume(1);
    }

    // create link
    if (enable) {
        // Guess a relative path to use as the symlink target. Note that if either the waits-for.d directory
        // path, or the "to" service description file path, contains symbolic links or path segments with
        // ".." or ".", this may not produce the "correct" result. We do a "reasonable effort". If the
        // result isn't correct, the only consequence is that the link may be broken or point to the wrong
        // file; Dinit itself won't be affected by this.
        string symlink_target;
        auto spos = waits_for_d_full.rfind('/');
        if (spos != std::string::npos) {
            if (strncmp(waits_for_d_full.c_str(), to_service_file_path.c_str(), spos) == 0) {
                symlink_target = "../";
                symlink_target.append(to_service_file_path, spos + 1, std::string::npos);
            }
        }
        if (symlink_target.empty()) {
            // not yet determined: try full path or just "../(to)"
            if (to_service_file_path[0] == '/') {
                symlink_target = to_service_file_path;
            }
            else {
                symlink_target = "../";
                symlink_target.append(to);
            }
        }

        if (symlink(symlink_target.c_str(), dep_link_path.c_str()) == -1) {
            cerr << DINITCTL_APPNAME ": could not create symlink at " << dep_link_path << ": "
                    << strerror(errno);
            if (socknum >= 0) {
                cerr << "\n" DINITCTL_APPNAME ": note: service was enabled for now; persistent "
                        "enable failed.";
            }
            cerr << "\n";
            return 1;
        }
    }
    else {
        if (unlink(dep_link_path.c_str()) == -1) {
            cerr << DINITCTL_APPNAME ": could not unlink dependency entry " << dep_link_path
                    << ": " << strerror(errno);
            if (socknum >= 0) {
                cerr << "\n" DINITCTL_APPNAME ": note: service was disabled for now; persistent "
                        "disable failed.";
            }
            cerr << "\n";
            return 1;
        }
    }

    if (socknum >= 0) {
        if (verbose) {
            cout << "Service '" << to << "' has been " << (enable ? "enabled" : "disabled") << ".\n";
        }

        char cmd_pkt = (char)(proto_version < 5 ? cp_cmd::SERVICESTATUS : cp_cmd::SERVICESTATUS5);

        // Check status of the service now
        auto m = membuf()
                .append(cmd_pkt)
                .append(to_handle);
        write_all_x(socknum, m);

        int statussize = proto_version < 5 ? STATUS_BUFFER_SIZE : STATUS_BUFFER5_SIZE;

        // For an enable, we want to wait until the service has started so we can report any
        // failure. But, if the service is already started, we won't get any service events, so we
        // have to request status via SERVICESTATUS to catch that case. However, we may get
        // a service event before the reply to SERVICESTATUS and in that case should use it to
        // report status.
        if (enable) {
            int r = rbuffer.fill_to(socknum, 2);
            while (r > 0 && rbuffer[0] >= 100) {
                unsigned pktlen = (unsigned char) rbuffer[1];
                fill_buffer_to(rbuffer, socknum, pktlen);

                if (value(rbuffer[0]).is_in((char)cp_info::SERVICEEVENT, (char)cp_info::SERVICEEVENT5)) {
                    int ret = process_service_event(rbuffer, pktlen, to_handle, to,
                            false /* start */, verbose);
                    if (ret >= 0) {
                        // Consume the outstanding reply packet before returning.
                        wait_for_reply(rbuffer, socknum);
                        if (rbuffer[0] != (char)cp_rply::SERVICESTATUS) {
                            cerr << DINITCTL_APPNAME ": protocol error.\n";
                            return 1;
                        }
                        // +2 is 1 byte packet type, 1 byte reserved
                        fill_buffer_to(rbuffer, socknum, statussize + 2);
                        rbuffer.consume(statussize + 2);
                        return ret;
                    }
                }
                else {
                    rbuffer.consume(pktlen);
                }

                r = rbuffer.fill_to(socknum, 2);
            }
            if (r == -1) {
                throw cp_read_exception(errno);
            }
            if (r == 0) {
                cerr << DINITCTL_APPNAME ": protocol error.\n";
                return 1;
            }
        }
        else {
            wait_for_reply(rbuffer, socknum);
        }

        if (rbuffer[0] != (char)cp_rply::SERVICESTATUS) {
            cerr << DINITCTL_APPNAME ": protocol error.\n";
            return 1;
        }
        rbuffer.consume(1);

        fill_buffer_to(rbuffer, socknum, statussize + 1 /* reserved */);
        rbuffer.consume(1);
        service_state_t current = static_cast<service_state_t>(rbuffer[0]);
        service_state_t target = static_cast<service_state_t>(rbuffer[1]);
        rbuffer.consume(statussize);

        if (enable) {
            if (current != service_state_t::STARTED) {
                wait_service_state(socknum, rbuffer, to_handle, to, false /* start */, verbose);
            }
        }
        else {
            if (target != service_state_t::STOPPED) {
                std::cerr << DINITCTL_APPNAME ": note: disabled service may have other dependents\n";
            }
        }
    }
    else {
        if (verbose) {
            cout << "Service '" << to << "' has been " << (enable ? "enabled" : "disabled") << ".\n";
        }
    }

    return 0;
}

static int do_setenv(int socknum, cpbuffer_t &rbuffer, std::vector<const char *> &env_names, bool unset)
{
    using std::cerr;
    using std::string;

    string buf;

    for (const char *envp : env_names) {
        envvar_len_t envvar_len;
        buf.clear();
        buf.reserve(6);
        // protocol message and size space
        buf.push_back((char)cp_cmd::SETENV);
        buf.append(sizeof(envvar_len), 0);
        const unsigned hdr_len = 1 + sizeof(envvar_len);
        // either full var or name
        auto elen = strlen(envp);
        buf.append(envp, elen);
        // if '=' not found, get value from environment, except for unset
        auto eq = memchr(envp, '=', elen);
        if (!eq && !unset) {
            buf.push_back('=');
            auto *envv = getenv(envp);
            if (envv) {
                buf.append(envv);
            }
        }
        else if (eq && unset) {
            cerr << DINITCTL_APPNAME ": environment variable '" << envp
                    << "' must not contain the '=' sign.\n";
            return 1;
        }
        envvar_len = buf.size() - hdr_len;
        // sanitize length early on
        if (buf.size() > cpbuffer_t::get_size()) {
            auto eq = buf.find('=', hdr_len);
            auto name = buf.substr(hdr_len, eq - hdr_len);
            cerr << DINITCTL_APPNAME ": environment variable '" << name << "' too long.\n";
            return 1;
        }
        // set size in protocol message
        memcpy(&buf[1], &envvar_len, sizeof(envvar_len));
        // send
        write_all_x(socknum, buf.data(), buf.size());
        wait_for_reply(rbuffer, socknum);
        if (rbuffer[0] == (char)cp_rply::BADREQ) {
            cerr << DINITCTL_APPNAME ": failed to export environment.\n";
            return 1;
        }
        else if (rbuffer[0] != (char)cp_rply::ACK) {
            throw dinit_protocol_error();
        }
        rbuffer.consume(1);
    }

    return 0;
}

static int trigger_service(int socknum, cpbuffer_t &rbuffer, const char *service_name, bool trigger_value)
{
    using std::cerr;

    handle_t handle;
    if (!load_service(socknum, rbuffer, service_name, &handle, nullptr, true)) {
        return 1;
    }

    // Issue SET_TRIGGER command.
    {
        auto m = membuf()
                .append((char)cp_cmd::SETTRIGGER)
                .append(handle)
                .append<trigger_val_t>(trigger_value);
        write_all_x(socknum, m);

        wait_for_reply(rbuffer, socknum);
        if (rbuffer[0] == (char)cp_rply::NAK) {
            cerr << DINITCTL_APPNAME ": cannot trigger a service that is not of 'triggered' type.\n";
            return 1;
        }
        if (rbuffer[0] != (char)cp_rply::ACK) {
            cerr << DINITCTL_APPNAME ": protocol error.\n";
            return 1;
        }
        rbuffer.consume(1);
    }

    return 0;
}

static int signal_send(int socknum, cpbuffer_t &rbuffer, const char *service_name, sig_num_t sig_num)
{
    using std::cerr;

    handle_t handle;

    if (!load_service(socknum, rbuffer, service_name, &handle, nullptr, true)) {
        return 1;
    }

    // Issue SIGNAL command.
    auto m = membuf()
            .append((char)cp_cmd::SIGNAL)
            .append(sig_num)
            .append(handle);
    write_all_x(socknum, m);

    wait_for_reply(rbuffer, socknum);
    cp_rply reply_pkt_h = (cp_rply)rbuffer[0];
    if (reply_pkt_h == cp_rply::SIGNAL_NOPID) {
        cerr << DINITCTL_APPNAME ": could not get valid PID of service; service is not "
                "process-based or is in wrong state.\n";
        return 1;
    }
    if (reply_pkt_h == cp_rply::SIGNAL_BADSIG) {
        cerr << DINITCTL_APPNAME ": provided signal was invalid.\n";
        return 1;
    }
    if (reply_pkt_h == cp_rply::SIGNAL_KILLERR) {
        cerr << DINITCTL_APPNAME ": failed sending signal to service.\n";
        return 1;
    }
    if (reply_pkt_h != cp_rply::ACK) {
        cerr << DINITCTL_APPNAME ": protocol error.\n";
        return 1;
    }
    rbuffer.consume(1);
    return 0;
}

static int signal_list()
{
    using std::cout;
    using namespace dinit_load;

    cout << DINITCTL_APPNAME ": The following signal names are supported:";
    int skip_none = 0;
    for (const auto &signal: signal_to_int_map) {
        if (skip_none < 2) {
            skip_none += 1;
        }
        else {
            cout << "\n" DINITCTL_APPNAME ": ";
            string sigpad = signal.first;
            sigpad.resize(5,' ');
            cout << sigpad << "-> " << signal.second;
        }
    }
    cout << "\n";
    return 0;
}

static int cat_service_log(int socknum, cpbuffer_t &rbuffer, const char *service_name, bool do_clear)
{
    using std::cout;
    using std::cerr;

    handle_t handle;
    if (!load_service(socknum, rbuffer, service_name, &handle, nullptr, true)) {
        return 1;
    }

    char flags = do_clear ? 1 : 0;

    // Issue CATLOG
    auto m = membuf()
             .append((char)cp_cmd::CATLOG)
             .append<char>(flags)
             .append(handle);
    write_all_x(socknum, m);

    wait_for_reply(rbuffer, socknum);
    if (rbuffer[0] == (char)cp_rply::NAK) {
        cerr << DINITCTL_APPNAME ": cannot cat log for service not configured to buffer output.\n";
        return 1;
    }
    if (rbuffer[0] != (char)cp_rply::SERVICE_LOG) {
        cerr << DINITCTL_APPNAME ": protocol error.\n";
        return 1;
    }

    fill_buffer_to(rbuffer, socknum, 2 + sizeof(unsigned));
    unsigned bufsize;
    rbuffer.extract(&bufsize, 2, sizeof(unsigned));
    rbuffer.consume(2 + sizeof(unsigned));

    // output the log
    if (bufsize > 0) {
        cout << std::flush;

        bool trailing_nl = false;
        char output_buf[cpbuffer_t::get_size()];
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
