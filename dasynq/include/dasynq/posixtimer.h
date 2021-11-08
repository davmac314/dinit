#ifndef DASYNQ_POSIXTIMER_H_
#define DASYNQ_POSIXTIMER_H_

#include <vector>
#include <utility>
#include <cstdlib>

#include <sys/time.h>
#include <ctime>
#include <csignal>

#include "config.h"
#include "timerbase.h"

namespace dasynq {

// Timer implementation based on POSIX create_timer et al.
// May require linking with -lrt

template <class Base, bool provide_mono_timer = true>
class posix_timer_events : public timer_base<Base>
{
    private:
    timer_t real_timer;
    timer_t mono_timer;

    // Set the timeout to match the first timer in the queue (disable the timer if there are no
    // active timers).
    void set_timer_from_queue(timer_t &timer, timer_queue_t &timer_queue) noexcept
    {
        struct itimerspec newalarm;

        if (timer_queue.empty()) {
            newalarm.it_value = {0, 0};
            newalarm.it_interval = {0, 0};
            timer_settime(timer, TIMER_ABSTIME, &newalarm, nullptr);
            return;
        }

        newalarm.it_interval = {0, 0};
        newalarm.it_value = timer_queue.get_root_priority();
        timer_settime(timer, TIMER_ABSTIME, &newalarm, nullptr);
    }

    protected:

    using sigdata_t = typename Base::sigdata_t;

    template <typename T>
    bool receive_signal(T & loop_mech, sigdata_t &siginfo, void *userdata)
    {
        auto &real_timer_queue = this->queue_for_clock(clock_type::SYSTEM);
        auto &mono_timer_queue = this->queue_for_clock(clock_type::MONOTONIC);

        if (siginfo.get_signo() == SIGALRM) {
            time_val curtime;

            if (! real_timer_queue.empty()) {
                this->get_time(curtime, clock_type::SYSTEM, true);
                this->process_timer_queue(real_timer_queue, curtime.get_timespec());
                set_timer_from_queue(real_timer, real_timer_queue);
            }

            if (! mono_timer_queue.empty() && provide_mono_timer) {
                this->get_time(curtime, clock_type::MONOTONIC, true);
                this->process_timer_queue(mono_timer_queue, curtime);
                set_timer_from_queue(mono_timer, mono_timer_queue);
            }

            return false; // don't disable signal watch
        }
        else {
            return Base::receive_signal(loop_mech, siginfo, userdata);
        }
    }

    timer_t &timer_for_clock(clock_type clock) noexcept
    {
        switch (clock) {
        case clock_type::MONOTONIC:
            return mono_timer;
        case clock_type::SYSTEM:
            return real_timer;
        default:
            DASYNQ_UNREACHABLE;
            std::abort();
        }
    }

    public:

    class traits_t : public Base::traits_t
    {
        constexpr static bool full_timer_support = true;
    };

    template <typename T> void init(T *loop_mech)
    {
        sigset_t sigmask;
        this->sigmaskf(SIG_UNBLOCK, nullptr, &sigmask);
        sigaddset(&sigmask, SIGALRM);
        this->sigmaskf(SIG_SETMASK, &sigmask, nullptr);
        loop_mech->add_signal_watch(SIGALRM, this);

        struct sigevent timer_sigevent;
        timer_sigevent.sigev_notify = SIGEV_SIGNAL;
        timer_sigevent.sigev_signo = SIGALRM;
        timer_sigevent.sigev_value.sival_int = 0;

        // Create the timers; throw std::system_error if we can't.
        if (timer_create(CLOCK_REALTIME, &timer_sigevent, &real_timer) == 0) {
            if (provide_mono_timer && timer_create(CLOCK_MONOTONIC, &timer_sigevent, &mono_timer) != 0) {
                timer_delete(real_timer);
                throw std::system_error(errno, std::system_category());
            }
        }
        else {
            throw std::system_error(errno, std::system_category());
        }

        try {
            Base::init(loop_mech);
        }
        catch (...) {
            cleanup();
            throw;
        }
    }

    void cleanup() noexcept
    {
        Base::cleanup();
        if (provide_mono_timer) {
            timer_delete(mono_timer);
        }
        timer_delete(real_timer);
    }

    // starts (if not started) a timer to timeout at the given time. Resets the expiry count to 0.
    //   enable: specifies whether to enable reporting of timeouts/intervals
    void set_timer(timer_handle_t &timer_id, const timespec &timeout, const timespec &interval,
            bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);

        timer_queue_t &timer_queue = this->queue_for_clock(clock);
        timer_t &timer = timer_for_clock(clock);

        auto &ts = timer_queue.node_data(timer_id);
        ts.interval_time = interval;
        ts.expiry_count = 0;
        ts.enabled = enable;

        if (timer_queue.is_queued(timer_id)) {
            // Already queued; alter timeout
            if (timer_queue.set_priority(timer_id, timeout)) {
                if (clock != clock_type::MONOTONIC || provide_mono_timer) {
                    set_timer_from_queue(timer, timer_queue);
                }
            }
        }
        else {
            if (timer_queue.insert(timer_id, timeout)) {
                if (clock != clock_type::MONOTONIC || provide_mono_timer) {
                    set_timer_from_queue(timer, timer_queue);
                }
            }
        }
    }

    // Set timer relative to current time:
    void set_timer_rel(timer_handle_t &timer_id, const timespec &timeout, const timespec &interval,
            bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        struct timespec curtime;
        this->get_time(curtime, clock, false);
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
        timer_queue_t &timer_queue = this->queue_for_clock(clock);
        timer_t &timer = timer_for_clock(clock);

        if (timer_queue.is_queued(timer_id)) {
            bool was_first = (&timer_queue.get_root()) == &timer_id;
            timer_queue.remove(timer_id);
            if (was_first && (clock != clock_type::MONOTONIC || provide_mono_timer)) {
                set_timer_from_queue(timer, timer_queue);
            }
        }
    }

    ~posix_timer_events() noexcept { }
};

} // namespace dasynq

#endif /* DASYNQ_POSIXTIMER_H_ */
