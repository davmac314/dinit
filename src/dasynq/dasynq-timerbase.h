#ifndef DASYNQ_TIMERBASE_H_INCLUDED
#define DASYNQ_TIMERBASE_H_INCLUDED

#include <utility>

#include "dasynq-naryheap.h"

namespace dasynq {

// time_val provides a wrapper around struct timespec, which overloads operators appropriately.
class time_val
{
    struct timespec time;

    public:
    using second_t = decltype(time.tv_sec);
    using nsecond_t = decltype(time.tv_nsec);

    time_val()
    {
        // uninitialised!
    }

    time_val(const struct timespec &t)
    {
        time = t;
    }

    time_val(second_t s, nsecond_t ns)
    {
        time.tv_sec = s;
        time.tv_nsec = ns;
    }

    second_t seconds() const { return time.tv_sec; }
    nsecond_t nseconds() const { return time.tv_nsec; }

    second_t & seconds() { return time.tv_sec; }
    nsecond_t & nseconds() { return time.tv_nsec; }

    //void set_seconds(second_t s) { time.tv_sec = s; }
    //void set_nseconds(nsecond_t ns) { time.tv_nsec = ns; }
    //void dec_seconds() { time.tv_sec--; }
    //void inc_seconds() { time.tv_sec++; }

    operator timespec() const
    {
       return time;
    }
};

inline time_val operator-(const time_val &t1, const time_val &t2)
{
    time_val diff;
    diff.seconds() = t1.seconds() - t2.seconds();
    if (t1.nseconds() > t2.nseconds()) {
        diff.nseconds() = t1.nseconds() - t2.nseconds();
    }
    else {
        diff.nseconds() = 1000000000 - t2.nseconds() + t1.nseconds();
        diff.seconds()--;
    }
    return diff;
}

inline time_val operator+(const time_val &t1, const time_val &t2)
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

inline time_val &operator+=(time_val &t1, const time_val &t2)
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

inline bool operator<(const time_val &t1, const time_val &t2)
{
    if (t1.seconds() < t2.seconds()) return true;
    if (t1.seconds() == t2.seconds() && t1.nseconds() < t2.nseconds()) return true;
    return false;
}

inline bool operator==(const time_val &t1, const time_val &t2)
{
    return (t1.seconds() == t2.seconds() && t1.nseconds() == t2.nseconds());
}

inline bool operator<=(const time_val &t1, const time_val &t2)
{
    if (t1.seconds() < t2.seconds()) return true;
    if (t1.seconds() == t2.seconds() && t1.nseconds() <= t2.nseconds()) return true;
    return false;
}

inline bool operator!=(const time_val &t1, const time_val &t2) { return !(t1 == t2); }
inline bool operator>(const time_val &t1, const time_val &t2) { return t2 < t1; }
inline bool operator>=(const time_val &t1, const time_val &t2) { return t2 <= t1; }

// Data corresponding to a single timer
class timer_data
{
    public:
    time_val interval_time; // interval (if 0, one-off timer)
    int expiry_count;  // number of times expired
    bool enabled;   // whether timer reports events
    void *userdata;

    timer_data(void *udata = nullptr) : interval_time(0,0), expiry_count(0), enabled(true), userdata(udata)
    {
        // constructor
    }
};

class compare_timespec
{
    public:
    bool operator()(const struct timespec &a, const struct timespec &b)
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

using timer_queue_t = NaryHeap<timer_data, struct timespec, compare_timespec>;
using timer_handle_t = timer_queue_t::handle_t;

static inline void init_timer_handle(timer_handle_t &hnd) noexcept
{
    timer_queue_t::init_handle(hnd);
}

static inline int divide_timespec(const struct timespec &num, const struct timespec &den, struct timespec &rem)
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

    auto &r_sec = rem.tv_sec;
    auto &r_nsec = rem.tv_nsec;
    r_sec = num.tv_sec;
    r_nsec = num.tv_nsec;
    auto d_sec = den.tv_sec;
    auto d_nsec = den.tv_nsec;

    r_sec -= d_sec;
    if (r_nsec >= d_nsec) {
        r_nsec -= d_nsec;
    }
    else {
        r_nsec += (1000000000ULL - d_nsec);
        r_sec -= 1;
    }

    // Check now for common case: one timer expiry with no overrun
    if (r_sec < d_sec || (r_sec == d_sec && r_nsec < d_nsec)) {
        return 1;
    }

    int nval = 1;
    int rval = 1; // we have subtracted 1*D already

    // shift denominator until it is greater than/equal to numerator:
    while (d_sec < r_sec) {
        d_sec *= 2;
        d_nsec *= 2;
        if (d_nsec >= 1000000000) {
            d_nsec -= 1000000000;
            d_sec++;
        }
        nval *= 2;
    }

    while (nval > 0) {
        if (d_sec < r_sec || (d_sec == r_sec && d_nsec <= r_nsec)) {
            // subtract:
            r_sec -= d_sec;
            if (d_nsec > r_nsec) {
                r_nsec += 1000000000;
                r_sec--;
            }
            r_nsec -= d_nsec;

            rval += nval;
        }

        bool low = d_sec & 1;
        d_nsec /= 2;
        d_nsec += low ? 500000000ULL : 0;
        d_sec /= 2;
        nval /= 2;
    }

    return rval;
}

template <typename Base> class timer_base : public Base
{
    protected:

    void process_timer_queue(timer_queue_t &queue, const struct timespec &curtime)
    {
        // Peek timer queue; calculate difference between current time and timeout
        const struct timespec * timeout = &queue.get_root_priority();
        while (timeout->tv_sec < curtime.tv_sec || (timeout->tv_sec == curtime.tv_sec &&
                timeout->tv_nsec <= curtime.tv_nsec)) {
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
                    Base::receiveTimerExpiry(thandle, data.userdata, expiry_count);
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
                    Base::receiveTimerExpiry(thandle, data.userdata, expiry_count);
                }
            }

            // repeat until all expired timeouts processed
            timeout = &queue.get_root_priority();
        }
    }
};

}

#endif /* DASYNQ_TIMERBASE_H_INCLUDED */
