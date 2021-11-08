#ifndef DASYNQ_PSELECT_H_
#define DASYNQ_PSELECT_H_

#include "select.h"
#include "signal.h"

namespace dasynq {

template <class Base> class pselect_events : public signal_events<Base, false>
{
    fd_set read_set;
    fd_set write_set;
    //fd_set error_set;  // logical OR of both the above
    int max_fd = -1; // highest fd in any of the sets, -1 if not initialised

    // userdata pointers in read and write respectively, for each fd:
    std::vector<void *> rd_udata;
    std::vector<void *> wr_udata;

    // Base contains:
    //   lock - a lock that can be used to protect internal structure.
    //          receive*() methods will be called with lock held.
    //   receive_signal(sigdata_t &, user *) noexcept
    //   receive_fd_event(fd_r, user *, int flags) noexcept

    using fd_r = typename select_traits::fd_r;

    void process_events(fd_set *read_set_p, fd_set *write_set_p, fd_set *error_set_p)
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);

        // Note: if error is set, report read-ready.

        for (int i = 0; i <= max_fd; i++) {
            if (FD_ISSET(i, read_set_p) || FD_ISSET(i, error_set_p)) {
                if (FD_ISSET(i, &read_set) && rd_udata[i] != nullptr) {
                    // report read
                    auto r = Base::receive_fd_event(*this, fd_r(i), rd_udata[i], IN_EVENTS);
                    if (std::get<0>(r) == 0) {
                        FD_CLR(i, &read_set);
                    }
                }
            }
        }

        for (int i = 0; i <= max_fd; i++) {
            if (FD_ISSET(i, write_set_p)) {
                if (FD_ISSET(i, &write_set) && wr_udata[i] != nullptr) {
                    // report write
                    auto r = Base::receive_fd_event(*this, fd_r(i), wr_udata[i], OUT_EVENTS);
                    if (std::get<0>(r) == 0) {
                        FD_CLR(i, &write_set);
                    }
                }
            }
        }
    }

    public:

    /**
     * pselect_events constructor.
     *
     * Throws std::system_error or std::bad_alloc if the event loop cannot be initialised.
     */
    pselect_events()
    {
        init();
    }

    pselect_events(typename Base::delayed_init d) noexcept
    {
        // delayed initialisation
    }

    void init()
    {
        max_fd = 0;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        Base::init(this);
    }

    ~pselect_events() noexcept
    {
        if (max_fd != -1) {
            Base::cleanup();
        }
    }

    //        fd:  file descriptor to watch
    //  userdata:  data to associate with descriptor
    //     flags:  IN_EVENTS | OUT_EVENTS | ONE_SHOT
    //             (only one of IN_EVENTS/OUT_EVENTS can be specified)
    // soft_fail:  true if unsupported file descriptors should fail by returning false instead
    //             of throwing an exception
    // returns: true on success; false if file descriptor type isn't supported and emulate == true
    // throws:  std::system_error or std::bad_alloc on failure
    bool add_fd_watch(int fd, void *userdata, int flags, bool enabled = true, bool soft_fail = false)
    {
        if (fd >= FD_SETSIZE) {
            throw std::system_error(EMFILE, std::system_category());
        }

        if (flags & IN_EVENTS) {
            FD_SET(fd, &read_set);
            if (size_t(fd) >= rd_udata.size()) {
                rd_udata.resize(fd + 1);
            }
            rd_udata[fd] = userdata;
        }
        else {
            FD_SET(fd, &write_set);
            if (size_t(fd) >= wr_udata.size()) {
                wr_udata.resize(fd + 1);
            }
            wr_udata[fd] = userdata;
        }

        max_fd = std::max(fd, max_fd);

        return true;
    }

    // returns: 0 on success
    //          IN_EVENTS  if in watch requires emulation
    //          OUT_EVENTS if out watch requires emulation
    int add_bidi_fd_watch(int fd, void *userdata, int flags, bool emulate = false)
    {
        if (fd >= FD_SETSIZE) {
            throw std::system_error(EMFILE, std::system_category());
        }

        if (flags & IN_EVENTS) {
            FD_SET(fd, &read_set);
            if (size_t(fd) >= rd_udata.size()) {
                rd_udata.resize(fd + 1);
            }
            rd_udata[fd] = userdata;
        }
        if (flags & OUT_EVENTS) {
            FD_SET(fd, &write_set);
            if (size_t(fd) >= wr_udata.size()) {
                wr_udata.resize(fd + 1);
            }
            wr_udata[fd] = userdata;
        }

        max_fd = std::max(fd, max_fd);

        return 0;
    }

    // flags specifies which watch to remove; ignored if the loop doesn't support
    // separate read/write watches.
    void remove_fd_watch_nolock(int fd, int flags)
    {
        if (flags & IN_EVENTS) {
            FD_CLR(fd, &read_set);
            rd_udata[fd] = nullptr;
        }
        if (flags & OUT_EVENTS) {
            FD_CLR(fd, &write_set);
            wr_udata[fd] = nullptr;
        }
    }

    void remove_fd_watch(int fd, int flags)
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        remove_fd_watch_nolock(fd, flags);
    }

    void remove_bidi_fd_watch(int fd) noexcept
    {
        FD_CLR(fd, &read_set);
        FD_CLR(fd, &write_set);
    }

    void enable_fd_watch_nolock(int fd, void *userdata, int flags)
    {
        if (flags & IN_EVENTS) {
            FD_SET(fd, &read_set);
        }
        else {
            FD_SET(fd, &write_set);
        }
    }

    void enable_fd_watch(int fd, void *userdata, int flags)
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        enable_fd_watch_nolock(fd, userdata, flags);
    }

    void disable_fd_watch_nolock(int fd, int flags)
    {
        if (flags & IN_EVENTS) {
            FD_CLR(fd, &read_set);
        }
        else {
            FD_CLR(fd, &write_set);
        }
    }

    void disable_fd_watch(int fd, int flags)
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        disable_fd_watch_nolock(fd, flags);
    }

    // If events are pending, process an unspecified number of them.
    // If no events are pending, wait until one event is received and
    // process this event (and possibly any other events received
    // simultaneously).
    // If processing an event removes a watch, there is a possibility
    // that the watched event will still be reported (if it has
    // occurred) before pull_events() returns.
    //
    //  do_wait - if false, returns immediately if no events are
    //            pending.
    void pull_events(bool do_wait) noexcept
    {
        struct timespec ts;
        struct timespec *wait_ts = nullptr;

        Base::lock.lock();

        // Check whether any timers are pending, and what the next timeout is.
        this->process_monotonic_timers(do_wait, ts, wait_ts);

        fd_set read_set_c;
        fd_set write_set_c;
        fd_set err_set;

        read_set_c = read_set;
        write_set_c = write_set;
        err_set = read_set;

        const sigset_t &active_sigmask = this->get_active_sigmask();

        sigset_t sigmask;
        this->sigmaskf(SIG_UNBLOCK, nullptr, &sigmask);

        // This is horrible, but hopefully will be optimised well. POSIX gives no way to combine signal
        // sets other than this.
        for (int i = 1; i < NSIG; i++) {
            if (! sigismember(&active_sigmask, i)) {
                sigdelset(&sigmask, i);
            }
        }
        int nfds = max_fd + 1;
        Base::lock.unlock();

        // using sigjmp/longjmp is ugly, but there is no other way. If a signal that we're watching is
        // received during polling, it will longjmp back to here:
        if (sigsetjmp(this->get_sigreceive_jmpbuf(), 1) != 0) {
            this->process_signal(sigmask);
            do_wait = false;
        }

        if (! do_wait) {
            ts.tv_sec = 0;
            ts.tv_nsec = 0;
            wait_ts = &ts;
        }

        int r = pselect(nfds, &read_set_c, &write_set_c, &err_set, wait_ts, &sigmask);

        if (r == -1 || r == 0) {
            // signal or no events
            if (r == 0) {
                if (! do_wait) {
                    // At least on Mac OS, pselect doesn't seem to give us a pending signal
                    // if we have a zero timeout. Force detection using sigmask:
                    sigset_t origmask;
                    this->sigmaskf(SIG_SETMASK, &sigmask, &origmask);
                    this->sigmaskf(SIG_SETMASK, &origmask, nullptr);
                }
                else {
                    // timeout:
                    Base::lock.lock();
                    this->process_monotonic_timers();
                    Base::lock.unlock();
                }
            }
            return;
        }

        process_events(&read_set_c, &write_set_c, &err_set);
    }
};

} // namespace dasynq

#endif /* DASYNQ_PSELECT_H_ */
