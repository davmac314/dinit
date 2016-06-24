#include <cstdio>
#include <cstddef>
#include <cstring>
#include <string>
#include <iostream>
#include <system_error>
#include <memory>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>
#include <pwd.h>

#include "control-cmds.h"
#include "service-constants.h"
#include "cpbuffer.h"

// dinitctl:  utility to control the Dinit daemon, including starting and stopping of services.

// This utility communicates with the dinit daemon via a unix stream socket (/dev/initctl, or $HOME/.dinitctl).

using handle_t = uint32_t;


class ReadCPException
{
    public:
    int errcode;
    ReadCPException(int err) : errcode(err) { }
};

enum class Command;

static int issueLoadService(int socknum, const char *service_name);
static int checkLoadReply(int socknum, CPBuffer<1024> &rbuffer, handle_t *handle_p, ServiceState *state_p);
static int startStopService(int socknum, const char *service_name, Command command, bool do_pin, bool wait_for_service, bool verbose);
static int unpinService(int socknum, const char *service_name, bool verbose);
static int listServices(int socknum);


// Fill a circular buffer from a file descriptor, reading at least _rlength_ bytes.
// Throws ReadException if the requested number of bytes cannot be read, with:
//     errcode = 0   if end of stream (remote end closed)
//     errcode = errno   if another error occurred
// Note that EINTR is ignored (i.e. the read will be re-tried).
static void fillBufferTo(CPBuffer<1024> *buf, int fd, int rlength)
{
    do {
        int r = buf->fill_to(fd, rlength);
        if (r == -1) {
            if (errno != EINTR) {
                throw ReadCPException(errno);
            }
        }
        else if (r == 0) {
            throw ReadCPException(0);
        }
        else {
            return;
        }
    }
    while (true);
}

static const char * describeState(bool stopped)
{
    return stopped ? "stopped" : "started";
}

static const char * describeVerb(bool stop)
{
    return stop ? "stop" : "start";
}

// Wait for a reply packet, skipping over any information packets
// that are received in the meantime.
static void wait_for_reply(CPBuffer<1024> &rbuffer, int fd)
{
    fillBufferTo(&rbuffer, fd, 1);
    
    while (rbuffer[0] >= 100) {
        // Information packet; discard.
        fillBufferTo(&rbuffer, fd, 1);
        int pktlen = (unsigned char) rbuffer[1];
        
        rbuffer.consume(1);  // Consume one byte so we'll read one byte of the next packet
        fillBufferTo(&rbuffer, fd, pktlen);
        rbuffer.consume(pktlen - 1);
    }
}


// Write *all* the requested buffer and re-try if necessary until
// the buffer is written or an unrecoverable error occurs.
static int write_all(int fd, const void *buf, size_t count)
{
    const char *cbuf = static_cast<const char *>(buf);
    int w = 0;
    while (count > 0) {
        int r = write(fd, cbuf, count);
        if (r == -1) {
            if (errno == EINTR) continue;
            return r;
        }
        w += r;
        cbuf += r;
        count -= r;
    }
    return w;
}


enum class Command {
    NONE,
    START_SERVICE,
    WAKE_SERVICE,
    STOP_SERVICE,
    RELEASE_SERVICE,
    UNPIN_SERVICE,
    LIST_SERVICES
};

// Entry point.
int main(int argc, char **argv)
{
    using namespace std;
    
    bool show_help = argc < 2;
    char *service_name = nullptr;
    
    std::string control_socket_str;
    const char * control_socket_path = nullptr;
    
    bool verbose = true;
    bool sys_dinit = false;  // communicate with system daemon
    bool wait_for_service = true;
    bool do_pin = false;
    
    Command command = Command::NONE;
        
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
                return 1;
            }
        }
        else if (command == Command::NONE) {
            if (strcmp(argv[i], "start") == 0) {
                command = Command::START_SERVICE; 
            }
            else if (strcmp(argv[i], "wake") == 0) {
                command = Command::WAKE_SERVICE;
            }
            else if (strcmp(argv[i], "stop") == 0) {
                command = Command::STOP_SERVICE;
            }
            else if (strcmp(argv[i], "release") == 0) {
                command = Command::RELEASE_SERVICE;
            }
            else if (strcmp(argv[i], "unpin") == 0) {
                command = Command::UNPIN_SERVICE;
            }
            else if (strcmp(argv[i], "list") == 0) {
                command = Command::LIST_SERVICES;
            }
            else {
                show_help = true;
                break;
            }
        }
        else {
            // service name
            if (service_name != nullptr) {
                show_help = true;
                break;
            }
            service_name = argv[i];
            // TODO support multiple services
        }
    }
    
    if (service_name != nullptr && command == Command::LIST_SERVICES) {
        show_help = true;
    }
    
    if ((service_name == nullptr && command != Command::LIST_SERVICES) || command == Command::NONE) {
        show_help = true;
    }

    if (show_help) {
        cout << "dinitctl:   control Dinit services" << endl;
        
        cout << "\nUsage:" << endl;
        cout << "    dinitctl [options] start [options] <service-name> : start and activate service" << endl;
        cout << "    dinitctl [options] stop [options] <service-name>  : stop service and cancel explicit activation" << endl;
        cout << "    dinitctl [options] wake [options] <service-name>  : start but do not mark activated" << endl;
        cout << "    dinitctl [options] release [options] <service-name> : release activation, stop if no dependents" << endl;
        cout << "    dinitctl [options] unpin <service-name>           : un-pin the service (after a previous pin)" << endl;
        cout << "    dinitctl list                                     : list loaded services" << endl;
        
        cout << "\nNote: An activated service continues running when its dependents stop." << endl;
        
        cout << "\nGeneral options:" << endl;
        cout << "  -s, --system     : control system daemon instead of user daemon" << endl;
        cout << "  --quiet          : suppress output (except errors)" << endl;
        
        cout << "\nCommand options:" << endl;
        cout << "  --help           : show this help" << endl;
        cout << "  --no-wait        : don't wait for service startup/shutdown to complete" << endl;
        cout << "  --pin            : pin the service in the requested (started/stopped) state" << endl;
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
    
    // TODO should start by querying protocol version
    
    if (command == Command::UNPIN_SERVICE) {
        return unpinService(socknum, service_name, verbose);
    }
    else if (command == Command::LIST_SERVICES) {
        return listServices(socknum);
    }

    return startStopService(socknum, service_name, command, do_pin, wait_for_service, verbose);
}

// Start/stop a service
static int startStopService(int socknum, const char *service_name, Command command, bool do_pin, bool wait_for_service, bool verbose)
{
    using namespace std;

    bool do_stop = (command == Command::STOP_SERVICE || command == Command::RELEASE_SERVICE);
    
    if (issueLoadService(socknum, service_name)) {
        return 1;
    }

    // Now we expect a reply:
    
    try {
        CPBuffer<1024> rbuffer;
        wait_for_reply(rbuffer, socknum);
        
        ServiceState state;
        //ServiceState target_state;
        handle_t handle;
        
        if (checkLoadReply(socknum, rbuffer, &handle, &state) != 0) {
            return 0;
        }
                
        ServiceState wanted_state = do_stop ? ServiceState::STOPPED : ServiceState::STARTED;
        int pcommand = 0;
        switch (command) {
        case Command::STOP_SERVICE:
            pcommand = DINIT_CP_STOPSERVICE;
            break;
        case Command::RELEASE_SERVICE:
            pcommand = DINIT_CP_RELEASESERVICE;
            break;
        case Command::START_SERVICE:
            pcommand = DINIT_CP_STARTSERVICE;
            break;
        case Command::WAKE_SERVICE:
            pcommand = DINIT_CP_WAKESERVICE;
            break;
        default: ;
        }
        
        // Need to issue STOPSERVICE/STARTSERVICE
        // We'll do this regardless of the current service state / target state, since issuing
        // start/stop also sets or clears the "explicitly started" flag on the service.
        {
            int r;
            
            {
                auto buf = new char[2 + sizeof(handle)];
                unique_ptr<char[]> ubuf(buf);
                
                buf[0] = pcommand;
                buf[1] = do_pin ? 1 : 0;
                memcpy(buf + 2, &handle, sizeof(handle));
                r = write_all(socknum, buf, 2 + sizeof(handle));
            }
            
            if (r == -1) {
                perror("dinitctl: write");
                return 1;
            }
            
            wait_for_reply(rbuffer, socknum);
            if (rbuffer[0] == DINIT_RP_ALREADYSS) {
                bool already = (state == wanted_state);
                if (verbose) {
                    cout << "Service " << (already ? "(already) " : "") << describeState(do_stop) << "." << endl;
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
        
        ServiceEvent completionEvent;
        ServiceEvent cancelledEvent;
        
        if (do_stop) {
            completionEvent = ServiceEvent::STOPPED;
            cancelledEvent = ServiceEvent::STOPCANCELLED;
        }
        else {
            completionEvent = ServiceEvent::STARTED;
            cancelledEvent = ServiceEvent::STARTCANCELLED;
        }
        
        // Wait until service started:
        int r = rbuffer.fill_to(socknum, 2);
        while (r > 0) {
            if (rbuffer[0] >= 100) {
                int pktlen = (unsigned char) rbuffer[1];
                fillBufferTo(&rbuffer, socknum, pktlen);
                
                if (rbuffer[0] == DINIT_IP_SERVICEEVENT) {
                    handle_t ev_handle;
                    rbuffer.extract((char *) &ev_handle, 2, sizeof(ev_handle));
                    ServiceEvent event = static_cast<ServiceEvent>(rbuffer[2 + sizeof(ev_handle)]);
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
                        else if (! do_stop && event == ServiceEvent::FAILEDSTART) {
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
    catch (ReadCPException &exc) {
        cerr << "dinitctl: control socket read failure or protocol error" << endl;
        return 1;
    }
    catch (std::bad_alloc &exc) {
        cerr << "dinitctl: out of memory" << endl;
        return 1;
    }
    
    return 0;
}

// Issue a "load service" command (DINIT_CP_LOADSERVICE), without waiting for
// a response. Returns 1 on failure (with error logged), 0 on success.
static int issueLoadService(int socknum, const char *service_name)
{
    using namespace std;
    
    // Build buffer;
    uint16_t sname_len = strlen(service_name);
    int bufsize = 3 + sname_len;
    int r;
    
    {
        // TODO: new: catch exception
        unique_ptr<char[]> ubuf(new char[bufsize]);
        auto buf = ubuf.get();
        
        buf[0] = DINIT_CP_LOADSERVICE;
        memcpy(buf + 1, &sname_len, 2);
        memcpy(buf + 3, service_name, sname_len);
        
        r = write_all(socknum, buf, bufsize);
    }
    
    if (r == -1) {
        perror("dinitctl: write");
        return 1;
    }
    
    return 0;
}

// Check that a "load service" reply was received, and that the requested service was found.
static int checkLoadReply(int socknum, CPBuffer<1024> &rbuffer, handle_t *handle_p, ServiceState *state_p)
{
    using namespace std;
    
    if (rbuffer[0] == DINIT_RP_SERVICERECORD) {
        fillBufferTo(&rbuffer, socknum, 2 + sizeof(*handle_p));
        rbuffer.extract((char *) handle_p, 2, sizeof(*handle_p));
        if (state_p) *state_p = static_cast<ServiceState>(rbuffer[1]);
        //target_state = static_cast<ServiceState>(rbuffer[2 + sizeof(handle)]);
        rbuffer.consume(3 + sizeof(*handle_p));
        return 0;
    }
    else if (rbuffer[0] == DINIT_RP_NOSERVICE) {
        cerr << "dinitctl: Failed to find/load service." << endl;
        return 1;
    }
    else {
        cerr << "dinitctl: Protocol error." << endl;
        return 1;
    }
}

static int unpinService(int socknum, const char *service_name, bool verbose)
{
    using namespace std;
    
    // Build buffer;
    if (issueLoadService(socknum, service_name) == 1) {
        return 1;
    }

    // Now we expect a reply:
    
    try {
        CPBuffer<1024> rbuffer;
        wait_for_reply(rbuffer, socknum);
        
        handle_t handle;
        
        if (checkLoadReply(socknum, rbuffer, &handle, nullptr) != 0) {
            return 1;
        }
        
        // Issue UNPIN command.
        {
            int r;
            
            {
                char *buf = new char[1 + sizeof(handle)];
                unique_ptr<char[]> ubuf(buf);
                buf[0] = DINIT_CP_UNPINSERVICE;
                memcpy(buf + 1, &handle, sizeof(handle));
                r = write_all(socknum, buf, 2 + sizeof(handle));
            }
            
            if (r == -1) {
                perror("dinitctl: write");
                return 1;
            }
            
            wait_for_reply(rbuffer, socknum);
            if (rbuffer[0] != DINIT_RP_ACK) {
                cerr << "dinitctl: Protocol error." << endl;
                return 1;
            }
            rbuffer.consume(1);
        }
    }
    catch (ReadCPException &exc) {
        cerr << "dinitctl: Control socket read failure or protocol error" << endl;
        return 1;
    }
    catch (std::bad_alloc &exc) {
        cerr << "dinitctl: Out of memory" << endl;
        return 1;
    }
    
    if (verbose) {
        cout << "Service unpinned." << endl;
    }
    return 0;
}

static int listServices(int socknum)
{
    using namespace std;
    
    try {
        char cmdbuf[] = { (char)DINIT_CP_LISTSERVICES };
        int r = write_all(socknum, cmdbuf, 1);
        
        if (r == -1) {
            perror("dinitctl: write");
            return 1;
        }
        
        CPBuffer<1024> rbuffer;
        wait_for_reply(rbuffer, socknum);
        while (rbuffer[0] == DINIT_RP_SVCINFO) {
            fillBufferTo(&rbuffer, socknum, 8);
            int nameLen = rbuffer[1];
            ServiceState current = static_cast<ServiceState>(rbuffer[2]);
            ServiceState target = static_cast<ServiceState>(rbuffer[3]);
            
            fillBufferTo(&rbuffer, socknum, nameLen + 8);
            
            char *name_ptr = rbuffer.get_ptr(8);
            int clength = std::min(rbuffer.get_contiguous_length(name_ptr), nameLen);
            
            string name = string(name_ptr, clength);
            name.append(rbuffer.get_buf_base(), nameLen - clength);
            
            cout << "[";
            
            cout << (target  == ServiceState::STARTED ? "{" : " ");
            cout << (current == ServiceState::STARTED ? "+" : " ");
            cout << (target  == ServiceState::STARTED ? "}" : " ");
            
            if (current == ServiceState::STARTING) {
                cout << "<<";
            }
            else if (current == ServiceState::STOPPING) {
                cout << ">>";
            }
            else {
                cout << "  ";
            }
            
            cout << (target  == ServiceState::STOPPED ? "{" : " ");
            cout << (current == ServiceState::STOPPED ? "-" : " ");
            cout << (target  == ServiceState::STOPPED ? "}" : " ");
            
            cout << "] " << name << endl;
            
            rbuffer.consume(8 + nameLen);
            wait_for_reply(rbuffer, socknum);
        }
        
        if (rbuffer[0] != DINIT_RP_LISTDONE) {
            cerr << "dinitctl: Control socket protocol error" << endl;
            return 1;
        }
    }
    catch (ReadCPException &exc) {
        cerr << "dinitctl: Control socket read failure or protocol error" << endl;
        return 1;
    }
    catch (std::bad_alloc &exc) {
        cerr << "dinitctl: Out of memory" << endl;
        return 1;
    }
    
    return 0;
}
