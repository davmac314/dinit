#include <sys/types.h>
#include <unistd.h>

// Mock system functions for testing.

namespace bp_sys {

int pipe2(int pipefd[2], int flags);
int close(int fd);
int kill(pid_t pid, int sig);

inline int fcntl(int fd, int cmd, ...)
{
    // This is used for setting the CLOEXEC flag, we can just return 0:
    return 0;
}

inline pid_t getpgid(pid_t pid)
{
    return pid;
}

inline int tcsetpgrp(int fd, pid_t pgid)
{
    return 0;
}

inline pid_t getpgrp()
{
    return getpid();
}

}
