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

#include "cpbuffer.h"
#include "control-cmds.h"
#include "service-constants.h"

// shutdown:  shut down the system
// This utility communicates with the dinit daemon via a unix socket (/dev/initctl).

void do_system_shutdown(shutdown_type_t shutdown_type);
static void unmount_disks();
static void swap_off();
static void wait_for_reply(cpbuffer<1024> &rbuffer, int fd);


class ReadCPException
{
    public:
    int errcode;
    ReadCPException(int err) : errcode(err) { }
};


int main(int argc, char **argv)
{
    using namespace std;
    
    bool show_help = false;
    bool sys_shutdown = false;
    bool use_passed_cfd = false;
    
    auto shutdown_type = shutdown_type_t::POWEROFF;
        
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "--help") == 0) {
                show_help = true;
                break;
            }
            
            if (strcmp(argv[i], "--system") == 0) {
                sys_shutdown = true;
            }
            else if (strcmp(argv[i], "-r") == 0) {
                shutdown_type = shutdown_type_t::REBOOT;
            }
            else if (strcmp(argv[i], "-h") == 0) {
                shutdown_type = shutdown_type_t::HALT;
            }
            else if (strcmp(argv[i], "-p") == 0) {
                shutdown_type = shutdown_type_t::POWEROFF;
            }
            else if (strcmp(argv[i], "--use-passed-cfd") == 0) {
                use_passed_cfd = true;
            }
            else {
                cerr << "Unrecognized command-line parameter: " << argv[i] << endl;
                return 1;
            }
        }
        else {
            // time argument? TODO
            show_help = true;
        }
    }

    if (show_help) {
        cout << "dinit-shutdown :   shutdown the system" << endl;
        cout << "  --help           : show this help" << endl;
        cout << "  -r               : reboot" << endl;
        cout << "  -h               : halt system" << endl;
        cout << "  -p               : power down (default)" << endl;
        cout << "  --use-passed-cfd : use the socket file descriptor identified by the DINIT_CS_FD" << endl;
        cout << "                     environment variable to communicate with the init daemon." << endl;
        cout << "  --system         : perform shutdown immediately, instead of issuing shutdown" << endl;
        cout << "                     command to the init program. Not recommended for use" << endl;
        cout << "                     by users." << endl;
        return 1;
    }
    
    if (sys_shutdown) {
        do_system_shutdown(shutdown_type);
        return 0;
    }

    signal(SIGPIPE, SIG_IGN);
    
    int socknum = 0;
    
    if (use_passed_cfd) {
        char * dinit_cs_fd_env = getenv("DINIT_CS_FD");
        if (dinit_cs_fd_env != nullptr) {
            char * endptr;
            long int cfdnum = strtol(dinit_cs_fd_env, &endptr, 10);
            if (endptr != dinit_cs_fd_env) {
                socknum = (int) cfdnum;
            }
            else {
                use_passed_cfd = false;
            }
        }
        else {
            use_passed_cfd = false;
        }
    }
    
    if (! use_passed_cfd) {
        socknum = socket(AF_UNIX, SOCK_STREAM, 0);
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
    }

    // Build buffer;
    constexpr int bufsize = 2;
    char buf[bufsize];
    
    buf[0] = DINIT_CP_SHUTDOWN;
    buf[1] = static_cast<char>(shutdown_type);
    
    cout << "Issuing shutdown command..." << endl;
    
    // TODO make sure to write the whole buffer
    int r = write(socknum, buf, bufsize);
    if (r == -1) {
        perror("write");
        return 1;
    }
    
    // Wait for ACK/NACK
    // r = read(socknum, buf, 1);
    //if (r > 0) {
    //    cout << "Received acknowledgement. System should now shut down." << endl;
    //}
    
    cpbuffer<1024> rbuffer;
    try {
        wait_for_reply(rbuffer, socknum);
        
        if (rbuffer[0] != DINIT_RP_ACK) {
            cerr << "shutdown: control socket protocol error" << endl;
            return 1;
        }
    }
    catch (ReadCPException &exc)
    {
        cerr << "shutdown: control socket read failure or protocol error" << endl;    
        return 1;
    }
    
    while (true) {
        pause();
    }
    
    return 0;
}

// Fill a circular buffer from a file descriptor, reading at least _rlength_ bytes.
// Throws ReadException if the requested number of bytes cannot be read, with:
//     errcode = 0   if end of stream (remote end closed)
//     errcode = errno   if another error occurred
// Note that EINTR is ignored (i.e. the read will be re-tried).
static void fillBufferTo(cpbuffer<1024> *buf, int fd, int rlength)
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

// Wait for a reply packet, skipping over any information packets
// that are received in the meantime.
static void wait_for_reply(cpbuffer<1024> &rbuffer, int fd)
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

// Actually shut down the system.
void do_system_shutdown(shutdown_type_t shutdown_type)
{
    using namespace std;
    
    // Mask all signals to prevent death of our parent etc from terminating us
    sigset_t allsigs;
    sigfillset(&allsigs);
    sigprocmask(SIG_SETMASK, &allsigs, nullptr);
    
    int reboot_type = 0;
    if (shutdown_type == shutdown_type_t::REBOOT) reboot_type = RB_AUTOBOOT;
    else if (shutdown_type == shutdown_type_t::POWEROFF) reboot_type = RB_POWER_OFF;
    else reboot_type = RB_HALT_SYSTEM;
    
    // Write to console rather than any terminal, since we lose the terminal it seems:
    close(STDOUT_FILENO);
    int consfd = open("/dev/console", O_WRONLY);
    if (consfd != STDOUT_FILENO) {
        dup2(consfd, STDOUT_FILENO);
    }
    
    cout << "Sending TERM/KILL to all processes..." << endl; // DAV
    
    // Send TERM/KILL to all (remaining) processes
    kill(-1, SIGTERM);
    sleep(1);
    kill(-1, SIGKILL);
    
    // perform shutdown
    cout << "Turning off swap..." << endl;
    swap_off();
    cout << "Unmounting disks..." << endl;
    unmount_disks();
    sync();
    
    cout << "Issuing shutdown via kernel..." << endl;
    reboot(reboot_type);
}

static void unmount_disks()
{
    pid_t chpid = fork();
    if (chpid == 0) {
        // umount -a -r
        //  -a : all filesystems (except proc)
        //  -r : mount readonly if can't unmount
        execl("/bin/umount", "/bin/umount", "-a", "-r", nullptr);
    }
    else if (chpid > 0) {
        int status;
        waitpid(chpid, &status, 0);
    }
}

static void swap_off()
{
    pid_t chpid = fork();
    if (chpid == 0) {
        // swapoff -a
        execl("/sbin/swapoff", "/sbin/swapoff", "-a", nullptr);
    }
    else if (chpid > 0) {
        int status;
        waitpid(chpid, &status, 0);
    }
}
