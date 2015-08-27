#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
// #include <netinet/in.h>
#include <cstdio>
#include <unistd.h>
#include <cstring>
#include <string>
#include <iostream>

// dinit-start:  utility to start a dinit service

// This utility communicates with the dinit daemon via a unix socket (/dev/initctl).

// TODO move these into a common include file:
constexpr static int DINIT_CP_STARTSERVICE = 0;
constexpr static int DINIT_CP_STOPSERVICE  = 1;


int main(int argc, char **argv)
{
    using namespace std;
    
    bool show_help = argc < 2;
    char *service_name = nullptr;
        
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "--help") == 0) {
                show_help = true;
                break;
            }
            else {
                cerr << "Unrecognized command-line parameter: " << argv[i] << endl;
                return 1;
            }
        }
        else {
            // service name
            service_name = argv[i];
            // TODO support multiple services (or at least give error if multiple services
            //     supplied)
        }
    }

    if (show_help) {
        cout << "dinit-start:   start a dinit service" << endl;
        cout << "  --help           : show this help" << endl;
        cout << "  <service-name>   : start the named service" << endl;
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
    // memset(name.sun_path, 0, sizeof(name.sun_path));
    strcpy(name.sun_path /* + 1 */, naddr);
    int sunlen = 2 + strlen(naddr); // family, (string), nul
    
    int connr = connect(socknum, (struct sockaddr *) &name, sunlen);
    if (connr == -1) {
        perror("connect");
        return 1;
    }
    
    // Build buffer;
    uint16_t sname_len = strlen(service_name);
    int bufsize = 3 + sname_len;
    char * buf = new char[bufsize];
    
    buf[0] = DINIT_CP_STARTSERVICE;
    memcpy(buf + 1, &sname_len, 2);
    memcpy(buf + 3, service_name, sname_len);
    
    int r = write(socknum, buf, bufsize);
    if (r == -1) {
        perror("write");
    }
    
    return 0;
}
