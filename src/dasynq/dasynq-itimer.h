#include <vector>
#include <utility>

#include <sys/time.h>
#include <time.h>

#include "dasynq-timerbase.h"

namespace dasynq {

// Timer implementation based on the (basically obsolete) POSIX itimer interface.

template <class Base> class ITimerEvents : public timer_base<Base>
{
    private:
    timer_queue_t timer_queue;

#if defined(CLOCK_MONOTONIC)
    static inline void get_curtime(struct timespec &curtime)
    {
        clock_gettime(CLOCK_MONOTONIC, &curtime);
    }
#else
    static inline void get_curtime(struct timespec &curtime)
    {
        struct timeval curtime_tv;
        gettimeofday(&curtime_tv, nullptr);
        curtime.tv_sec = curtime_tv.tv_sec;
        curtime.tv_nsec = curtime_tv.tv_usec * 1000;
    }
#endif
    
    // Set the timerfd timeout to match the first timer in the queue (disable the timerfd
    // if there are no active timers).
    void set_timer_from_queue()
    {
        time_val newtime;
        struct itimerval newalarm;
        if (timer_queue.empty()) {
            newalarm.it_value = {0, 0};
            newalarm.it_interval = {0, 0};
            setitimer(ITIMER_REAL, &newalarm, nullptr);
            return;
        }

        newtime = timer_queue.get_root_priority();
        
        struct timespec curtime;
        get_curtime(curtime);
        time_val curtimev = curtime;

        newalarm.it_interval = {0, 0};
        if (curtimev < newtime) {
            newalarm.it_value.tv_sec = newtime.seconds() - curtime.tv_sec;
            if (curtimev.nseconds() > newtime.nseconds()) {
                newalarm.it_value.tv_usec = (1000000000 - curtimev.nseconds()
                        + newtime.nseconds()) / 1000;
                newalarm.it_value.tv_sec--;
            }
            else {
                newalarm.it_value.tv_usec = (newtime.nseconds() - curtime.tv_nsec) / 1000;
            }
        }
        else {
            // We passed the timeout: set alarm to expire immediately (we must use {0,1} as
            // {0,0} disables the timer).
            // TODO: it would be better if we just processed the appropriate timers here,
            //       but that is complicated to get right especially if the event loop
            //       is multi-threaded.
            newalarm.it_value.tv_sec = 0;
            newalarm.it_value.tv_usec = 1;
        }

        setitimer(ITIMER_REAL, &newalarm, nullptr);
    }
    
    protected:
    
    using SigInfo = typename Base::SigInfo;

    template <typename T>
    bool receive_signal(T & loop_mech, SigInfo &siginfo, void *userdata)
    {
        if (siginfo.get_signo() == SIGALRM) {
            struct timespec curtime;
            get_curtime(curtime);
            
            timer_base<Base>::process_timer_queue(timer_queue, curtime);

            // arm timerfd with timeout from head of queue
            set_timer_from_queue();
            return false; // don't disable signal watch
        }
        else {
            return Base::receive_signal(loop_mech, siginfo, userdata);
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
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        timer_queue.allocate(h, userdata);
    }
    
    void removeTimer(timer_handle_t &timer_id, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
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
    void setTimer(timer_handle_t &timer_id, const time_val &timeouttv, const time_val &intervaltv,
            bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        timespec timeout = timeouttv;
        timespec interval = intervaltv;

        std::lock_guard<decltype(Base::lock)> guard(Base::lock);

        auto &ts = timer_queue.node_data(timer_id);
        ts.interval_time = interval;
        ts.expiry_count = 0;
        ts.enabled = enable;

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
    }

    // Set timer relative to current time:    
    void setTimerRel(timer_handle_t &timer_id, const time_val &timeouttv, const time_val &intervaltv,
            bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        timespec timeout = timeouttv;
        timespec interval = intervaltv;

        // TODO consider caching current time somehow; need to decide then when to update cached value.
        struct timespec curtime;
        get_curtime(curtime);
        curtime.tv_sec += timeout.tv_sec;
        curtime.tv_nsec += timeout.tv_nsec;
        if (curtime.tv_nsec > 1000000000) {
            curtime.tv_nsec -= 1000000000;
            curtime.tv_sec++;
        }
        setTimer(timer_id, curtime, interval, enable, clock);
    }
    
    // Enables or disabling report of timeouts (does not stop timer)
    void enableTimer(timer_handle_t &timer_id, bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        enableTimer_nolock(timer_id, enable, clock);
    }
    
    void enableTimer_nolock(timer_handle_t &timer_id, bool enable, clock_type = clock_type::MONOTONIC) noexcept
    {
        auto &node_data = timer_queue.node_data(timer_id);
        auto expiry_count = node_data.expiry_count;
        if (expiry_count != 0) {
            node_data.expiry_count = 0;
            Base::receiveTimerExpiry(timer_id, node_data.userdata, expiry_count);
        }
        else {
            timer_queue.node_data(timer_id).enabled = enable;
        }
    }

    void stop_timer(timer_handle_t &timer_id, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        stop_timer_nolock(timer_id, clock);
    }

    void stop_timer_nolock(timer_handle_t &timer_id, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        if (timer_queue.is_queued(timer_id)) {
            bool was_first = (&timer_queue.get_root()) == &timer_id;
            timer_queue.remove(timer_id);
            if (was_first) {
                set_timer_from_queue();
            }
        }
    }

    void get_time(time_val &tv, clock_type clock, bool force_update) noexcept
    {
        timespec ts;
        get_time(ts, clock, force_update);
        tv = ts;
    }

    void get_time(timespec &ts, clock_type clock, bool force_update) noexcept
    {
        get_curtime(ts);
    }
};

}
