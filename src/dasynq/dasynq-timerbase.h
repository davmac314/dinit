#ifndef DASYNQ_TIMERBASE_H_INCLUDED
#define DASYNQ_TIMERBASE_H_INCLUDED

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

using timer_handle_t = BinaryHeap<TimerData, struct timespec, CompareTimespec>::handle_t;

static void init_timer_handle(timer_handle_t &hnd) noexcept
{
    BinaryHeap<TimerData, struct timespec, CompareTimespec>::init_handle(hnd);
}

}

#endif /* DASYNQ_TIMERBASE_H_INCLUDED */
