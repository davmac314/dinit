#ifndef DINIT_H_INCLUDED
#define DINIT_H_INCLUDED 1

// dummy dinit.h

#include "dasynq.h"

using clock_type = dasynq::clock_type;
using rearm = dasynq::rearm;
using time_val = dasynq::time_val;

namespace bp_sys {
    extern pid_t last_forked_pid;
}

class eventloop_t
{
    public:
    void get_time(time_val &tv, dasynq::clock_type clock) noexcept
    {
        tv = {0, 0};
    }

    class child_proc_watcher
    {
        public:
        pid_t fork(eventloop_t &loop, bool reserved_child_watcher, int priority = dasynq::DEFAULT_PRIORITY)
        {
            bp_sys::last_forked_pid++;
            return bp_sys::last_forked_pid;
        }

        void add_reserved(eventloop_t &eloop, pid_t child, int prio = dasynq::DEFAULT_PRIORITY) noexcept
        {

        }

        void stop_watch(eventloop_t &eloop) noexcept
        {

        }

        void deregister(eventloop_t &loop, pid_t pid) noexcept
        {

        }
    };

    template <typename Derived> class child_proc_watcher_impl : public child_proc_watcher
    {

    };

    class fd_watcher
    {
        int watched_fd;

        public:
        void add_watch(eventloop_t &loop, int fd, int events, bool enable = true)
        {
            watched_fd = fd;
        }

        int get_watched_fd() noexcept
        {
            return watched_fd;
        }

        void set_enabled(eventloop_t &loop, bool enable) noexcept
        {

        }

        void deregister(eventloop_t &loop) noexcept
        {

        }
    };

    template <typename Derived> class fd_watcher_impl : public fd_watcher
    {

    };

    class timer
    {
        public:
        void add_timer(eventloop_t &loop)
        {

        }

        void arm_timer_rel(eventloop_t &loop, time_val timeout) noexcept
        {

        }

        void stop_timer(eventloop_t &loop) noexcept
        {

        }
    };

    template <typename Derived> class timer_impl : public timer
    {

    };
};

inline void open_control_socket(bool report_ro_failure = true) noexcept
{
}

inline void setup_external_log() noexcept
{
}

extern eventloop_t event_loop;

#endif
