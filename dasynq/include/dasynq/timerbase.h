#ifndef DASYNQ_TIMERBASE_H_
#define DASYNQ_TIMERBASE_H_

#include <utility>
#include <mutex>

#include <ctime>
#include <sys/time.h>

#include "daryheap.h"

namespace dasynq {

// time_val provides a wrapper around struct timespec, which overloads operators appropriately.
class time_val
{
    struct timespec time;

    public:
    using second_t = decltype(time.tv_sec);
    using nsecond_t = decltype(time.tv_nsec);

    time_val() noexcept
    {
        // uninitialised!
    }

    time_val(const struct timespec &t) noexcept
    {
        time = t;
    }

    time_val(second_t s, nsecond_t ns) noexcept
    {
        time.tv_sec = s;
        time.tv_nsec = ns;
    }

    second_t seconds() const noexcept{ return time.tv_sec; }
    nsecond_t nseconds() const noexcept { return time.tv_nsec; }

    second_t & seconds() noexcept { return time.tv_sec; }
    nsecond_t & nseconds() noexcept { return time.tv_nsec; }

    timespec & get_timespec() noexcept
    {
        return time;
    }

    const timespec & get_timespec() const noexcept
    {
        return time;
    }

    operator timespec() const noexcept
    {
       return time;
    }
};

inline time_val operator-(const time_val &t1, const time_val &t2) noexcept
{
    time_val diff;
    diff.seconds() = t1.seconds() - t2.seconds();
    if (t1.nseconds() >= t2.nseconds()) {
        diff.nseconds() = t1.nseconds() - t2.nseconds();
    }
    else {
        diff.nseconds() = 1000000000 - t2.nseconds() + t1.nseconds();
        diff.seconds()--;
    }
    return diff;
}

inline time_val operator+(const time_val &t1, const time_val &t2) noexcept
{
    auto ns = t1.nseconds() + t2.nseconds();
    auto s = t1.seconds() + t2.seconds();
    static_assert(std::numeric_limits<decltype(ns)>::max() >= 2000000000, "type too small");
    if (ns >= 1000000000) {
        ns -= 1000000000;
        s++;
    }
    return time_val(s, ns);
}

inline time_val &operator+=(time_val &t1, const time_val &t2) noexcept
{
    auto nsum = t1.nseconds() + t2.nseconds();
    t1.seconds() = t1.seconds() + t2.seconds();
    if (nsum >= 1000000000) {
        nsum -= 1000000000;
        t1.seconds()++;
    }
    t1.nseconds() = nsum;
    return t1;
}

inline time_val &operator-=(time_val &t1, const time_val &t2) noexcept
{
    time_val diff;
    t1.seconds() = t1.seconds() - t2.seconds();
    if (t1.nseconds() >= t2.nseconds()) {
        t1.nseconds() = t1.nseconds() - t2.nseconds();
    }
    else {
        t1.nseconds() = 1000000000 - t2.nseconds() + t1.nseconds();
        t1.seconds()--;
    }
    return t1;
}

inline bool operator<(const time_val &t1, const time_val &t2) noexcept
{
    if (t1.seconds() < t2.seconds()) return true;
    if (t1.seconds() == t2.seconds() && t1.nseconds() < t2.nseconds()) return true;
    return false;
}

inline bool operator==(const time_val &t1, const time_val &t2) noexcept
{
    return (t1.seconds() == t2.seconds() && t1.nseconds() == t2.nseconds());
}

inline bool operator<=(const time_val &t1, const time_val &t2) noexcept
{
    if (t1.seconds() < t2.seconds()) return true;
    if (t1.seconds() == t2.seconds() && t1.nseconds() <= t2.nseconds()) return true;
    return false;
}

inline bool operator!=(const time_val &t1, const time_val &t2) noexcept { return !(t1 == t2); }
inline bool operator>(const time_val &t1, const time_val &t2) noexcept { return t2 < t1; }
inline bool operator>=(const time_val &t1, const time_val &t2) noexcept { return t2 <= t1; }

static inline int divide_timespec(const struct timespec &num, const struct timespec &den, struct timespec &rem) noexcept;

inline int operator/(const time_val &t1, const time_val &t2) noexcept
{
    struct timespec remainder;
    return divide_timespec(t1.get_timespec(), t2.get_timespec(), remainder);
}

inline time_val & operator<<=(time_val &t, int n) noexcept
{
    for (int i = 0; i < n; i++) {
        t.seconds() *= 2;
        t.nseconds() *= 2;
        if (t.nseconds() >= 1000000000) {
            t.nseconds() -= 1000000000;
            t.seconds()++;
        }
    }
    return t;
}

inline time_val operator<<(time_val &t, int n) noexcept
{
    auto r = t;
    r <<= n;
    return r;
}

inline time_val & operator>>=(time_val &t, int n) noexcept
{
    for (int i = 0; i < n; i++) {
        bool low = t.seconds() & 1;
        t.nseconds() /= 2;
        t.nseconds() += low ? 500000000ULL : 0;
        t.seconds() /= 2;
    }
    return t;
}

inline time_val operator>>(time_val &t, int n) noexcept
{
    auto r = t;
    r >>= n;
    return r;
}

// Data corresponding to a single timer
class timer_data
{
    public:
    time_val interval_time; // interval (if 0, one-off timer)
    int expiry_count;  // number of times expired
    bool enabled;   // whether timer reports events
    void *userdata;

    timer_data(void *udata = nullptr) noexcept : interval_time(0,0), expiry_count(0), enabled(true), userdata(udata)
    {
        // constructor
    }
};

class compare_timespec
{
    public:
    bool operator()(const struct timespec &a, const struct timespec &b) noexcept
    {
        if (a.tv_sec < b.tv_sec) {
            return true;
        }

        if (a.tv_sec == b.tv_sec) {
            return a.tv_nsec < b.tv_nsec;
        }

        return false;
    }
};

using timer_queue_t = dary_heap<timer_data, time_val, compare_timespec>;
using timer_handle_t = timer_queue_t::handle_t;

static inline void init_timer_handle(timer_handle_t &hnd) noexcept
{
    timer_queue_t::init_handle(hnd);
}

static inline int divide_timespec(const struct timespec &num, const struct timespec &den, struct timespec &rem) noexcept
{
    if (num.tv_sec < den.tv_sec) {
        rem = num;
        return 0;
    }

    if (num.tv_sec == den.tv_sec) {
        if (num.tv_nsec < den.tv_nsec) {
            rem = num;
            return 0;
        }
        if (num.tv_sec == 0) {
            rem.tv_sec = 0;
            rem.tv_nsec = num.tv_nsec % den.tv_nsec;
            return num.tv_nsec / den.tv_nsec;
        }
        // num.tv_sec == den.tv_sec and both are >= 1.
        // The result can only be 1:
        rem.tv_sec = 0;
        rem.tv_nsec = num.tv_nsec - den.tv_nsec;
        return 1;
    }

    // At this point, num.tv_sec >= 1.

    time_val n = { num.tv_sec, num.tv_nsec };
    time_val d = { den.tv_sec, den.tv_nsec };
    time_val r = n;

    // starting with numerator, subtract 1*denominator
    r -= d;

    // Check now for common case: one timer expiry with no overrun
    if (r < d) {
        rem = r;
        return 1;
    }

    int nval = 1;
    int rval = 1; // we have subtracted 1*D already

    // shift denominator until it is greater than / roughly equal to numerator:
    while (d.seconds() < r.seconds()) {
        d <<= 1;
        nval *= 2;
    }

    while (nval > 0) {
        if (d <= r) {
            r -= d;
            rval += nval;
        }

        d >>= 1;
        nval /= 2;
    }

    rem = r;
    return rval;
}

template <typename Base> class timer_base : public Base
{
    private:
    timer_queue_t timer_queue;

#if defined(CLOCK_MONOTONIC)
    timer_queue_t mono_timer_queue;

    protected:
    inline timer_queue_t &queue_for_clock(clock_type clock)
    {
        if (clock == clock_type::MONOTONIC) {
            return mono_timer_queue;
        }
        else {
            return timer_queue;
        }
    }

    inline bool timer_queues_empty()
    {
        return timer_queue.empty() && mono_timer_queue.empty();
    }
#else
    // If there is no monotonic clock, map both clock_type::MONOTONIC and clock_type::SYSTEM to a
    // single clock (based on gettimeofday).
    protected:
    inline timer_queue_t &queue_for_clock(clock_type clock)
    {
        return timer_queue;
    }

    inline bool timer_queues_empty()
    {
        return timer_queue.empty();
    }
#endif

    // For the specified timer queue, issue expirations for all timers set to expire on or before the given
    // time (curtime).
    void process_timer_queue(timer_queue_t &queue, const struct timespec &curtime) noexcept
    {
        if (queue.empty()) return;

        // Peek timer queue; calculate difference between current time and timeout
        const time_val * timeout = &queue.get_root_priority();
        time_val curtime_tv = curtime;
        while (*timeout <= curtime_tv) {
            auto & thandle = queue.get_root();
            timer_data &data = queue.node_data(thandle);
            time_val &interval = data.interval_time;
            data.expiry_count++;
            queue.pull_root();
            if (interval.seconds() == 0 && interval.nseconds() == 0) {
                // Non periodic timer
                if (data.enabled) {
                    data.enabled = false;
                    int expiry_count = data.expiry_count;
                    data.expiry_count = 0;
                    Base::receive_timer_expiry(thandle, data.userdata, expiry_count);
                }
                if (queue.empty()) {
                    break;
                }
            }
            else {
                // First calculate the overrun in time:
                time_val overrun = time_val(curtime) - time_val(*timeout);

                // Now we have to divide the time overrun by the period to find the
                // interval overrun. This requires a division of a value not representable
                // as a long...
                struct timespec rem;
                data.expiry_count += divide_timespec(overrun, interval, rem);

                // new time is current time + interval - remainder:
                time_val newtime = curtime + interval - rem;

                queue.insert(thandle, newtime);
                if (data.enabled) {
                    data.enabled = false;
                    int expiry_count = data.expiry_count;
                    data.expiry_count = 0;
                    Base::receive_timer_expiry(thandle, data.userdata, expiry_count);
                }
            }

            // repeat until all expired timeouts processed
            timeout = &queue.get_root_priority();
        }
    }

    // Process timers based on the current clock time. If any timers have expired,
    // set do_wait to false; otherwise, if any timers are pending, set ts to the delay before
    // the next timer expires and set wait_ts to &ts.
    // (If no timers are active, none of the output parameters are set).
    inline void process_timers(clock_type clock, bool &do_wait, timespec &ts, timespec *&wait_ts)
    {
        timespec now;
        auto &timer_q = this->queue_for_clock(clock);
        this->get_time(now, clock, true);
        if (! timer_q.empty()) {
            const time_val &timeout = timer_q.get_root_priority();
            if (timeout <= now) {
                this->process_timer_queue(timer_q, now);
                do_wait = false; // don't wait, we have events already
            }
            else if (do_wait) {
                ts = (timeout - now);
                wait_ts = &ts;
            }
        }
    }

    // Process timers based on the current clock time. If any timers have expired,
    // set do_wait to false; otherwise, if any timers are pending, set tv to the delay before
    // the next timer expires and set wait_tv to &tv.
    // (If no timers are active, none of the output parameters are set).
    inline void process_timers(clock_type clock, bool &do_wait, timeval &tv, timeval *&wait_tv)
    {
        timespec now;
        auto &timer_q = this->queue_for_clock(clock);
        this->get_time(now, clock, true);
        if (! timer_q.empty()) {
            const time_val &timeout = timer_q.get_root_priority();
            if (timeout <= now) {
                this->process_timer_queue(timer_q, now);
                do_wait = false; // don't wait, we have events already
            }
            else if (do_wait) {
                time_val delay = (timeout - now);
                tv.tv_sec = delay.seconds();
                tv.tv_usec = (delay.nseconds() + 999) / 1000;
                wait_tv = &tv;
            }
        }
    }

    // Process monotonic timers based on the current clock time.
    inline void process_monotonic_timers()
    {
        timespec now;
        auto &timer_q = this->queue_for_clock(clock_type::MONOTONIC);
        this->get_time(now, clock_type::MONOTONIC, true);
        process_timer_queue(timer_q, now);
    }

    // Process monotonic timers based on the current clock time. If any timers have expired,
    // set do_wait to false; otherwise, if any timers are pending, set ts to the delay before
    // the next timer expires and set wait_ts to &ts.
    // (If no timers are active, none of the output parameters are set).
    inline void process_monotonic_timers(bool &do_wait, timespec &ts, timespec *&wait_ts)
    {
        process_timers(clock_type::MONOTONIC, do_wait, ts, wait_ts);
    }

    // Process monotonic timers based on the current clock time. If any timers have expired,
    // set do_wait to false; otherwise, if any timers are pending, set ts to the delay before
    // the next timer expires and set wait_ts to &ts.
    // (If no timers are active, none of the output parameters are set).
    inline void process_monotonic_timers(bool &do_wait, timeval &tv, timeval *&wait_tv)
    {
        process_timers(clock_type::MONOTONIC, do_wait, tv, wait_tv);
    }

    public:

    void get_time(time_val &tv, clock_type clock, bool force_update) noexcept
    {
        get_time(tv.get_timespec(), clock, force_update);
    }

#ifdef CLOCK_MONOTONIC
    void get_time(timespec &ts, clock_type clock, bool force_update) noexcept
    {
        clockid_t posix_clock_id = (clock == clock_type::MONOTONIC) ? CLOCK_MONOTONIC : CLOCK_REALTIME;
        clock_gettime(posix_clock_id, &ts);
    }
#else
    // If CLOCK_MONOTONIC is not defined, assume we only have gettimeofday():
    void get_time(timespec &ts, clock_type clock, bool force_update) noexcept
    {
        struct timeval curtime_tv;
        gettimeofday(&curtime_tv, nullptr);
        ts.tv_sec = curtime_tv.tv_sec;
        ts.tv_nsec = curtime_tv.tv_usec * 1000;
    }
#endif

    void add_timer_nolock(timer_handle_t &h, void *userdata, clock_type clock = clock_type::MONOTONIC)
    {
        this->queue_for_clock(clock).allocate(h, userdata);
    }

    void remove_timer(timer_handle_t &timer_id, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        remove_timer_nolock(timer_id, clock);
    }

    void remove_timer_nolock(timer_handle_t &timer_id, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        auto &timer_queue = this->queue_for_clock(clock);
        if (timer_queue.is_queued(timer_id)) {
            timer_queue.remove(timer_id);
        }
        timer_queue.deallocate(timer_id);
    }

    // Enables or disabling report of timeouts (does not stop timer)
    void enable_timer(timer_handle_t &timer_id, bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        enable_timer_nolock(timer_id, enable, clock);
    }

    void enable_timer_nolock(timer_handle_t &timer_id, bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        auto &timer_queue = this->queue_for_clock(clock);
        auto &node_data = timer_queue.node_data(timer_id);
        auto expiry_count = node_data.expiry_count;
        if (expiry_count != 0 && enable) {
            node_data.expiry_count = 0;
            Base::receive_timer_expiry(timer_id, node_data.userdata, expiry_count);
        }
        else {
            timer_queue.node_data(timer_id).enabled = enable;
        }
    }
};

} // namespace dasynq

#endif /* DASYNQ_TIMERBASE_H_ */
