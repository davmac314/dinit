#include <sys/types.h>

// Mock system functions for testing.

namespace bp_sys {

inline int pipe2(int pipefd[2], int flags)
{
    abort();
    return 0;
}

inline int fcntl(int fd, int cmd, ...)
{
    // This is used for setting the CLOEXEC flag, we can just return 0:
    return 0;
}

inline int close(int fd)
{
    abort();
    return 0;
}

inline int kill(pid_t pid, int sig)
{
    abort();
    return 0;
}

}
