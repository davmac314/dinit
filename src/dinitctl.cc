#include <cstdio>
#include <cstddef>
#include <cstring>
#include <string>
#include <iostream>
#include <system_error>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <pwd.h>

#include "control-cmds.h"
#include "service-constants.h"
#include "cpbuffer.h"

// dinitctl:  utility to control the Dinit daemon, including starting and stopping of services.

// This utility communicates with the dinit daemon via a unix socket (/dev/initctl).

using handle_t = uint32_t;


class ReadCPException
{
    public:
    int errcode;
    ReadCPException(int err) : errcode(err) { }
};

static void fillBufferTo(CPBuffer *buf, int fd, int rlength)
{
    int r = buf->fillTo(fd, rlength);
    if (r == -1) {
        throw ReadCPException(errno);
    }
    else if (r == 0) {
        throw ReadCPException(0);
    }
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
static void wait_for_reply(CPBuffer &rbuffer, int fd)
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

// Entry point.
int main(int argc, char **argv)
{
    using namespace std;
    
    bool do_stop = false;
    bool show_help = argc < 2;
    char *service_name = nullptr;
    
    std::string control_socket_str;
    const char * control_socket_path = nullptr;
    
    bool verbose = true;
    bool sys_dinit = false;  // communicate with system daemon
    bool wait_for_service = true;
    bool do_pin = false;
    
    int command = 0;
    
    constexpr int START_SERVICE = 1;
    constexpr int STOP_SERVICE = 2;
        
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
                cerr << "Unrecognized command-line parameter: " << argv[i] << endl;
                return 1;
            }
        }
        else if (command == 0) {
            if (strcmp(argv[i], "start") == 0) {
                command = START_SERVICE; 
            }
            else if (strcmp(argv[i], "stop") == 0) {
                command = STOP_SERVICE;
            }
            else {
                show_help = true;
                break;
            }
        }
        else {
            // service name
            service_name = argv[i];
            // TODO support multiple services (or at least give error if multiple
            //      services supplied)
        }
    }
    
    if (service_name == nullptr || command == 0) {
        show_help = true;
    }

    if (show_help) {
        cout << "dinitctl:   control Dinit services" << endl;
        
        cout << "\nUsage:" << endl;
        cout << "    dinitctl [options] start [options] <service-name> : start and activate service" << endl;
        cout << "    dinitctl [options] stop [options] <service-name>  : stop service and cancel explicit activation" << endl;
        // TODO:
        // cout << "    dinitctl [options] wake <service-name>  : start but don't activate service" << endl;
        
        cout << "\nNote: An activated service keeps its dependencies running when possible." << endl;
        
        cout << "\nGeneral options:" << endl;
        cout << "  -s, --system     : control system daemon instead of user daemon" << endl;
        cout << "  --quiet          : suppress output (except errors)" << endl;
        
        cout << "\nCommand options:" << endl;
        cout << "  --help           : show this help" << endl;
        cout << "  --no-wait        : don't wait for service startup/shutdown to complete" << endl;
        cout << "  --pin            : pin the service in the requested (started/stopped) state" << endl;
        return 1;
    }
    
    do_stop = (command == STOP_SERVICE);
    
    control_socket_path = "/dev/dinitctl";
    
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
        perror("socket");
        return 1;
    }

    struct sockaddr_un * name;
    uint sockaddr_size = offsetof(struct sockaddr_un, sun_path) + strlen(control_socket_path) + 1;
    name = (struct sockaddr_un *) malloc(sockaddr_size);
    if (name == nullptr) {
        cerr << "dinit-start: out of memory" << endl;
        return 1;
    }
    
    name->sun_family = AF_UNIX;
    strcpy(name->sun_path, control_socket_path);
    
    int connr = connect(socknum, (struct sockaddr *) name, sockaddr_size);
    if (connr == -1) {
        perror("connect");
        return 1;
    }
    
    // TODO should start by querying protocol version
    
    // Build buffer;
    uint16_t sname_len = strlen(service_name);
    int bufsize = 3 + sname_len;
    char * buf = new char[bufsize];
    
    buf[0] = DINIT_CP_LOADSERVICE;
    memcpy(buf + 1, &sname_len, 2);
    memcpy(buf + 3, service_name, sname_len);
    
    int r = write_all(socknum, buf, bufsize);
    delete [] buf;
    if (r == -1) {
        perror("write");
        return 1;
    }
    
    // Now we expect a reply:
    
    try {
        CPBuffer rbuffer;
        wait_for_reply(rbuffer, socknum);
        
        //ServiceState state;
        //ServiceState target_state;
        handle_t handle;
        
        if (rbuffer[0] == DINIT_RP_SERVICERECORD) {
            fillBufferTo(&rbuffer, socknum, 2 + sizeof(handle));
            rbuffer.extract((char *) &handle, 2, sizeof(handle));
            //state = static_cast<ServiceState>(rbuffer[1]);
            //target_state = static_cast<ServiceState>(rbuffer[2 + sizeof(handle)]);
            rbuffer.consume(3 + sizeof(handle));
        }
        else if (rbuffer[0] == DINIT_RP_NOSERVICE) {
            cerr << "Failed to find/load service." << endl;
            return 1;
        }
        else {
            cerr << "Protocol error." << endl;
            return 1;
        }
        
        // ServiceState wanted_state = do_stop ? ServiceState::STOPPED : ServiceState::STARTED;
        int command = do_stop ? DINIT_CP_STOPSERVICE : DINIT_CP_STARTSERVICE;
        
        // Need to issue STOPSERVICE/STARTSERVICE
        // We'll do this regardless of the current service state / target state, since issuing
        // start/stop also sets or clears the "explicitly started" flag on the service.
        //if (target_state != wanted_state) {
        {
            buf = new char[2 + sizeof(handle)];
            buf[0] = command;
            buf[1] = do_pin ? 1 : 0;
            memcpy(buf + 2, &handle, sizeof(handle));
            r = write_all(socknum, buf, 2 + sizeof(handle));
            delete buf;
            
            if (r == -1) {
                perror("write");
                return 1;
            }
            
            wait_for_reply(rbuffer, socknum);
            if (rbuffer[0] == DINIT_RP_ALREADYSS) {
                if (verbose) {
                    cout << "Service already " << describeState(do_stop) << "." << endl;
                }
                return 0; // success!
            }
            if (rbuffer[0] != DINIT_RP_ACK) {
                cerr << "Protocol error." << endl;
                return 1;
            }
            rbuffer.consume(1);
        }
        
        /*
        if (state == wanted_state) {
            if (verbose) {
                cout << "Service already " << describeState(do_stop) << "." << endl;
            }
            return 0; // success!
        }
        */
        
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
        r = rbuffer.fillTo(socknum, 2);
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
                r = rbuffer.fillTo(socknum, 2);
            }
            else {
                // Not an information packet?
                cerr << "protocol error" << endl;
                return 1;
            }
        }
        
        if (r == -1) {
            perror("read");
        }
        else {
            cerr << "protocol error (connection closed by server)" << endl;
        }
        return 1;
    }
    catch (ReadCPException &exc) {
        cerr << "control socket read failure or protocol error" << endl;
        return 1;
    }
    catch (std::bad_alloc &exc) {
        cerr << "out of memory" << endl;
        return 1;
    }
    
    return 0;
}
