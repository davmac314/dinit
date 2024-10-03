#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <cstring>
#include <csignal>
#include <cstdio>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "dinit-client.h"
#include "service-constants.h"
#include "control-datatypes.h"

// dinit-monitor: watch service states and report them via execution of a notification command

// common communication datatypes
using namespace dinit_cptypes;

static constexpr uint16_t min_cp_version = 1;
static constexpr uint16_t max_cp_version = 5;

struct stringview {
    const char *str;
    size_t len;
};

static std::vector<stringview> split_command(const char *cmd);
static bool load_service(int socknum, cpbuffer_t &rbuffer, const char *name, handle_t *handle,
        service_state_t *state);
static void request_environ(int socknum, cpbuffer_t &rbuffer, uint16_t proto_version);
static size_t get_allenv(int socknum, cpbuffer_t &rbuffer);

// dummy handler, so we can wait for children
static void sigchld_handler(int) { }
static void issue_command(const char* name, const char* value, const char* event, std::vector<stringview> &command_parts, bool is_env = false);
static size_t read_var_and_issue(int socknum, cpbuffer_t &rbuffer, size_t dsz, const std::unordered_set<std::string> &varset, std::string &enval, std::vector<stringview> &command_parts, bool &issued, const char* str_set, const char* str_unset);

int dinit_monitor_main(int argc, char **argv)
{
    bool show_help = argc < 2;
    std::string control_socket_str;
    const char *control_socket_path = nullptr;
    bool user_dinit = (getuid() != 0);  // communicate with user daemon
    bool issue_init = false;  // request initial service state
    bool use_environ = false;  // listening on activation environment changes
    bool exit_after = false;  // exit after first issued command
    const char *str_started = "started";
    const char *str_stopped = "stopped";
    const char *str_failed = "failed";
    const char *str_set = "set";
    const char *str_unset = "unset";

    const char *command_str = nullptr;
    std::vector<const char *> services;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "--help") == 0) {
                show_help = true;
                break;
            }
            else if (strcmp(argv[i], "--version") == 0) {
                std::cout << "Dinit version " << DINIT_VERSION << ".\n";
                return 0;
            }
            else if (strcmp(argv[i], "--exit") == 0 || strcmp(argv[i], "-e") == 0) {
                exit_after = true;
            }
            else if (strcmp(argv[i], "--env") == 0 || strcmp(argv[i], "-E") == 0) {
                use_environ = true;
            }
            else if (strcmp(argv[i], "--system") == 0 || strcmp(argv[i], "-s") == 0) {
                user_dinit = false;
            }
            else if (strcmp(argv[i], "--user") == 0 || strcmp(argv[i], "-u") == 0) {
                user_dinit = true;
            }
            else if (strcmp(argv[i], "--initial") == 0 || strcmp(argv[i], "-i") == 0) {
                issue_init = true;
            }
            else if (strcmp(argv[i], "--socket-path") == 0 || strcmp(argv[i], "-p") == 0) {
                ++i;
                if (i == argc) {
                    std::cerr << "dinit-monitor: --socket-path/-p should be followed by socket path\n";
                    return 1;
                }
                control_socket_str = argv[i];
            }
            else if (strcmp(argv[i], "--str-started") == 0) {
                ++i;
                if (i == argc) {
                    std::cerr << "dinit-monitor: --str-started should be followed by an argument\n";
                    return 1;
                }
                str_started = argv[i];
            }
            else if (strcmp(argv[i], "--str-stopped") == 0) {
                ++i;
                if (i == argc) {
                    std::cerr << "dinit-monitor: --str-stopped should be followed by an argument\n";
                    return 1;
                }
                str_stopped = argv[i];
            }
            else if (strcmp(argv[i], "--str-failed") == 0) {
                ++i;
                if (i == argc) {
                    std::cerr << "dinit-monitor: --str-failed should be followed by an argument\n";
                    return 1;
                }
                str_failed = argv[i];
            }
            else if (strcmp(argv[i], "--str-set") == 0) {
                ++i;
                if (i == argc) {
                    std::cerr << "dinit-monitor: --str-set should be followed by an argument\n";
                    return 1;
                }
                str_set = argv[i];
            }
            else if (strcmp(argv[i], "--str-unset") == 0) {
                ++i;
                if (i == argc) {
                    std::cerr << "dinit-monitor: --str-unset should be followed by an argument\n";
                    return 1;
                }
                str_unset = argv[i];
            }
            else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--command") == 0) {
                ++i;
                if (i == argc) {
                    std::cerr << "dinit-monitor: --command/-c should be followed by command\n";
                    return 1;
                }
                command_str = argv[i];
            }
        }
        else {
            services.emplace_back(argv[i]);
        }
    }

    if (show_help) {
        std::cout << "dinit-monitor:   monitor Dinit services\n"
                "\n"
                "Usage:\n"
                "    dinit-monitor [options] <service-names|environ-names...>\n"
                "\n"
                "Options:\n"
                "  --help           : show this help\n"
                "  -e, --exit       : exit after the first issued command\n"
                "  -E, --env        : monitor activation environment changes\n"
                "  -s, --system     : monitor system daemon (default if run as root)\n"
                "  -u, --user       : monitor user daemon\n"
                "  -i, --initial    : also execute command for initial service state\n"
                "  --str-started <started-text>\n"
                "                   : specify text describing status when service starts\n"
                "  --str-stopped <stopped-text>\n"
                "                   : specify text describing status when service stops\n"
                "  --str-failed <failed-text>\n"
                "                   : specify text describing status when service fails\n"
                "  --socket-path <path>, -p <path>\n"
                "                   : specify socket for communication with daemon\n"
                "  -c, --command    : specify command to execute on service status change\n"
                "                     (%n for service name, %s for status)\n";
        return 1;
    }

    if (services.empty() && !use_environ) {
        std::cerr << "dinit-monitor: specify at least one service name\n";
        return 1;
    }

    if (command_str == nullptr) {
        std::cerr << "dinit-monitor: command must specified\n";
        return 1;
    }

    std::vector<stringview> command_parts = split_command(command_str);
    if (command_parts.size() == 0) {
        std::cerr << "dinit-monitor: specified command is empty\n";
        return 1;
    }

    // Ignore SIGPIPE to avoid dying due to it, and set up a SIGCHLD handler (but mask it)
    sigset_t signal_mask;
    sigprocmask(SIG_SETMASK, nullptr, &signal_mask);
    sigaddset(&signal_mask, SIGCHLD);
    sigprocmask(SIG_SETMASK, &signal_mask, nullptr);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, sigchld_handler);

    // Locate control socket
    if (! control_socket_str.empty()) {
        control_socket_path = control_socket_str.c_str();
    }
    else {
        control_socket_path = get_default_socket_path(control_socket_str, user_dinit);
        if (control_socket_path == nullptr) {
            std::cerr << "dinit-monitor: cannot locate user home directory (set XDG_RUNTIME_DIR, "
                    "HOME, check /etc/passwd file, or specify socket path via -p)\n";
            return 1;
        }
    }

    try {
        int socknum = connect_to_daemon(control_socket_path);

        // Start by querying protocol version:
        cpbuffer_t rbuffer;
        uint16_t protocol_ver = check_protocol_version(min_cp_version, max_cp_version, rbuffer, socknum);

        // Load all services
        std::unordered_map<handle_t, const char *> service_map;
        std::unordered_set<std::string> environ_set;
        std::vector<std::pair<const char *, service_state_t>> service_init_state;
        std::string env_value;

        for (const char *service_name : services) {

            handle_t hndl;
            service_state_t state;

            if (use_environ) {
                environ_set.emplace(service_name);
                continue;
            }

            if (!load_service(socknum, rbuffer, service_name, &hndl, &state)) {
                std::cerr << "dinit-monitor: cannot load service: " << service_name << "\n";
                return 1;
            }

            service_map.emplace(hndl, service_name);
            service_init_state.push_back(std::make_pair(service_name, state));
        }

        if (use_environ) {
            // Request listening on environ events
            request_environ(socknum, rbuffer, protocol_ver);
            if (issue_init) {
                // Get the whole block and see if it's already set
                auto envsz = get_allenv(socknum, rbuffer);
                while (envsz > 0) {
                    bool issued;
                    envsz = read_var_and_issue(socknum, rbuffer, envsz, environ_set, env_value, command_parts, issued, str_set, str_unset);
                    if (issued && exit_after) {
                        return 0;
                    }
                }
            }
        }
        else if (issue_init) {
            // Issue initial status commands if requested
            for (auto state : service_init_state ) {
                if (state.second == service_state_t::STARTED) {
                    issue_command(state.first, nullptr, str_started, command_parts);
                    if (exit_after) {
                        return 0;
                    }
                }
                else if (state.second == service_state_t::STOPPED) {
                    issue_command(state.first, nullptr, str_stopped, command_parts);
                    if (exit_after) {
                        return 0;
                    }
                }
            }
        }

        // Watch information packets; execute notification

        int r = rbuffer.fill_to(socknum, 2);
        while (r > 0) {
            if (rbuffer[0] >= 100) {
                int pktlen = (unsigned char) rbuffer[1];
                fill_buffer_to(rbuffer, socknum, pktlen);

                if (use_environ && rbuffer[0] == (char)cp_info::ENVEVENT) {
                    envvar_len_t envln;
                    rbuffer.extract((char *) &envln, 3, sizeof(envln));
                    rbuffer.consume(pktlen);
                    // this will return 0, we don't want to consume after this
                    bool issued;
                    pktlen = read_var_and_issue(socknum, rbuffer, envln, environ_set, env_value, command_parts, issued, str_set, str_unset);
                    if (issued && exit_after) {
                        return 0;
                    }
                }
                else if (!use_environ && rbuffer[0] == (char)cp_info::SERVICEEVENT) {
                    handle_t ev_handle;
                    rbuffer.extract((char *) &ev_handle, 2, sizeof(ev_handle));
                    service_event_t event = static_cast<service_event_t>(rbuffer[2 + sizeof(ev_handle)]);

                    const char *service_name = service_map.at(ev_handle);
                    const char *event_str;

                    if (event == service_event_t::STARTED) {
                        event_str = str_started;
                    }
                    else if (event == service_event_t::STOPPED) {
                        event_str = str_stopped;
                    }
                    else if (event == service_event_t::FAILEDSTART) {
                        event_str = str_failed;
                    }
                    else {
                        goto consume_packet;
                    }
                    issue_command(service_name, nullptr, event_str, command_parts);
                    if (exit_after) {
                        return 0;
                    }
                }

                consume_packet:

                rbuffer.consume(pktlen);
                r = rbuffer.fill_to(socknum, 2);
            }
            else {
                // Not an information packet?
                std::cerr << "dinit-monitor: protocol error\n";
                return 1;
            }
        }

        if (r == -1) {
            perror("dinit-monitor: read");
        }
        else {
            std::cerr << "dinit-monitor: connection closed by server\n";
        }
        return 1;
    }
    catch (cp_old_client_exception &e) {
        std::cerr << "dinit-monitor: too old (server reports newer protocol version)\n";
    }
    catch (cp_old_server_exception &e) {
        std::cerr << "dinit-monitor: server too old or protocol error\n";
    }
    catch (cp_read_exception &e) {
        std::cerr << "dinit-monitor: control socket read failure or protocol error\n";
    }
    catch (cp_write_exception &e) {
        std::cerr << "dinit-monitor: control socket write error: " << std::strerror(e.errcode) << "\n";
    }
    catch (dinit_protocol_error &e) {
        std::cerr << "dinit-monitor: protocol error\n";
    }
    catch (general_error &ge) {
        std::cerr << "dinit-monitor";
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


static void issue_command(const char* name, const char* value, const char* event, std::vector<stringview> &command_parts, bool is_env) {
    std::vector<std::string> final_cmd_parts;
    std::vector<const char *> final_cmd_parts_cstr;

    for (stringview cmd_part : command_parts) {
        std::string cmd_part_str;
        cmd_part_str.reserve(cmd_part.len);

        for (size_t i = 0; i < cmd_part.len; ++i) {
            if (cmd_part.str[i] == '%') {
                ++i;
                if (i >= cmd_part.len) {
                    cmd_part_str.append(1, '%');
                    break;
                }
                if (cmd_part.str[i] == 'n') {
                    cmd_part_str.append(name);
                }
                else if (cmd_part.str[i] == 'v') {
                    if (value) cmd_part_str.append(value);
                }
                else if (cmd_part.str[i] == 's') {
                    cmd_part_str.append(event);
                }
                else {
                    // invalid specifier, just output as is
                    cmd_part_str.append(1, '%');
                    cmd_part_str.append(1, cmd_part_str[i]);
                }
            }
            else if (cmd_part.str[i] == '"') {
                // consume.
            }
            else {
                cmd_part_str.append(1, cmd_part.str[i]);
            }
        }

        final_cmd_parts.emplace_back(std::move(cmd_part_str));
    }

    for (const std::string &cmd_part : final_cmd_parts) {
        final_cmd_parts_cstr.push_back(cmd_part.c_str());
    }
    final_cmd_parts_cstr.push_back(nullptr);

    pid_t child_pid = fork();
    if (child_pid == 0) {
        // we are the child
        char **argv = const_cast<char **>(final_cmd_parts_cstr.data());
        execvp(argv[0], argv);
        perror("dinit-monitor: exec");
    }
    else if (child_pid == -1) {
        perror("dinit-monitor: fork");
    }
    else {
        int wstatus;
        if (wait(&wstatus) != -1) {
            if (wstatus != 0) {
                if (WIFEXITED(wstatus)) {
                    std::cerr << "dinit-monitor: notification command terminated with exit status "
                            << WEXITSTATUS(wstatus) << "\n";
                }
                if (WIFSIGNALED(wstatus)) {
                    std::cerr << "dinit-monitor: notification command terminated due to signal "
                            << WTERMSIG(wstatus) << "\n";
                }
            }
        }

        // Don't bother clearing any pending SIGCHLD. POSIX says that:
        // - either SIGCHLD doesn't queue, in which case we're only leaving one pending signal
        // - or, it does queue, but wait() removes it from the queue.
    }

}

int main(int argc, char **argv) {
    try {
        return dinit_monitor_main(argc, argv);
    }
    catch (std::bad_alloc &e) {
        std::cerr << "dinit-monitor: out of memory\n";
    }
    return 1;
}

static std::vector<stringview> split_command(const char *cmd)
{
    using std::locale;
    using std::isspace;
    const locale &classic_loc = locale::classic();
    std::vector<stringview> result;

    const char *c = cmd;

    do {
        while (*c != '\0' && isspace(*c, classic_loc)) ++c;
        if (*c == '\0') break;
        const char *str = c;

        while (*c != '\0' && !isspace(*c, classic_loc)) {
            if (*c == '"') {
                ++c; // skip begin quote
                while (*c != '\0' && *c != '"') ++c;
            }
            ++c;
        }

        size_t len = c - str;

        result.emplace_back(stringview {str, len});
    } while (*c != '\0');

    return result;
}

// Issue a "load service" command (LOADSERVICE), without waiting for
// a response. Returns 1 on failure (with error logged), 0 on success.
static int issue_load_service(int socknum, const char *service_name, bool find_only = false)
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
static int check_load_reply(int socknum, cpbuffer_t &rbuffer, handle_t *handle_p, service_state_t *state_p)
{
    using namespace std;

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
        return 1;
    }
    else {
        throw dinit_protocol_error();
    }
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
        service_state_t *state)
{
    // Load 'to' service:
    if (issue_load_service(socknum, name)) {
        return false;
    }

    wait_for_reply(rbuffer, socknum);

    if (check_load_reply(socknum, rbuffer, handle, state) != 0) {
        return false;
    }

    return true;
}

static void request_environ(int socknum, cpbuffer_t &rbuffer, uint16_t proto_version)
{
    char c = (char)cp_cmd::LISTENENV;

    if (proto_version < 5) {
        throw cp_old_server_exception();
    }

    write_all_x(socknum, &c, 1);

    wait_for_reply(rbuffer, socknum);

    cp_rply reply_pkt_h = (cp_rply)rbuffer[0];
    if (reply_pkt_h != cp_rply::ACK) {
        throw dinit_protocol_error();
    }
    rbuffer.consume(1);
}

// get the whole environment block of the dinit instance in a way
// that leaves individual variables available for read, without
// the packet header; that allows read_var_and_issue to be used
static size_t get_allenv(int socknum, cpbuffer_t &rbuffer)
{
    char buf[2] = { (char)cp_cmd::GETALLENV, 0 };
    write_all_x(socknum, buf, 2);

    wait_for_reply(rbuffer, socknum);

    cp_rply reply_pkt_h = (cp_rply)rbuffer[0];
    if (reply_pkt_h != cp_rply::ALLENV) {
        throw dinit_protocol_error();
    }

    // 1-byte packet header, then size_t
    constexpr size_t allenv_hdr_size = 1 + sizeof(size_t);
    rbuffer.fill_to(socknum, allenv_hdr_size);

    size_t dsize;
    rbuffer.extract(&dsize, 1, sizeof(dsize));
    rbuffer.consume(allenv_hdr_size);

    return dsize;
}

static bool issue_var(std::string &envar, const std::unordered_set<std::string> &varset, std::vector<stringview> &command_parts,
        const char* str_set, const char* str_unset)
{
    auto eq = envar.find('=');
    if (eq == envar.npos) {
        /* unset */
        eq = envar.size();
    }
    auto *sp = &envar[0];
    sp[eq] = '\0';
    if (varset.empty() || varset.find(sp) != varset.end()) {
        if (eq == envar.size()) {
            issue_command(sp, nullptr, str_unset, command_parts, true);
        }
        else {
            issue_command(sp, &sp[eq + 1], str_set, command_parts, true);
        }
        return true;
    }
    return false;
}

static size_t read_var_and_issue(int socknum, cpbuffer_t &rbuffer, size_t dsz, const std::unordered_set<std::string> &varset,
        std::string &enval, std::vector<stringview> &command_parts, bool &issued, const char* str_set, const char *str_unset)
{
    enval.clear();
    issued = false;
    while (dsz > 0) {
        auto colen = rbuffer.get_contiguous_length(rbuffer.get_ptr(0));
        auto chlen = std::min((size_t)colen, dsz);
        for (unsigned i = 0; i < chlen; ++i) {
            if (rbuffer[i] != '\0') {
                continue;
            }
            enval.append(rbuffer.get_ptr(0), rbuffer.get_ptr(0) + i);
            rbuffer.consume(i + 1);
            issued = issue_var(enval, varset, command_parts, str_set, str_unset);
            return dsz - i - 1;
        }
        // copy what we have so far and fill some more
        enval.append(rbuffer.get_ptr(0), rbuffer.get_ptr(0) + chlen);
        rbuffer.consume(chlen);
        dsz -= chlen;
        if (dsz == 0) {
            // didn't find null terminator, malformed
            throw dinit_protocol_error();
        }
        if (rbuffer.get_length() == 0) {
            fill_some(rbuffer, socknum);
        }
    }
    // unreachable
    throw dinit_protocol_error();
}
