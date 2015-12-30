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

// dinit-start:  utility to start a dinit service

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
            else {
                cerr << "Unrecognized command-line parameter: " << argv[i] << endl;
                return 1;
            }
        }
        else {
            // service name
            service_name = argv[i];
            // TODO support multiple services (or at least give error if multiple
            //      services supplied)
        }
    }

    if (show_help) {
        cout << "dinit-start:   start a dinit service" << endl;
        cout << "  --help           : show this help" << endl;
        cout << "  --no-wait        : don't wait for service startup/shutdown to complete" << endl;
        cout << "  --quiet          : suppress output (except errors)" << endl;
        cout << "  -s, --system     : control system daemon instead of user daemon" << endl;
        cout << "  <service-name>   : start the named service" << endl;
        return 1;
    }
    
    
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
    
    int r = write(socknum, buf, bufsize);
    // TODO make sure we write it all
    delete [] buf;
    if (r == -1) {
        perror("write");
        return 1;
    }
    
    // Now we expect a reply:
    // NOTE: should skip over information packets.
    
    try {
        CPBuffer rbuffer;
        fillBufferTo(&rbuffer, socknum, 1);
        
        ServiceState state;
        ServiceState target_state;
        handle_t handle;
        
        if (rbuffer[0] == DINIT_RP_SERVICERECORD) {
            fillBufferTo(&rbuffer, socknum, 2 + sizeof(handle));
            rbuffer.extract((char *) &handle, 2, sizeof(handle));
            state = static_cast<ServiceState>(rbuffer[1]);
            target_state = static_cast<ServiceState>(rbuffer[2 + sizeof(handle)]);
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
        
        // Need to issue STARTSERVICE:
        if (target_state != ServiceState::STARTED) {
            buf = new char[2 + sizeof(handle)];
            buf[0] = DINIT_CP_STARTSERVICE;
            buf[1] = 0;  // don't pin
            memcpy(buf + 2, &handle, sizeof(handle));
            r = write(socknum, buf, 2 + sizeof(handle));
            delete buf;
        }
        
        if (state == ServiceState::STARTED) {
            if (verbose) {
                cout << "Service already started." << endl;
            }
            return 0; // success!
        }
        
        if (! wait_for_service) {
            return 0;
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
                    if (ev_handle == handle && event == ServiceEvent::STARTED) {
                        if (verbose) {
                            cout << "Service started." << endl;
                        }
                        return 0;
                    }
                }
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
