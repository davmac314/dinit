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

// shutdown:  shut down the system
// This utility communicates with the dinit daemon via a unix socket (/dev/initctl).

static void unmount_disks();
static void swap_off();

int main(int argc, char **argv)
{
    using namespace std;
    
    int sd_type = 0;
    
    //bool show_help = argc < 2;
    bool show_help = false;
        
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "--help") == 0) {
                show_help = true;
                break;
            }
            else if (strcmp(argv[i], "-r") == 0) {
                // Reboot
                sd_type = 1;
            }
            else if (strcmp(argv[i], "-p") == 0) {
                // Power down
                sd_type = 2;
            }
            else if (strcmp(argv[i], "-h") == 0) {
                // Halt
                sd_type = 3;
            }
            else if (strcmp(argv[i], "-l") == 0) {
                // Loop
                sd_type = 0;
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
        return 1;
    }
    
    if (sd_type == 0) {
        while (true) {
            pause();
        }
    }
    
    int reboot_type = 0;
    if (sd_type == 1) reboot_type = RB_AUTOBOOT;
    else if (sd_type == 2) reboot_type = RB_POWER_OFF;
    else reboot_type = RB_HALT_SYSTEM;
    
    // Write to console rather than any terminal, since we lose the terminal it seems:
    close(STDOUT_FILENO);
    int consfd = open("/dev/console", O_WRONLY);
    if (consfd != STDOUT_FILENO) {
        dup2(consfd, STDOUT_FILENO);
    }
    
    // At this point, util-linux 2.13 shutdown sends SIGTERM to all processes with uid >= 100 and
    // calls it 'sendiong SIGTERM to mortals'.
    // Equivalent would probably be to rollback 'loginready' service. However, that will happen as
    // part of the regular rollback anyway.
    
    //cout << "Writing rollback command..." << endl; // DAV
    
    //int r = write(socknum, buf, bufsize);
    //if (r == -1) {
    //    perror("write");
    //}
    
    cout << "Sending TERM/KILL..." << endl; // DAV
    
    // Send TERM/KILL to all (remaining) processes
    kill(-1, SIGTERM);
    sleep(1);
    kill(-1, SIGKILL);
    
    cout << "Sending QUIT to init..." << endl; // DAV
    
    // Tell init to exec reboot:
    // TODO what if it's not PID=1? probably should have dinit pass us its PID
    kill(1, SIGQUIT);
    
    // TODO can we wait somehow for above to work?
    // maybe have a pipe/socket and we read from our end...
    
    // TODO: close all ancillary file descriptors.
    
    // perform shutdown
    cout << "Turning off swap..." << endl;
    swap_off();
    cout << "Unmounting disks..." << endl;
    unmount_disks();
    sync();
    
    cout << "Issuing shutdown via kernel..." << endl;
    reboot(reboot_type);
    
    return 0;
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
