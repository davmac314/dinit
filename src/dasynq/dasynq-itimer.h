#include <vector>
#include <utility>

#include <sys/time.h>
#include <time.h>

#include "dasynq-timerbase.h"
#include "dasynq-binaryheap.h"

namespace dasynq {

// Timer implementation based on the (basically obselete) POSIX itimer interface.

template <class Base> class ITimerEvents : public Base
{
    private:
    int timerfd_fd = -1;

    BinaryHeap<TimerData, struct timespec, CompareTimespec> timer_queue;
    
#if defined(__APPLE__)
#define itimerspec itimerval
#endif
    
    static int divide_timespec(const struct timespec &num, const struct timespec &den)
    {
        // TODO
        return 0;
    }
    
    // Set the timerfd timeout to match the first timer in the queue (disable the timerfd
    // if there are no active timers).
    void set_timer_from_queue()
    {
        struct itimerspec newtime;
        if (timer_queue.empty()) {
            newtime.it_value = {0, 0};
            newtime.it_interval = {0, 0};
        }
        else {
#if defined(__APPLE__)
            auto &rp = timer_queue.get_root_priority();
            newtime.it_value.tv_sec = rp.tv_sec;
            newtime.it_value.tv_usec = rp.tv_nsec / 1000;
#else
            newtime.it_value = timer_queue.get_root_priority();
            newtime.it_interval = {0, 0};
#endif
        }
        
        struct timespec curtime;
#if defined(__APPLE__)
        struct timeval curtime_tv;
        gettimeofday(&curtime_tv, nullptr);
        curtime.tv_sec = curtime_tv.tv_sec;
        curtime.tv_nsec = curtime_tv.tv_usec * 1000;
#else
        clock_gettime(CLOCK_MONOTONIC, &curtime);
#endif
        struct itimerval newalarm;
        newalarm.it_interval = {0, 0};
        newalarm.it_value.tv_sec = newtime.it_value.tv_sec - curtime.tv_sec;
#if defined(__APPLE__)
        newalarm.it_value.tv_usec = newtime.it_value.tv_usec - curtime.tv_nsec / 1000;
#else
        newalarm.it_value.tv_usec = (newtime.it_value.tv_nsec - curtime.tv_nsec) / 1000;
#endif
        if (newalarm.it_value.tv_usec < 0) {
            newalarm.it_value.tv_usec += 1000000;
            newalarm.it_value.tv_sec--;
        }
        setitimer(ITIMER_REAL, &newalarm, nullptr);
    }

#if defined(__APPLE__)
#undef itimerspec
#endif
    
    protected:
    
    using SigInfo = typename Base::SigInfo;

    template <typename T>
    bool receiveSignal(T & loop_mech, SigInfo &siginfo, void *userdata)
    {
        if (siginfo.get_signo() == SIGALRM) {
            struct timespec curtime;
#if defined(__APPLE__)
            struct timeval curtime_tv;
            gettimeofday(&curtime_tv, nullptr);
            curtime.tv_sec = curtime_tv.tv_sec;
            curtime.tv_nsec = curtime_tv.tv_usec * 1000;
#else
            clock_gettime(CLOCK_MONOTONIC, &curtime);
            // should we use the REALTIME clock instead? I have no idea :/
#endif
            
            // Peek timer queue; calculate difference between current time and timeout
            struct timespec * timeout = &timer_queue.get_root_priority();
            while (timeout->tv_sec < curtime.tv_sec || (timeout->tv_sec == curtime.tv_sec &&
                    timeout->tv_nsec <= curtime.tv_nsec)) {
                // Increment expiry count
                timer_queue.node_data(timer_queue.get_root()).expiry_count++;
                // (a periodic timer may have overrun; calculated below).
                
                auto thandle = timer_queue.get_root();
                TimerData &data = timer_queue.node_data(thandle);
                timespec &interval = data.interval_time;
                if (interval.tv_sec == 0 && interval.tv_nsec == 0) {
                    // Non periodic timer
                    timer_queue.pull_root();
                    if (data.enabled) {
                        int expiry_count = data.expiry_count;
                        data.expiry_count = 0;
                        Base::receiveTimerExpiry(thandle, timer_queue.node_data(thandle).userdata, expiry_count);
                    }
                    if (timer_queue.empty()) {
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
            set_timer_from_queue();
            loop_mech.rearmSignalWatch_nolock(SIGALRM);
            return false; // don't disable signal watch
        }
        else {
            return Base::receiveSignal(loop_mech, siginfo, userdata);
        }
    }
        
    public:

    template <typename T> void init(T *loop_mech)
    {
        sigset_t sigmask;
        sigprocmask(SIG_UNBLOCK, nullptr, &sigmask);
        sigaddset(&sigmask, SIGALRM);
        sigprocmask(SIG_SETMASK, &sigmask, nullptr);
        loop_mech->addSignalWatch(SIGALRM, nullptr);
        Base::init(loop_mech);
    }

    void addTimer(timer_handle_t &h, void *userdata, clock_type clock = clock_type::MONOTONIC)
    {
        timer_queue.allocate(h, userdata);
    }
    
    void removeTimer(timer_handle_t &timer_id, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        removeTimer_nolock(timer_id, clock);
    }
    
    void removeTimer_nolock(timer_handle_t &timer_id, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        if (timer_queue.is_queued(timer_id)) {
            timer_queue.remove(timer_id);
        }
        timer_queue.deallocate(timer_id);
    }
    
    // starts (if not started) a timer to timeout at the given time. Resets the expiry count to 0.
    //   enable: specifies whether to enable reporting of timeouts/intervals
    void setTimer(timer_handle_t &timer_id, struct timespec &timeout, struct timespec &interval,
            bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        auto &ts = timer_queue.node_data(timer_id);
        ts.interval_time = interval;
        ts.expiry_count = 0;

        // TODO also update interval / enabled
        
        if (timer_queue.is_queued(timer_id)) {
            // Already queued; alter timeout
            if (timer_queue.set_priority(timer_id, timeout)) {
                set_timer_from_queue();
            }
        }
        else {
            if (timer_queue.insert(timer_id, timeout)) {
                set_timer_from_queue();
            }
        }
        
        // TODO locking (here and everywhere)
    }

    // Set timer relative to current time:    
    void setTimerRel(timer_handle_t &timer_id, struct timespec &timeout, struct timespec &interval,
            bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        // TODO consider caching current time somehow; need to decide then when to update cached value.
        struct timespec curtime;
#if defined(__APPLE__)
        struct timeval curtime_tv;
        gettimeofday(&curtime_tv, nullptr);
        curtime.tv_sec = curtime_tv.tv_sec;
        curtime.tv_nsec = curtime_tv.tv_usec * 1000;
#else
        clock_gettime(CLOCK_MONOTONIC, &curtime);
#endif
        curtime.tv_sec += timeout.tv_sec;
        curtime.tv_nsec += timeout.tv_nsec;
        if (curtime.tv_nsec > 1000000000) {
            curtime.tv_nsec -= 1000000000;
            curtime.tv_sec++;
        }
        setTimer(timer_id, curtime, interval, enable);
    }
    
    // Enables or disabling report of timeouts (does not stop timer)
    void enableTimer(timer_handle_t &timer_id, bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        enableTimer_nolock(timer_id, enable, clock);
    }
    
    void enableTimer_nolock(timer_handle_t &timer_id, bool enable, clock_type = clock_type::MONOTONIC) noexcept
    {
        timer_queue.node_data(timer_id).enabled = enable;
    }
};

}
