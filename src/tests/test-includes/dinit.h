#ifndef DINIT_H_INCLUDED
#define DINIT_H_INCLUDED 1

// dummy dinit.h

#include <unordered_set>
#include <map>
#include <string>
#include <cassert>

#include <dasynq.h>

using clock_type = dasynq::clock_type;
using rearm = dasynq::rearm;
using time_val = dasynq::time_val;

namespace bp_sys {
    extern pid_t last_forked_pid;
}

// This is a mock for a Dasynq-based event loop
class eventloop_t
{
    time_val current_time {0, 0};

    public:
    void get_time(time_val &tv, dasynq::clock_type clock) noexcept
    {
        tv = current_time;
    }

    // Advance the simulated current time by the given amount, and call timer callbacks.
    void advance_time(time_val amount)
    {
        current_time += amount;
        auto active_copy = active_timers;
        for (timer * t : active_copy) {
            if (t->expiry_time <= current_time) {
                t->stop_timer(*this);
                rearm r = t->expired(*this, 1);
                assert(r == rearm::NOOP); // others not handled
            }
        }
    }

    void send_fd_event(int fd, int events)
    {
        auto i = regd_fd_watchers.find(fd);
        if (i != regd_fd_watchers.end()) {
            fd_watcher *watcher = i->second;
            dasynq::rearm r = watcher->fd_event(*this, fd, events);
            if (r == dasynq::rearm::REMOVE) {
                watcher->deregister(*this);
            }
        }
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

        void unreserve(eventloop_t &loop) noexcept
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
            if (loop.regd_fd_watchers.find(fd) != loop.regd_fd_watchers.end()
                    || loop.regd_bidi_watchers.find(fd) != loop.regd_bidi_watchers.end()) {
                throw std::string("must not add_watch when already active");
            }
            watched_fd = fd;
            loop.regd_fd_watchers[fd] = this;
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
            loop.regd_fd_watchers.erase(watched_fd);
            watched_fd = -1;
            watch_removed();
        }

        virtual rearm fd_event(eventloop_t & loop, int fd, int flags) = 0;

        virtual void watch_removed() noexcept { }
    };

    template <typename Derived> class fd_watcher_impl : public fd_watcher
    {

    };

    class bidi_fd_watcher
    {
        int watched_fd = -1;

        public:
        void set_watches(eventloop_t &eloop, int newFlags) noexcept
        {

        }

        void add_watch(eventloop_t &eloop, int fd, int flags, int inprio = dasynq::DEFAULT_PRIORITY,
                int outprio = dasynq::DEFAULT_PRIORITY)
        {
            if (eloop.regd_bidi_watchers.find(fd) != eloop.regd_bidi_watchers.end()) {
                throw std::string("must not add_watch when already active");
            }
            eloop.regd_bidi_watchers[fd] = this;
            watched_fd = fd;
        }

        int get_watched_fd() noexcept
        {
            return watched_fd;
        }

        void deregister(eventloop_t &eloop) noexcept
        {
            eloop.regd_bidi_watchers.erase(watched_fd);
            watched_fd = -1;
        }

        // In the real implementation these are not virtual, but it is easier for testing if they are:
        virtual rearm read_ready(eventloop_t &loop, int fd) noexcept = 0;
        virtual rearm write_ready(eventloop_t &loop, int fd) noexcept = 0;
    };

    template <typename Derived> class bidi_fd_watcher_impl : public bidi_fd_watcher
    {

    };

    class timer
    {
        friend class eventloop_t;

        private:
        time_val expiry_time;

        protected:
        virtual rearm expired(eventloop_t &loop, int expiry_count)
        {
            return rearm::NOOP;
        }

        public:
        void add_timer(eventloop_t &loop)
        {

        }

        void arm_timer_rel(eventloop_t &loop, time_val timeout) noexcept
        {
            expiry_time = loop.current_time + timeout;
            loop.active_timers.insert(this);
        }

        void stop_timer(eventloop_t &loop) noexcept
        {
            loop.active_timers.erase(this);
        }

        void deregister(eventloop_t &loop) noexcept
        {

        }
    };

    template <typename Derived> class timer_impl : public timer
    {
        protected:
        virtual rearm expired(eventloop_t &loop, int expiry_count) override
        {
            return static_cast<Derived *>(this)->timer_expiry(loop, expiry_count);
        }
    };

    std::unordered_set<timer *> active_timers;
    std::map<int, bidi_fd_watcher *> regd_bidi_watchers;
    std::map<int, fd_watcher *> regd_fd_watchers;
};

inline void rootfs_is_rw() noexcept
{
}

inline void setup_external_log() noexcept
{
}

extern eventloop_t event_loop;

#endif
