#ifndef DASYNQ_INTERRUPT_H_INCLUDED
#define DASYNQ_INTERRUPT_H_INCLUDED

#include <unistd.h>
#include <fcntl.h>

#include "dasynq-config.h"
#include "dasynq-mutex.h"

/*
 * Mechanism for interrupting an event loop wait.
 */

namespace dasynq {

template <typename Base, typename Mutex = typename Base::mutex_t> class interrupt_channel;

// In the non-multi-thread case, this doesn't need to be supported:
template <typename Base> class interrupt_channel<Base, NullMutex> : public Base
{
    public:
    void interrupt_wait()
    {

    }
};

template <typename Base, typename Mutex> class interrupt_channel : public Base
{
#ifdef HAVE_PIPE2
    int create_pipe(int filedes[2])
    {
        return pipe2(filedes, O_CLOEXEC | O_NONBLOCK);
    }
#else
    int create_pipe(int filedes[2])
    {
        int r = pipe(filedes);
        if (r != -1) {
            fcntl(filedes[0], F_SETFD, O_CLOEXEC);
            fcntl(filedes[1], F_SETFD, O_CLOEXEC);
            fcntl(filedes[0], F_SETFL, O_NONBLOCK);
            fcntl(filedes[1], F_SETFL, O_NONBLOCK);
        }
        return r;
    }
#endif

    int pipe_r_fd;
    int pipe_w_fd;

    public:

    template <typename T> void init(T *loop_mech)
    {
        int pipedes[2];
        if (create_pipe(pipedes) == -1) {
            throw std::system_error(errno, std::system_category());
        }

        pipe_r_fd = pipedes[0];
        pipe_w_fd = pipedes[1];

        try {
            loop_mech->addFdWatch(pipe_r_fd, &pipe_r_fd, IN_EVENTS);
        }
        catch (...) {
            close (pipe_r_fd);
            close (pipe_w_fd);
            throw;
        }

        Base::init(loop_mech);
    }

    template <typename T>
    void receiveFdEvent(T &loop_mech, typename Base::FD_r fd_r, void * userdata, int flags)
    {
        if (userdata == &pipe_r_fd) {
            // try to clear the pipe
            char buf[64];
            read(pipe_r_fd, buf, 64);
        }
        else {
            Base::receiveFdEvent(loop_mech, fd_r, userdata, flags);
        }
    }

    void interrupt_wait()
    {
        char buf[1] = { 0 };
        write(pipe_w_fd, buf, 1);
    }
};

}

#endif /* DASYNQ_INTERRUPT_H_INCLUDED */
