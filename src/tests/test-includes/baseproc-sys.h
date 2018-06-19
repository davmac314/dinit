#ifndef BPSYS_INCLUDED
#define BPSYS_INCLUDED

#include <string>
#include <sys/types.h>
#include <unistd.h>

// Mock system functions for testing.

namespace bp_sys {

// implementations elsewhere:
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

class exit_status
{
    friend pid_t waitpid(pid_t, exit_status *, int);

    bool did_exit_v;
    bool was_signalled_v;
    int status;

    public:
    exit_status() : did_exit_v(true), was_signalled_v(false), status(0) { }

    // status_p is either the exit status or termination signal:
    exit_status(bool did_exit_p, bool was_signalled_p, int status_p)
        : did_exit_v(did_exit_p), was_signalled_v(was_signalled_p), status(status_p)
    { }

    explicit exit_status(int status_p)
    {
        throw std::string("initalised exit_status with integer argument");
    }

    bool did_exit()
    {
        return did_exit_v;
    }

    bool did_exit_clean()
    {
        return did_exit_v && status == 0;
    }

    bool was_signalled()
    {
        return was_signalled_v;
    }

    int get_exit_status()
    {
        return status;
    }

    int get_term_sig()
    {
        return status;
    }

    int as_int()
    {
        return -1;
    }
};

inline pid_t waitpid(pid_t p, exit_status *statusp, int flags)
{
    throw std::string("not implemented");
}

}

#endif
