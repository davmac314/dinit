#ifndef DASYNQ_TIMERFD_H_
#define DASYNQ_TIMERFD_H_

#include <vector>
#include <utility>

#include <sys/timerfd.h>
#include <ctime>

#include "timerbase.h"

namespace dasynq {

// Timer implementation based on Linux's "timerfd".

// We could use one timerfd per timer, but then we need to differentiate timer
// descriptors from regular file descriptors when events are reported by the loop
// mechanism so that we can correctly report a timer event or fd event.

// With a file descriptor or signal, we can use the item itself as the identifier for
// adding/removing watches. For timers, it's more complicated. When we add a timer,
// we are given a handle; we need to use this to modify the watch. We delegate the
// process of allocating a handle to a priority heap implementation (BinaryHeap).

template <class Base> class timer_fd_events : public timer_base<Base>
{
    private:
    int timerfd_fd = -1;
    int systemtime_fd = -1;
    
    // Set the timerfd timeout to match the first timer in the queue (disable the timerfd
    // if there are no active timers).
    static void set_timer_from_queue(int fd, timer_queue_t &queue) noexcept
    {
        struct itimerspec newtime;
        if (queue.empty()) {
            newtime.it_value = {0, 0};
            newtime.it_interval = {0, 0};
        }
        else {
            newtime.it_value = queue.get_root_priority();
            newtime.it_interval = {0, 0};
        }
        timerfd_settime(fd, TFD_TIMER_ABSTIME, &newtime, nullptr);
    }
    
    void process_timer(clock_type clock, int fd) noexcept
    {
        timer_queue_t &queue = this->queue_for_clock(clock);
        struct timespec curtime;
        switch (clock) {
        case clock_type::SYSTEM:
            clock_gettime(CLOCK_REALTIME, &curtime);
            break;
        case clock_type::MONOTONIC:
            clock_gettime(CLOCK_MONOTONIC, &curtime);
            break;
        default:
            DASYNQ_UNREACHABLE;
        }

        timer_base<Base>::process_timer_queue(queue, curtime);

        // arm timerfd with timeout from head of queue
        set_timer_from_queue(fd, queue);
    }

    void set_timer(timer_handle_t & timer_id, const time_val &timeouttv, const time_val &intervaltv,
            timer_queue_t &queue, int fd, bool enable) noexcept
    {
        timespec timeout = timeouttv;
        timespec interval = intervaltv;

        std::lock_guard<decltype(Base::lock)> guard(Base::lock);

        auto &ts = queue.node_data(timer_id);
        ts.interval_time = interval;
        ts.expiry_count = 0;
        ts.enabled = enable;

        if (queue.is_queued(timer_id)) {
            // Already queued; alter timeout
            if (queue.set_priority(timer_id, timeout)) {
                set_timer_from_queue(fd, queue);
            }
        }
        else {
            if (queue.insert(timer_id, timeout)) {
                set_timer_from_queue(fd, queue);
            }
        }
    }

    public:

    class traits_t : public Base::traits_t
    {
        constexpr static bool full_timer_support = true;
    };

    template <typename T>
    std::tuple<int, typename traits_t::fd_s>
    receive_fd_event(T &loop_mech, typename traits_t::fd_r fd_r_a, void * userdata, int flags)
    {
        if (userdata == &timerfd_fd) {
            process_timer(clock_type::MONOTONIC, timerfd_fd);
            return std::make_tuple(IN_EVENTS, typename traits_t::fd_s(timerfd_fd));
        }
        else if (userdata == &systemtime_fd) {
            process_timer(clock_type::SYSTEM, systemtime_fd);
            if (Base::traits_t::supports_non_oneshot_fd) {
                return std::make_tuple(0, typename traits_t::fd_s(systemtime_fd));
            }
            return std::make_tuple(IN_EVENTS, typename traits_t::fd_s(systemtime_fd));
        }
        else {
            return Base::receive_fd_event(loop_mech, fd_r_a, userdata, flags);
        }
    }

    template <typename T> void init(T *loop_mech)
    {
        timerfd_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (timerfd_fd == -1) {
            throw std::system_error(errno, std::system_category());
        }
        systemtime_fd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC | TFD_NONBLOCK);
        if (systemtime_fd == -1) {
            close (timerfd_fd);
            throw std::system_error(errno, std::system_category());
        }

        try {
            loop_mech->add_fd_watch(timerfd_fd, &timerfd_fd, IN_EVENTS);
            loop_mech->add_fd_watch(systemtime_fd, &systemtime_fd, IN_EVENTS);
            Base::init(loop_mech);
        }
        catch (...) {
            close(timerfd_fd);
            close(systemtime_fd);
            throw;
        }
    }

    void cleanup() noexcept
    {
        Base::cleanup();
        close(timerfd_fd);
        close(systemtime_fd);
    }

    void stop_timer(timer_handle_t &timer_id, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        stop_timer_nolock(timer_id, clock);
    }

    void stop_timer_nolock(timer_handle_t &timer_id, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        timer_queue_t &queue = this->queue_for_clock(clock);
        int fd = (clock == clock_type::MONOTONIC) ? timerfd_fd : systemtime_fd;
        if (queue.is_queued(timer_id)) {
            bool was_first = (&queue.get_root()) == &timer_id;
            queue.remove(timer_id);
            if (was_first) {
                set_timer_from_queue(fd, queue);
            }
        }
    }

    // starts (if not started) a timer to timeout at the given time. Resets the expiry count to 0.
    //   enable: specifies whether to enable reporting of timeouts/intervals
    void set_timer(timer_handle_t & timer_id, const time_val &timeouttv, const time_val &intervaltv,
            bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        timespec timeout = timeouttv;
        timespec interval = intervaltv;
        timer_queue_t &queue = this->queue_for_clock(clock);

        switch (clock) {
        case clock_type::SYSTEM:
            set_timer(timer_id, timeout, interval, queue, systemtime_fd, enable);
            break;
        case clock_type::MONOTONIC:
            set_timer(timer_id, timeout, interval, queue, timerfd_fd, enable);
            break;
        default:
            DASYNQ_UNREACHABLE;
        }
    }

    // Set timer relative to current time:    
    void set_timer_rel(timer_handle_t & timer_id, const time_val &timeout, const time_val &interval,
            bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        time_val alarmtime;
        this->get_time(alarmtime, clock, false);
        alarmtime += timeout;

        set_timer(timer_id, alarmtime, interval, enable, clock);
    }
    
    ~timer_fd_events()
    {
    }
};

} // namespace dasynq

#endif /* DASYNQ_TIMERFD_H_ */
