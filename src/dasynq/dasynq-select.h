#include <system_error>
#include <vector>
#include <atomic>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/select.h>

#include <unistd.h>
#include <signal.h>
#include <setjmp.h>

#include "dasynq-config.h"

// "pselect"-based event loop mechanism.
//

namespace dasynq {

template <class Base> class select_events;

class select_traits
{
    public:

    class sigdata_t
    {
        template <class Base> friend class select_events;

        siginfo_t info;

        public:
        // mandatory:
        int get_signo() { return info.si_signo; }
        int get_sicode() { return info.si_code; }
        pid_t get_sipid() { return info.si_pid; }
        uid_t get_siuid() { return info.si_uid; }
        void * get_siaddr() { return info.si_addr; }
        int get_sistatus() { return info.si_status; }
        int get_sival_int() { return info.si_value.sival_int; }
        void * get_sival_ptr() { return info.si_value.sival_ptr; }

        // XSI
        int get_sierrno() { return info.si_errno; }

        // XSR (streams) OB (obselete)
#if !defined(__OpenBSD__)
        // Note: OpenBSD doesn't have this; most other systems do. Technically it is part of the STREAMS
        // interface.
        int get_siband() { return info.si_band; }
#endif

        void set_signo(int signo) { info.si_signo = signo; }
    };

    class fd_r;

    // File descriptor optional storage. If the mechanism can return the file descriptor, this
    // class will be empty, otherwise it can hold a file descriptor.
    class fd_s {
        public:
        fd_s(int fd) noexcept { }

        DASYNQ_EMPTY_BODY
    };

    // File descriptor reference (passed to event callback). If the mechanism can return the
    // file descriptor, this class holds the file descriptor. Otherwise, the file descriptor
    // must be stored in an fd_s instance.
    class fd_r {
        int fd;
        public:
        int getFd(fd_s ss)
        {
            return fd;
        }
        fd_r(int nfd) : fd(nfd)
        {
        }
    };

    constexpr static bool has_bidi_fd_watch = false;
    constexpr static bool has_separate_rw_fd_watches = true;
    // requires interrupt after adding/enabling an fd:
    constexpr static bool interrupt_after_fd_add = true;
    constexpr static bool interrupt_after_signal_add = true;
    constexpr static bool supports_non_oneshot_fd = false;
};

namespace dprivate {
namespace select_mech {

// We need to declare and define a non-static data variable, "siginfo_p", in this header, without
// violating the "one definition rule". The only way to do that is via a template, even though we
// don't otherwise need a template here:
template <typename T = decltype(nullptr)> class sig_capture_templ
{
    public:
    static siginfo_t siginfo_cap;
    static sigjmp_buf rjmpbuf;

    static void signal_handler(int signo, siginfo_t *siginfo, void *v)
    {
        siginfo_cap = *siginfo;
        siglongjmp(rjmpbuf, 1);
    }
};
template <typename T> siginfo_t sig_capture_templ<T>::siginfo_cap;
template <typename T> sigjmp_buf sig_capture_templ<T>::rjmpbuf;

using sig_capture = sig_capture_templ<>;

inline void prepare_signal(int signo)
{
    struct sigaction the_action;
    the_action.sa_sigaction = sig_capture::signal_handler;
    the_action.sa_flags = SA_SIGINFO;
    sigfillset(&the_action.sa_mask);

    sigaction(signo, &the_action, nullptr);
}

inline sigjmp_buf &get_sigreceive_jmpbuf()
{
    return sig_capture::rjmpbuf;
}

inline void unprep_signal(int signo)
{
    signal(signo, SIG_DFL);
}

inline siginfo_t * get_siginfo()
{
    return &sig_capture::siginfo_cap;
}

} } // namespace dasynq :: select_mech

template <class Base> class select_events : public Base
{
    fd_set read_set;
    fd_set write_set;
    //fd_set error_set;  // logical OR of both the above
    int max_fd = 0; // highest fd in any of the sets

    sigset_t active_sigmask; // mask out unwatched signals i.e. active=0
    void * sig_userdata[NSIG];

    // userdata pointers in read and write respectively, for each fd:
    std::vector<void *> rd_udata;
    std::vector<void *> wr_udata;

    // Base contains:
    //   lock - a lock that can be used to protect internal structure.
    //          receive*() methods will be called with lock held.
    //   receive_signal(sigdata_t &, user *) noexcept
    //   receive_fd_event(fd_r, user *, int flags) noexcept

    using sigdata_t = select_traits::sigdata_t;
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
     * select_events constructor.
     *
     * Throws std::system_error or std::bad_alloc if the event loop cannot be initialised.
     */
    select_events()
    {
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        sigfillset(&active_sigmask);
        Base::init(this);
    }

    ~select_events()
    {
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

    // Note signal should be masked before call.
    void add_signal_watch(int signo, void *userdata)
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        add_signal_watch_nolock(signo, userdata);
    }

    // Note signal should be masked before call.
    void add_signal_watch_nolock(int signo, void *userdata)
    {
        sig_userdata[signo] = userdata;
        sigdelset(&active_sigmask, signo);
        dprivate::select_mech::prepare_signal(signo);
    }

    // Note, called with lock held:
    void rearm_signal_watch_nolock(int signo, void *userdata) noexcept
    {
        sig_userdata[signo] = userdata;
        sigdelset(&active_sigmask, signo);
    }

    void remove_signal_watch_nolock(int signo) noexcept
    {
        dprivate::select_mech::unprep_signal(signo);
        sigaddset(&active_sigmask, signo);
        sig_userdata[signo] = nullptr;
        // No need to signal other threads
    }

    void remove_signal_watch(int signo) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        remove_signal_watch_nolock(signo);
    }

    public:

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
        using namespace dprivate::select_mech;

        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 0;

        fd_set read_set_c;
        fd_set write_set_c;
        fd_set err_set;

        Base::lock.lock();
        read_set_c = read_set;
        write_set_c = write_set;
        err_set = read_set;

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

        volatile bool was_signalled = false;

        // using sigjmp/longjmp is ugly, but there is no other way. If a signal that we're watching is
        // received during polling, it will longjmp back to here:
        if (sigsetjmp(get_sigreceive_jmpbuf(), 1) != 0) {
            std::atomic_signal_fence(std::memory_order_acquire);
            auto * sinfo = get_siginfo();
            sigdata_t sigdata;
            sigdata.info = *sinfo;
            Base::lock.lock();
            void *udata = sig_userdata[sinfo->si_signo];
            if (udata != nullptr && Base::receive_signal(*this, sigdata, udata)) {
                sigaddset(&sigmask, sinfo->si_signo);
                sigaddset(&active_sigmask, sinfo->si_signo);
            }
            Base::lock.unlock();
            was_signalled = true;
        }

        if (was_signalled) {
            do_wait = false;
        }

        std::atomic_signal_fence(std::memory_order_release);

        int r = pselect(nfds, &read_set_c, &write_set_c, &err_set, do_wait ? nullptr : &ts, &sigmask);
        if (r == -1 || r == 0) {
            // signal or no events
            return;
        }

        process_events(&read_set_c, &write_set_c, &err_set);
    }
};

} // end namespace
