#include <vector>
#include <utility>

#include <sys/timerfd.h>
#include <time.h>

#include "dasynq-timerbase.h"
#include "dasynq-binaryheap.h"

namespace dasynq {

// We could use one timerfd per timer, but then we need to differentiate timer
// descriptors from regular file descriptors when events are reported by the loop
// mechanism so that we can correctly report a timer event or fd event.

// With a file descriptor or signal, we can use the item itself as the identifier for
// adding/removing watches. For timers, it's more complicated. When we add a timer,
// we are given a handle; we need to use this to modify the watch. We delegate the
// process of allocating a handle to a priority heap implementation (BinaryHeap).

template <class Base> class TimerFdEvents : public Base
{
    private:
    int timerfd_fd = -1;
    int systemtime_fd = -1;

    using timer_queue_t = BinaryHeap<TimerData, struct timespec, CompareTimespec>;
    timer_queue_t timer_queue;
    timer_queue_t wallclock_queue;
    
    static int divide_timespec(const struct timespec &num, const struct timespec &den) noexcept
    {
        // TODO
        return 0;
    }
    
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
    
    void process_timer(clock_type clock, int fd, timer_queue_t &queue) noexcept
    {
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

        // Peek timer queue; calculate difference between current time and timeout
        struct timespec * timeout = &queue.get_root_priority();
        while (timeout->tv_sec < curtime.tv_sec || (timeout->tv_sec == curtime.tv_sec &&
                timeout->tv_nsec <= curtime.tv_nsec)) {
            // Increment expiry count
            queue.node_data(queue.get_root()).expiry_count++;
            // (a periodic timer may have overrun; calculated below).

            auto thandle = queue.get_root();
            TimerData &data = queue.node_data(thandle);
            timespec &interval = data.interval_time;
            if (interval.tv_sec == 0 && interval.tv_nsec == 0) {
                // Non periodic timer
                queue.pull_root();
                if (data.enabled) {
                    int expiry_count = data.expiry_count;
                    data.expiry_count = 0;
                    Base::receiveTimerExpiry(thandle, data.userdata, expiry_count);
                }
                if (queue.empty()) {
                    break;
                }
            }
            else {
                // Periodic timer TODO
                // First calculate the overrun in time:
                /*
                struct timespec diff;
                diff.tv_sec = curtime.tv_sec - timeout->tv_sec;
                diff.tv_nsec = curtime.tv_nsec - timeout->tv_nsec;
                if (diff.tv_nsec < 0) {
                    diff.tv_nsec += 1000000000;
                    diff.tv_sec--;
                }
                */
                // Now we have to divide the time overrun by the period to find the
                // interval overrun. This requires a division of a value not representable
                // as a long...
                // TODO use divide_timespec
                // TODO better not to remove from queue maybe, but instead mark as inactive,
                // adjust timeout, and bubble into correct position
                // call Base::receieveTimerEvent
                // TODO
            }

            // repeat until all expired timeouts processed
            // timeout = &timer_queue[0].timeout;
            //  (shouldn't be necessary; address hasn't changed...)
        }
        // arm timerfd with timeout from head of queue
        set_timer_from_queue(fd, queue);
    }

    void setTimer(timer_handle_t & timer_id, struct timespec &timeout, struct timespec &interval,
            timer_queue_t &queue, int fd, bool enable) noexcept
    {
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

        // TODO locking (here and everywhere)
    }

    public:
    template <typename T>
    void receiveFdEvent(T &loop_mech, typename Base::FD_r fd_r, void * userdata, int flags)
    {
        if (userdata == &timerfd_fd) {
            process_timer(clock_type::MONOTONIC, timerfd_fd, timer_queue);
        }
        else if (userdata == &systemtime_fd) {
            process_timer(clock_type::SYSTEM, systemtime_fd, wallclock_queue);
        }
        else {
            Base::receiveFdEvent(loop_mech, fd_r, userdata, flags);
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
            loop_mech->addFdWatch(timerfd_fd, &timerfd_fd, IN_EVENTS);
            loop_mech->addFdWatch(systemtime_fd, &systemtime_fd, IN_EVENTS);
            Base::init(loop_mech);
        }
        catch (...) {
            close(timerfd_fd);
            close(systemtime_fd);
            throw;
        }
    }

    // Add timer, store into given handle
    void addTimer(timer_handle_t &h, void *userdata, clock_type clock = clock_type::MONOTONIC)
    {
        switch(clock) {
        case clock_type::SYSTEM:
            wallclock_queue.allocate(h, userdata);
            break;
        case clock_type::MONOTONIC:
            timer_queue.allocate(h, userdata);
            break;
        default:
            DASYNQ_UNREACHABLE;
        }
    }
    
    void removeTimer(timer_handle_t &timer_id, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        removeTimer_nolock(timer_id, clock);
    }
    
    void removeTimer_nolock(timer_handle_t &timer_id, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        switch(clock) {
        case clock_type::SYSTEM:
            if (wallclock_queue.is_queued(timer_id)) {
                wallclock_queue.remove(timer_id);
            }
            wallclock_queue.deallocate(timer_id);
            break;
        case clock_type::MONOTONIC:
            if (timer_queue.is_queued(timer_id)) {
                timer_queue.remove(timer_id);
            }
            timer_queue.deallocate(timer_id);
            break;
        default:
            DASYNQ_UNREACHABLE;
        }
    }
    
    // starts (if not started) a timer to timeout at the given time. Resets the expiry count to 0.
    //   enable: specifies whether to enable reporting of timeouts/intervals
    void setTimer(timer_handle_t & timer_id, struct timespec &timeout, struct timespec &interval,
            bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        switch (clock) {
        case clock_type::SYSTEM:
            setTimer(timer_id, timeout, interval, wallclock_queue, systemtime_fd, enable);
            break;
        case clock_type::MONOTONIC:
            setTimer(timer_id, timeout, interval, timer_queue, timerfd_fd, enable);
            break;
        default:
            DASYNQ_UNREACHABLE;
        }
    }

    // Set timer relative to current time:    
    void setTimerRel(timer_handle_t & timer_id, struct timespec &timeout, struct timespec &interval,
            bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        clockid_t sclock;
        switch (clock) {
        case clock_type::SYSTEM:
            sclock = CLOCK_REALTIME;
            break;
        case clock_type::MONOTONIC:
            sclock = CLOCK_MONOTONIC;
            break;
        default:
            DASYNQ_UNREACHABLE;
        }

        // TODO consider caching current time somehow; need to decide then when to update cached value.
        struct timespec curtime;
        clock_gettime(sclock, &curtime);
        curtime.tv_sec += timeout.tv_sec;
        curtime.tv_nsec += timeout.tv_nsec;
        if (curtime.tv_nsec > 1000000000) {
            curtime.tv_nsec -= 1000000000;
            curtime.tv_sec++;
        }

        setTimer(timer_id, curtime, interval, enable, clock);
    }
    
    // Enables or disabling report of timeouts (does not stop timer)
    void enableTimer(timer_handle_t & timer_id, bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        enableTimer_nolock(timer_id, enable, clock);
    }
    
    void enableTimer_nolock(timer_handle_t & timer_id, bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        switch (clock) {
        case clock_type::SYSTEM:
            wallclock_queue.node_data(timer_id).enabled = enable;
            break;
        case clock_type::MONOTONIC:
            timer_queue.node_data(timer_id).enabled = enable;
            break;
        default:
            DASYNQ_UNREACHABLE;
        }
    }

    ~TimerFdEvents()
    {
        close(timerfd_fd);
        close(systemtime_fd);
    }
};

}
