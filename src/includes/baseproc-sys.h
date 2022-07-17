/*
 * This header implements a namespace, bp_sys, which wraps various system calls used by baseproc-service.cc.
 *
 * When running tests, another header is substituted in place of this one. The substitute provides
 * mocks/stubs for the functions, to avoid calling the real functions and thus allow for unit-level testing.
 */

#ifndef BPSYS_INCLUDED
#define BPSYS_INCLUDED

#include <cstdlib> // getenv

#include "dasynq.h" // for pipe2

#include <sys/uio.h> // writev
#include <unistd.h>
#include <fcntl.h>

extern char **environ;

namespace bp_sys {

using dasynq::pipe2;

using ::fcntl;
using ::open;
using ::close;
using ::kill;
using ::getpgid;
using ::tcsetpgrp;
using ::getpgrp;
using ::read;
using ::write;
using ::writev;
using std::getenv;

using ::environ;

// Wrapper around a POSIX exit status
class exit_status
{
    friend pid_t waitpid(pid_t, exit_status *, int);

    int status;

    public:
    exit_status() noexcept : status(0) { }
    explicit exit_status(int status_p) noexcept : status(status_p) { }

    bool did_exit() noexcept
    {
        return WIFEXITED(status);
    }

    bool did_exit_clean() noexcept
    {
        // POSIX requires that if the process exited clearly with a status code of 0,
        // the exit status value will be 0:
        return status == 0;
    }

    bool was_signalled() noexcept
    {
        return WIFSIGNALED(status);
    }

    int get_exit_status() noexcept
    {
        return WEXITSTATUS(status);
    }

    int get_term_sig() noexcept
    {
        return WTERMSIG(status);
    }

    int as_int() noexcept
    {
        return status;
    }
};

inline pid_t waitpid(pid_t p, exit_status *statusp, int flags)
{
    return ::waitpid(p, &statusp->status, flags);
}

}

#endif  // BPSYS_INCLUDED
