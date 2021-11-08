#ifndef DASYNQ_INTERRUPT_H_
#define DASYNQ_INTERRUPT_H_

#include <unistd.h>
#include <fcntl.h>

#ifdef DASYNQ_HAVE_EVENTFD
#include <sys/eventfd.h>
#endif

#include "config.h"
#include "mutex.h"
#include "util.h"

/*
 * Mechanism for interrupting an event loop wait.
 */

namespace dasynq {

template <typename Base, typename Mutex = typename Base::mutex_t> class interrupt_channel;

// In the non-multi-thread case, this doesn't need to be supported:
template <typename Base> class interrupt_channel<Base, null_mutex> : public Base
{
    public:
    void interrupt_wait()
    {

    }
};

template <typename Base, typename Mutex> class interrupt_channel : public Base
{
#if !DASYNQ_HAVE_EVENTFD
    static inline int create_pipe(int filedes[2])
    {
        return pipe2(filedes, O_CLOEXEC | O_NONBLOCK);
    }
#endif

    // If we have eventfd, we only need a single "pipe" end. Otherwise we need a separate read
    // end and write end.

    int pipe_r_fd;

#if !DASYNQ_HAVE_EVENTFD
    int pipe_w_fd;
#endif

    public:

    template <typename T> void init(T *loop_mech)
    {
#if !DASYNQ_HAVE_EVENTFD
        int pipedes[2];

        if (create_pipe(pipedes) == -1) {
            throw std::system_error(errno, std::system_category());
        }

        pipe_r_fd = pipedes[0];
        pipe_w_fd = pipedes[1];
#else
        pipe_r_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
#endif

        try {
            loop_mech->add_fd_watch(pipe_r_fd, &pipe_r_fd, IN_EVENTS);
        }
        catch (...) {
            close (pipe_r_fd);
#if !DASYNQ_HAVE_EVENTFD
            close (pipe_w_fd);
#endif
            throw;
        }

        Base::init(loop_mech);
    }

    template <typename T>
    std::tuple<int, typename Base::traits_t::fd_s>
    receive_fd_event(T &loop_mech, typename Base::traits_t::fd_r fd_r_a, void * userdata, int flags)
    {
        if (userdata == &pipe_r_fd) {
            // try to clear the pipe
#if !DASYNQ_HAVE_EVENTFD
            char buf[64];
            read(pipe_r_fd, buf, 64);
#else
            eventfd_t val;
            eventfd_read(pipe_r_fd, &val);
#endif
            if (Base::traits_t::supports_non_oneshot_fd) {
                // If the loop mechanism actually persists none-oneshot marked watches, we don't need
                // to re-enable:
                return std::make_tuple(0, typename Base::traits_t::fd_s(pipe_r_fd));
            }
            else {
                return std::make_tuple(IN_EVENTS, typename Base::traits_t::fd_s(pipe_r_fd));
            }
        }
        else {
            return Base::receive_fd_event(loop_mech, fd_r_a, userdata, flags);
        }
    }

    void interrupt_wait()
    {
#if !DASYNQ_HAVE_EVENTFD
        char buf[1] = { 0 };
        write(pipe_w_fd, buf, 1);
#else
        eventfd_write(pipe_r_fd, 1);
#endif
    }
};

} // namespace dasynq

#endif /* DASYNQ_INTERRUPT_H_ */
