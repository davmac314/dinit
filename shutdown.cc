// #include <netinet/in.h>
#include <cstddef>
#include <cstdio>
#include <csignal>
#include <unistd.h>
#include <cstring>
#include <string>
#include <iostream>

#include <sys/reboot.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "control-cmds.h"
#include "service-constants.h"

// shutdown:  shut down the system
// This utility communicates with the dinit daemon via a unix socket (/dev/initctl).

int main(int argc, char **argv)
{
    using namespace std;
    
    //bool show_help = argc < 2;
    bool show_help = false;
    
    auto shutdown_type = ShutdownType::POWEROFF;
        
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "--help") == 0) {
                show_help = true;
                break;
            }
            else if (strcmp(argv[i], "-r") == 0) {
                shutdown_type = ShutdownType::REBOOT;
            }
            else if (strcmp(argv[i], "-h") == 0) {
                shutdown_type = ShutdownType::POWEROFF;
            }
            else {
                cerr << "Unrecognized command-line parameter: " << argv[i] << endl;
                return 1;
            }
        }
        else {
            // time argument? TODO
        }
    }

    if (show_help) {
        cout << "dinit-shutdown :   shutdown the system" << endl;
        cout << "  --help           : show this help" << endl;
        return 1;
    }
    
    int socknum = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socknum == -1) {
        perror("socket");
        return 1;
    }

    const char *naddr = "/dev/dinitctl";
    
    struct sockaddr_un name;
    name.sun_family = AF_UNIX;
    strcpy(name.sun_path, naddr);
    int sunlen = offsetof(struct sockaddr_un, sun_path) + strlen(naddr) + 1; // family, (string), nul
    
    int connr = connect(socknum, (struct sockaddr *) &name, sunlen);
    if (connr == -1) {
        perror("connect");
        return 1;
    }
    
    // Build buffer;
    //uint16_t sname_len = strlen(service_name);
    int bufsize = 2;
    char * buf = new char[bufsize];
    
    buf[0] = DINIT_CP_SHUTDOWN;
    buf[1] = static_cast<char>(shutdown_type);
    
    //memcpy(buf + 1, &sname_len, 2);
    //memcpy(buf + 3, service_name, sname_len);
    
    // Make sure we can't die due to a signal at this point:
    //sigset_t sigmask;
    //sigfillset(&sigmask);
    //sigprocmask(SIG_BLOCK, &sigmask, nullptr);
    
    // Write to console rather than any terminal, since we lose the terminal it seems:
    //close(STDOUT_FILENO);
    //int consfd = open("/dev/console", O_WRONLY);
    //if (consfd != STDOUT_FILENO) {
    //    dup2(consfd, STDOUT_FILENO);
    //}
    
    // At this point, util-linux 2.13 shutdown sends SIGTERM to all processes with uid >= 100 and
    // calls it 'sendiong SIGTERM to mortals'.
    // Equivalent would probably be to rollback 'loginready' service. However, that will happen as
    // part of the regular rollback anyway.
    
    cout << "Writing shutdown command..." << endl; // DAV
    
    // TODO make sure to write the whole buffer
    int r = write(socknum, buf, bufsize);
    if (r == -1) {
        perror("write");
    }
    
    cout << "Waiting for ACK..." << endl; // DAV
    
    // Wait for ACK/NACK
    r = read(socknum, buf, 1);
    // TODO: check result
    
    return 0;
}
