#ifndef DASYNQ_TIMERBASE_H_INCLUDED
#define DASYNQ_TIMERBASE_H_INCLUDED

#include "dasynq-naryheap.h"

namespace dasynq {

class TimerData
{
    public:
    struct timespec interval_time; // interval (if 0, one-off timer)
    int expiry_count;  // number of times expired
    bool enabled;   // whether timer reports events
    void *userdata;

    TimerData(void *udata = nullptr) : interval_time({0,0}), expiry_count(0), enabled(true), userdata(udata)
    {
        // constructor
    }
};

class CompareTimespec
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

using timer_queue_t = NaryHeap<TimerData, struct timespec, CompareTimespec>;
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
            TimerData &data = queue.node_data(thandle);
            timespec &interval = data.interval_time;
            data.expiry_count++;
            queue.pull_root();
            if (interval.tv_sec == 0 && interval.tv_nsec == 0) {
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
                struct timespec diff;
                diff.tv_sec = curtime.tv_sec - timeout->tv_sec;
                if (curtime.tv_nsec > timeout->tv_nsec) {
                    diff.tv_nsec = curtime.tv_nsec - timeout->tv_nsec;
                }
                else {
                    diff.tv_nsec = 1000000000 - timeout->tv_nsec + curtime.tv_nsec;
                    diff.tv_sec--;
                }

                // Now we have to divide the time overrun by the period to find the
                // interval overrun. This requires a division of a value not representable
                // as a long...
                struct timespec rem;
                data.expiry_count += divide_timespec(diff, interval, rem);

                // new time is current time + interval - remainder:
                struct timespec newtime = curtime;
                newtime.tv_sec += interval.tv_sec;
                newtime.tv_nsec += interval.tv_nsec;
                if (newtime.tv_nsec > 1000000000) {
                    newtime.tv_nsec -= 1000000000;
                    newtime.tv_sec++;
                }
                newtime.tv_sec -= rem.tv_sec;
                if (rem.tv_nsec > newtime.tv_nsec) {
                    newtime.tv_nsec += 1000000000 - rem.tv_nsec;
                    newtime.tv_sec--;
                }
                else {
                    newtime.tv_nsec -= rem.tv_nsec;
                }

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
