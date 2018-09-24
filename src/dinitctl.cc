#include <cstdio>
#include <cstddef>
#include <cstring>
#include <string>
#include <iostream>
#include <system_error>
#include <memory>
#include <algorithm>

#include <sys/types.h>
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

// dinitctl:  utility to control the Dinit daemon, including starting and stopping of services.

// This utility communicates with the dinit daemon via a unix stream socket (/dev/initctl,
// or $HOME/.dinitctl).

static constexpr uint16_t min_cp_version = 1;
static constexpr uint16_t max_cp_version = 1;

enum class command_t;

static int issue_load_service(int socknum, const char *service_name, bool find_only = false);
static int check_load_reply(int socknum, cpbuffer_t &, handle_t *handle_p, service_state_t *state_p);
static int start_stop_service(int socknum, cpbuffer_t &, const char *service_name, command_t command,
        bool do_pin, bool wait_for_service, bool verbose);
static int unpin_service(int socknum, cpbuffer_t &, const char *service_name, bool verbose);
static int unload_service(int socknum, cpbuffer_t &, const char *service_name);
static int list_services(int socknum, cpbuffer_t &);
static int shutdown_dinit(int soclknum, cpbuffer_t &);
static int add_remove_dependency(int socknum, cpbuffer_t &rbuffer, bool add, char *service_from,
        char *service_to, dependency_type dep_type);


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
    RELEASE_SERVICE,
    UNPIN_SERVICE,
    UNLOAD_SERVICE,
    LIST_SERVICES,
    SHUTDOWN,
    ADD_DEPENDENCY,
    RM_DEPENDENCY
};


// Entry point.
int main(int argc, char **argv)
{
    using namespace std;
    
    bool show_help = argc < 2;
    char *service_name = nullptr;
    char *to_service_name = nullptr;
    dependency_type dep_type;
    bool dep_type_set = false;
    
    std::string control_socket_str;
    const char * control_socket_path = nullptr;
    
    bool verbose = true;
    bool sys_dinit = false;  // communicate with system daemon
    bool wait_for_service = true;
    bool do_pin = false;
    
    command_t command = command_t::NONE;
        
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "--help") == 0) {
                show_help = true;
                break;
            }
            else if (strcmp(argv[i], "--no-wait") == 0) {
                wait_for_service = false;
            }
            else if (strcmp(argv[i], "--quiet") == 0) {
                verbose = false;
            }
            else if (strcmp(argv[i], "--system") == 0 || strcmp(argv[i], "-s") == 0) {
                sys_dinit = true;
            }
            else if (strcmp(argv[i], "--pin") == 0) {
                do_pin = true;
            }
            else {
                cerr << "dinitctl: unrecognized option: " << argv[i] << " (use --help for help)\n";
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
            else if (strcmp(argv[i], "release") == 0) {
                command = command_t::RELEASE_SERVICE;
            }
            else if (strcmp(argv[i], "unpin") == 0) {
                command = command_t::UNPIN_SERVICE;
            }
            else if (strcmp(argv[i], "unload") == 0) {
                command = command_t::UNLOAD_SERVICE;
            }
            else if (strcmp(argv[i], "list") == 0) {
                command = command_t::LIST_SERVICES;
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
            else {
                if (service_name != nullptr) {
                    show_help = true;
                    break;
                }
                service_name = argv[i];
                // TODO support multiple services
            }
        }
    }
    
    bool no_service_cmd = (command == command_t::LIST_SERVICES || command == command_t::SHUTDOWN);

    if (service_name != nullptr && no_service_cmd) {
        show_help = true;
    }
    
    if ((service_name == nullptr && ! no_service_cmd) || command == command_t::NONE) {
        show_help = true;
    }

    if ((command == command_t::ADD_DEPENDENCY || command == command_t::RM_DEPENDENCY)
            && (! dep_type_set || service_name == nullptr || to_service_name == nullptr)) {
        show_help = true;
    }

    if (show_help) {
        cout << "dinitctl:   control Dinit services\n"
          "\n"
          "Usage:\n"
          "    dinitctl [options] start [options] <service-name>\n"
          "    dinitctl [options] stop [options] <service-name>\n"
          "    dinitctl [options] wake [options] <service-name>\n"
          "    dinitctl [options] release [options] <service-name>\n"
          "    dinitctl [options] unpin <service-name>\n"
          "    dinitctl unload <service-name>\n"
          "    dinitctl list\n"
          "    dinitctl shutdown\n"
          "    dinitctl add-dep <type> <from-service> <to-service>\n"
          "    dinitctl rm-dep <type> <from-service> <to-service>\n"
          "\n"
          "Note: An activated service continues running when its dependents stop.\n"
          "\n"
          "General options:\n"
          "  -s, --system     : control system daemon instead of user daemon\n"
          "  --quiet          : suppress output (except errors)\n"
          "\n"
          "Command options:\n"
          "  --help           : show this help\n"
          "  --no-wait        : don't wait for service startup/shutdown to complete\n"
          "  --pin            : pin the service in the requested state\n";
        return 1;
    }
    
    signal(SIGPIPE, SIG_IGN);
    
    control_socket_path = "/dev/dinitctl";
    
    // Locate control socket
    if (! sys_dinit) {
        char * userhome = getenv("HOME");
        if (userhome == nullptr) {
            struct passwd * pwuid_p = getpwuid(getuid());
            if (pwuid_p != nullptr) {
                userhome = pwuid_p->pw_dir;
            }
        }
        
        if (userhome != nullptr) {
            control_socket_str = userhome;
            control_socket_str += "/.dinitctl";
            control_socket_path = control_socket_str.c_str();
        }
        else {
            cerr << "Cannot locate user home directory (set HOME or check /etc/passwd file)" << endl;
            return 1;
        }
    }
    
    int socknum = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socknum == -1) {
        perror("dinitctl: socket");
        return 1;
    }

    struct sockaddr_un * name;
    uint sockaddr_size = offsetof(struct sockaddr_un, sun_path) + strlen(control_socket_path) + 1;
    name = (struct sockaddr_un *) malloc(sockaddr_size);
    if (name == nullptr) {
        cerr << "dinitctl: Out of memory" << endl;
        return 1;
    }
    
    name->sun_family = AF_UNIX;
    strcpy(name->sun_path, control_socket_path);
    
    int connr = connect(socknum, (struct sockaddr *) name, sockaddr_size);
    if (connr == -1) {
        perror("dinitctl: connect");
        return 1;
    }
    
    try {
        // Start by querying protocol version:
        cpbuffer_t rbuffer;
        check_protocol_version(min_cp_version, max_cp_version, rbuffer, socknum);

        if (command == command_t::UNPIN_SERVICE) {
            return unpin_service(socknum, rbuffer, service_name, verbose);
        }
        else if (command == command_t::UNLOAD_SERVICE) {
            return unload_service(socknum, rbuffer, service_name);
        }
        else if (command == command_t::LIST_SERVICES) {
            return list_services(socknum, rbuffer);
        }
        else if (command == command_t::SHUTDOWN) {
            return shutdown_dinit(socknum, rbuffer);
        }
        else if (command == command_t::ADD_DEPENDENCY || command == command_t::RM_DEPENDENCY) {
            return add_remove_dependency(socknum, rbuffer, command == command_t::ADD_DEPENDENCY,
                    service_name, to_service_name, dep_type);
        }
        else {
            return start_stop_service(socknum, rbuffer, service_name, command, do_pin,
                    wait_for_service, verbose);
        }
    }
    catch (cp_old_client_exception &e) {
        std::cerr << "dinitctl: too old (server reports newer protocol version)" << std::endl;
        return 1;
    }
    catch (cp_old_server_exception &e) {
        std::cerr << "dinitctl: server too old or protocol error" << std::endl;
        return 1;
    }
    catch (cp_read_exception &e) {
        cerr << "dinitctl: control socket read failure or protocol error" << endl;
        return 1;
    }
    catch (cp_write_exception &e) {
        cerr << "dinitctl: control socket write error: " << std::strerror(e.errcode) << endl;
        return 1;
    }
}

// Extract/read a string of specified length from the buffer/socket. The string is consumed
// from the buffer.
static std::string read_string(int socknum, cpbuffer_t &rbuffer, uint32_t length)
{
    int rb_len = rbuffer.get_length();
    if (rb_len >= length) {
        std::string r = rbuffer.extract_string(0, length);
        rbuffer.consume(length);
        return r;
    }

    std::string r = rbuffer.extract_string(0, rb_len);
    uint32_t rlen = length - rb_len;
    uint32_t clen;
    do {
        rbuffer.reset();
        rbuffer.fill(socknum);
        char *bptr = rbuffer.get_ptr(0);
        clen = rbuffer.get_length();
        clen = std::min(clen, rlen);
        r.append(bptr, clen);
        rlen -= clen;
    } while (rlen > 0);

    rbuffer.consume(clen);

    return r;
}

// Start/stop a service
static int start_stop_service(int socknum, cpbuffer_t &rbuffer, const char *service_name,
        command_t command, bool do_pin, bool wait_for_service, bool verbose)
{
    using namespace std;

    bool do_stop = (command == command_t::STOP_SERVICE || command == command_t::RELEASE_SERVICE);
    
    if (issue_load_service(socknum, service_name)) {
        return 1;
    }

    // Now we expect a reply:
    
    wait_for_reply(rbuffer, socknum);

    service_state_t state;
    //service_state_t target_state;
    handle_t handle;

    if (check_load_reply(socknum, rbuffer, &handle, &state) != 0) {
        return 1;
    }

    service_state_t wanted_state = do_stop ? service_state_t::STOPPED : service_state_t::STARTED;
    int pcommand = 0;
    switch (command) {
        case command_t::STOP_SERVICE:
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
        char buf[2 + sizeof(handle)];
        buf[0] = pcommand;
        buf[1] = do_pin ? 1 : 0;
        memcpy(buf + 2, &handle, sizeof(handle));
        write_all_x(socknum, buf, 2 + sizeof(handle));
        
        wait_for_reply(rbuffer, socknum);
        if (rbuffer[0] == DINIT_RP_ALREADYSS) {
            bool already = (state == wanted_state);
            if (verbose) {
                cout << "Service " << (already ? "(already) " : "")
                        << describeState(do_stop) << "." << endl;
            }
            return 0; // success!
        }
        if (rbuffer[0] != DINIT_RP_ACK) {
            cerr << "dinitctl: Protocol error." << endl;
            return 1;
        }
        rbuffer.consume(1);
    }

    if (! wait_for_service) {
        if (verbose) {
            cout << "Issued " << describeVerb(do_stop) << " command successfully." << endl;
        }
        return 0;
    }

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
            int pktlen = (unsigned char) rbuffer[1];
            fill_buffer_to(rbuffer, socknum, pktlen);

            if (rbuffer[0] == DINIT_IP_SERVICEEVENT) {
                handle_t ev_handle;
                rbuffer.extract((char *) &ev_handle, 2, sizeof(ev_handle));
                service_event_t event = static_cast<service_event_t>(rbuffer[2 + sizeof(ev_handle)]);
                if (ev_handle == handle) {
                    if (event == completionEvent) {
                        if (verbose) {
                            cout << "Service " << describeState(do_stop) << "." << endl;
                        }
                        return 0;
                    }
                    else if (event == cancelledEvent) {
                        if (verbose) {
                            cout << "Service " << describeVerb(do_stop) << " cancelled." << endl;
                        }
                        return 1;
                    }
                    else if (! do_stop && event == service_event_t::FAILEDSTART) {
                        if (verbose) {
                            cout << "Service failed to start." << endl;
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
            cerr << "dinitctl: protocol error" << endl;
            return 1;
        }
    }

    if (r == -1) {
        perror("dinitctl: read");
    }
    else {
        cerr << "protocol error (connection closed by server)" << endl;
    }
    return 1;
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
static int check_load_reply(int socknum, cpbuffer_t &rbuffer, handle_t *handle_p, service_state_t *state_p)
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
        cerr << "dinitctl: failed to find/load service." << endl;
        return 1;
    }
    else {
        cerr << "dinitctl: protocol error." << endl;
        return 1;
    }
}

static int unpin_service(int socknum, cpbuffer_t &rbuffer, const char *service_name, bool verbose)
{
    using namespace std;
    
    // Build buffer;
    if (issue_load_service(socknum, service_name) == 1) {
        return 1;
    }

    // Now we expect a reply:
    
    wait_for_reply(rbuffer, socknum);

    handle_t handle;

    if (check_load_reply(socknum, rbuffer, &handle, nullptr) != 0) {
        return 1;
    }

    // Issue UNPIN command.
    {
        char buf[1 + sizeof(handle)];
        buf[0] = DINIT_CP_UNPINSERVICE;
        memcpy(buf + 1, &handle, sizeof(handle));
        write_all_x(socknum, buf, 2 + sizeof(handle));
        
        wait_for_reply(rbuffer, socknum);
        if (rbuffer[0] != DINIT_RP_ACK) {
            cerr << "dinitctl: protocol error." << endl;
            return 1;
        }
        rbuffer.consume(1);
    }

    if (verbose) {
        cout << "Service unpinned." << endl;
    }
    return 0;
}

static int unload_service(int socknum, cpbuffer_t &rbuffer, const char *service_name)
{
    using namespace std;

    // Build buffer;
    if (issue_load_service(socknum, service_name, true) == 1) {
        return 1;
    }

    // Now we expect a reply:
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
        char buf[1 + sizeof(handle)];
        buf[0] = DINIT_CP_UNLOADSERVICE;
        memcpy(buf + 1, &handle, sizeof(handle));
        write_all_x(socknum, buf, 2 + sizeof(handle));

        wait_for_reply(rbuffer, socknum);
        if (rbuffer[0] == DINIT_RP_NAK) {
            cerr << "dinitctl: Could not unload service; service not stopped, or is a dependency of "
                    "other service." << endl;
            return 1;
        }
        if (rbuffer[0] != DINIT_RP_ACK) {
            cerr << "dinitctl: Protocol error." << endl;
            return 1;
        }
        rbuffer.consume(1);
    }

    cout << "Service unloaded." << endl;
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
        int nameLen = rbuffer[1];
        service_state_t current = static_cast<service_state_t>(rbuffer[2]);
        service_state_t target = static_cast<service_state_t>(rbuffer[3]);

        int console_flags = rbuffer[4];
        bool has_console = (console_flags & 2) != 0;
        bool waiting_console = (console_flags & 1) != 0;
        bool was_skipped = (console_flags & 4) != 0;

        stopped_reason_t stop_reason = static_cast<stopped_reason_t>(rbuffer[5]);

        pid_t service_pid;
        int exit_status;
        if (current != service_state_t::STOPPED) {
            rbuffer.extract((char *)&service_pid, 8, sizeof(service_pid));
        }
        else {
        	rbuffer.extract((char *)&exit_status, 8, sizeof(exit_status));
        }

        fill_buffer_to(rbuffer, socknum, nameLen + hdrsize);

        char *name_ptr = rbuffer.get_ptr(hdrsize);
        int clength = std::min(rbuffer.get_contiguous_length(name_ptr), nameLen);

        string name = string(name_ptr, clength);
        name.append(rbuffer.get_buf_base(), nameLen - clength);

        cout << "[";

        cout << (target  == service_state_t::STARTED ? "{" : " ");
        if (current == service_state_t::STARTED) {
            cout << (was_skipped ? "s" : "+");
        }
        else {
            cout << " ";
        }
        cout << (target  == service_state_t::STARTED ? "}" : " ");
        
        if (current == service_state_t::STARTING) {
            cout << "<<";
        }
        else if (current == service_state_t::STOPPING) {
            cout << ">>";
        }
        else {
            cout << "  ";
        }
        
        cout << (target  == service_state_t::STOPPED ? "{" : " ");
        if (current == service_state_t::STOPPED) {
            bool did_fail = false;
            if (stop_reason == stopped_reason_t::TERMINATED) {
                if (!WIFEXITED(exit_status) || WEXITSTATUS(exit_status) != 0) {
                    did_fail = true;
                }
            }
            else did_fail = (stop_reason != stopped_reason_t::NORMAL);

            cout << (did_fail ? "X" : "-");
        }
        else {
        	cout << " ";
        }
        cout << (target == service_state_t::STOPPED ? "}" : " ");

        cout << "] " << name;

        if (current != service_state_t::STOPPED && service_pid != -1) {
        	cout << " (pid: " << service_pid << ")";
        }
        
        if (current == service_state_t::STOPPED && stop_reason == stopped_reason_t::TERMINATED) {
            if (WIFEXITED(exit_status)) {
                cout << " (exit status: " << WEXITSTATUS(exit_status) << ")";
            }
            else if (WIFSIGNALED(exit_status)) {
                cout << " (signal: " << WSTOPSIG(exit_status) << ")";
            }
        }

        if (has_console) {
        	cout << " (has console)";
        }
        else if (waiting_console) {
        	cout << " (waiting for console)";
        }

        cout << endl;

        rbuffer.consume(hdrsize + nameLen);
        wait_for_reply(rbuffer, socknum);
    }

    if (rbuffer[0] != DINIT_RP_LISTDONE) {
        cerr << "dinitctl: Control socket protocol error" << endl;
        return 1;
    }

    return 0;
}

static int add_remove_dependency(int socknum, cpbuffer_t &rbuffer, bool add, char *service_from,
        char *service_to, dependency_type dep_type)
{
    using namespace std;

    // First find the "from" service:
    if (issue_load_service(socknum, service_from, false) == 1) {
        return 1;
    }

    wait_for_reply(rbuffer, socknum);

    handle_t from_handle;

    if (check_load_reply(socknum, rbuffer, &from_handle, nullptr) != 0) {
        return 1;
    }

    // Then find or load the "to" service:
    if (issue_load_service(socknum, service_to, false) == 1) {
        return 1;
    }

    wait_for_reply(rbuffer, socknum);

    handle_t to_handle;

    if (check_load_reply(socknum, rbuffer, &to_handle, nullptr) != 0) {
        return 1;
    }

    constexpr int pktsize = 2 + sizeof(handle_t) * 2;
    char cmdbuf[pktsize] = { add ? (char)DINIT_CP_ADD_DEP : (char)DINIT_CP_REM_DEP, (char)dep_type};
    memcpy(cmdbuf + 2, &from_handle, sizeof(from_handle));
    memcpy(cmdbuf + 2 + sizeof(from_handle), &to_handle, sizeof(to_handle));
    write_all_x(socknum, cmdbuf, pktsize);

    wait_for_reply(rbuffer, socknum);

    // check reply
    if (rbuffer[0] == DINIT_RP_NAK) {
        cerr << "dinitctl: Could not add dependency: circular dependency or wrong state" << endl;
        return 1;
    }
    if (rbuffer[0] != DINIT_RP_ACK) {
        cerr << "dinitctl: Control socket protocol error" << endl;
        return 1;
    }

    return 0;
}

static int shutdown_dinit(int socknum, cpbuffer_t &rbuffer)
{
    // TODO support no-wait option.
    using namespace std;

    // Build buffer;
    constexpr int bufsize = 2;
    char buf[bufsize];

    buf[0] = DINIT_CP_SHUTDOWN;
    buf[1] = static_cast<char>(shutdown_type_t::HALT);

    write_all_x(socknum, buf, bufsize);

    wait_for_reply(rbuffer, socknum);

    if (rbuffer[0] != DINIT_RP_ACK) {
        cerr << "dinitctl: Control socket protocol error" << endl;
        return 1;
    }

    // Now wait for rollback complete:
    try {
        while (true) {
            wait_for_info(rbuffer, socknum);
            if (rbuffer[0] == DINIT_ROLLBACK_COMPLETED) {
                break;
            }
        }
    }
    catch (cp_read_exception &exc) {
        // Dinit can terminate before replying: let's assume that happened.
        // TODO: better check, possibly ensure that dinit actually sends rollback complete before
        // termination.
    }

    return 0;
}
