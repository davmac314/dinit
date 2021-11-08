#ifndef DASYNQ_ITIMER_H_
#define DASYNQ_ITIMER_H_

#include <vector>
#include <utility>

#include <csignal>
#include <sys/time.h>
#include <ctime>

#include "timerbase.h"

namespace dasynq {

// Timer implementation based on the (basically obsolete) POSIX itimer interface.

// With this timer implementation, we only have one real clock (the monotonic clock) that we can
// run a timer against. However, if the system has both clocks, we still maintain two separate queues.
// If provide_mono_timer is false we actually provide no system timer and rely on the event mechanism
// which extends this class to measure time and run timeouts (via process_monotonic_timers functions).

template <class Base, bool provide_mono_timer = true>
class itimer_events : public timer_base<Base>
{
    private:
    
    // Set the alarm timeout to match the first timer in the queue (disable the alarm if there are no
    // active timers).
    void set_timer_from_queue()
    {
        time_val newtime;
        struct itimerval newalarm;

        bool interval_set = false;
        time_val interval_tv = {0, 0};

        auto &timer_queue = this->queue_for_clock(clock_type::SYSTEM);
        if (! timer_queue.empty()) {
            newtime = timer_queue.get_root_priority();

            time_val curtimev;
            timer_base<Base>::get_time(curtimev, clock_type::SYSTEM, true);

            // interval before next timeout:
            if (curtimev < newtime) {
                interval_tv = newtime - curtimev;
            }

            interval_set = true;
        }

#ifdef CLOCK_MONOTONIC
        auto &mono_timer_queue = this->queue_for_clock(clock_type::MONOTONIC);

        if (! mono_timer_queue.empty()) {

            // If we have a separate monotonic clock, we get the interval for the expiry of the next monotonic
            // timer and use the lesser of the system interval and monotonic interval:
            time_val mono_newtime = mono_timer_queue.get_root_priority();

            time_val curtimev_mono;
            timer_base<Base>::get_time(curtimev_mono, clock_type::MONOTONIC, true);

            time_val interval_mono = {0, 0};
            if (curtimev_mono < mono_newtime) {
                interval_mono = mono_newtime - curtimev_mono;
            }

            if (! interval_set || interval_mono < interval_tv) {
                interval_tv = interval_mono;
            }

            interval_set = true;
        }
#endif

        newalarm.it_value.tv_sec = interval_tv.seconds();
        newalarm.it_value.tv_usec = interval_tv.nseconds() / 1000;
        newalarm.it_interval.tv_sec = 0;
        newalarm.it_interval.tv_usec = 0;

        if (interval_set && newalarm.it_value.tv_sec == 0 && newalarm.it_value.tv_usec == 0) {
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
    
    using sigdata_t = typename Base::sigdata_t;

    template <typename T>
    bool receive_signal(T & loop_mech, sigdata_t &siginfo, void *userdata)
    {
        if (siginfo.get_signo() == SIGALRM) {
            process_timers();
            return false; // don't disable signal watch
        }
        else {
            return Base::receive_signal(loop_mech, siginfo, userdata);
        }
    }

#ifdef CLOCK_MONOTONIC
    // We need to override the monotonic timer processing functions from timer_base to check both
    // timer queues, since we aren't actually providing a separate timer for the system clock:

    inline void process_monotonic_timers(bool &do_wait, timeval &tv, timeval *&wait_tv)
    {
        // We need to process both timer queues and set tv/wait_tv according to which has the
        // timer that will expire soonest:
        timer_base<Base>::process_timers(clock_type::MONOTONIC, do_wait, tv, wait_tv);

        if (! do_wait) {
            timer_base<Base>::process_timers(clock_type::SYSTEM, do_wait, tv, wait_tv);
        }
        else {
            timeval mono_tv = tv;
            timer_base<Base>::process_timers(clock_type::SYSTEM, do_wait, tv, wait_tv);
            if (mono_tv.tv_sec < tv.tv_sec ||
                    (mono_tv.tv_sec == tv.tv_sec && mono_tv.tv_usec < tv.tv_usec)) {
                tv = mono_tv;
            }
        }
    }

    inline void process_monotonic_timers(bool &do_wait, timespec &ts, timespec *&wait_ts)
    {
        // We need to process both timer queues and set tv/wait_tv according to which has the
        // timer that will expire soonest:
        timer_base<Base>::process_timers(clock_type::MONOTONIC, do_wait, ts, wait_ts);

        if (! do_wait) {
            timer_base<Base>::process_timers(clock_type::SYSTEM, do_wait, ts, wait_ts);
        }
        else {
            timespec mono_ts = ts;
            timer_base<Base>::process_timers(clock_type::SYSTEM, do_wait, ts, wait_ts);
            ts = std::min(time_val(ts), time_val(mono_ts));
        }
    }

    // Process monotonic timers based on the current clock time. However, we shadow the
    // function and provide an implementation which also processes the system clock timer queue.
    inline void process_monotonic_timers()
    {
        auto &mono_timer_queue = this->queue_for_clock(clock_type::MONOTONIC);
        if (! mono_timer_queue.empty()) {
            struct timespec curtime_mono;
            timer_base<Base>::get_time(curtime_mono, clock_type::MONOTONIC, true);
            timer_base<Base>::process_timer_queue(mono_timer_queue, curtime_mono);
        }

        auto &sys_timer_queue = this->queue_for_clock(clock_type::SYSTEM);
        if (! sys_timer_queue.empty()) {
            struct timespec curtime_sys;
            timer_base<Base>::get_time(curtime_sys, clock_type::SYSTEM, true);
            timer_base<Base>::process_timer_queue(sys_timer_queue, curtime_sys);
        }
    }

#endif

    void process_timers()
    {
        auto &timer_queue = this->queue_for_clock(clock_type::SYSTEM);
        if (! timer_queue.empty()) {
            struct timespec curtime;
            timer_base<Base>::get_time(curtime, clock_type::SYSTEM, true);
            timer_base<Base>::process_timer_queue(timer_queue, curtime);
        }
        
#ifdef CLOCK_MONOTONIC
        if (provide_mono_timer) {
            auto &mono_timer_queue = this->queue_for_clock(clock_type::MONOTONIC);
            if (! mono_timer_queue.empty()) {
                struct timespec curtime_mono;
                timer_base<Base>::get_time(curtime_mono, clock_type::MONOTONIC, true);
                timer_base<Base>::process_timer_queue(mono_timer_queue, curtime_mono);
            }
        }
#endif

        if (provide_mono_timer) {
            // arm alarm with timeout from head of queue
            set_timer_from_queue();
        }
    }

    public:

    class traits_t : public Base::traits_t
    {
        constexpr static bool full_timer_support = false;
    };

    template <typename T> void init(T *loop_mech)
    {
        if (provide_mono_timer) {
            sigset_t sigmask;
            this->sigmaskf(SIG_UNBLOCK, nullptr, &sigmask);
            sigaddset(&sigmask, SIGALRM);
            this->sigmaskf(SIG_SETMASK, &sigmask, nullptr);
            loop_mech->add_signal_watch(SIGALRM, this);
        }
        Base::init(loop_mech);
    }
    
    // starts (if not started) a timer to timeout at the given time. Resets the expiry count to 0.
    //   enable: specifies whether to enable reporting of timeouts/intervals
    void set_timer(timer_handle_t &timer_id, const time_val &timeouttv, const time_val &intervaltv,
            bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        auto &timer_queue = this->queue_for_clock(clock);
        timespec timeout = timeouttv;
        timespec interval = intervaltv;

        std::lock_guard<decltype(Base::lock)> guard(Base::lock);

        auto &ts = timer_queue.node_data(timer_id);
        ts.interval_time = interval;
        ts.expiry_count = 0;
        ts.enabled = enable;

        bool do_set_timer;
        if (timer_queue.is_queued(timer_id)) {
            // Already queued; alter timeout
            do_set_timer = timer_queue.set_priority(timer_id, timeout);
        }
        else {
            do_set_timer = timer_queue.insert(timer_id, timeout);
        }

        if (do_set_timer) {
            if (provide_mono_timer) {
                set_timer_from_queue();
            }
            else {
                this->interrupt_wait();
            }
        }
    }

    // Set timer relative to current time:    
    void set_timer_rel(timer_handle_t &timer_id, const time_val &timeouttv, const time_val &intervaltv,
            bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        timespec timeout = timeouttv;
        timespec interval = intervaltv;

        struct timespec curtime;
        timer_base<Base>::get_time(curtime, clock, false);
        curtime.tv_sec += timeout.tv_sec;
        curtime.tv_nsec += timeout.tv_nsec;
        if (curtime.tv_nsec > 1000000000) {
            curtime.tv_nsec -= 1000000000;
            curtime.tv_sec++;
        }
        set_timer(timer_id, curtime, interval, enable, clock);
    }
    
    void stop_timer(timer_handle_t &timer_id, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        stop_timer_nolock(timer_id, clock);
    }

    void stop_timer_nolock(timer_handle_t &timer_id, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        auto &timer_queue = this->queue_for_clock(clock);
        if (timer_queue.is_queued(timer_id)) {
            bool was_first = (&timer_queue.get_root()) == &timer_id;
            timer_queue.remove(timer_id);
            if (was_first && provide_mono_timer) {
                set_timer_from_queue();
            }
        }
    }
};

} // namespace dasynq

#endif /* DASYNQ_ITIMER_H_ */
