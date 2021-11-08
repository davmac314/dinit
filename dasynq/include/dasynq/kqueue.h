#ifndef DASYNQ_KQUEUE_H_
#define DASYNQ_KQUEUE_H_

#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <tuple>

#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include <unistd.h>
#include <csignal>

#include "config.h"

// "kqueue"-based event loop mechanism.
//
// kqueue is available on BSDs and Mac OS X, though there are subtle differences from OS to OS.
//
// kqueue supports watching file descriptors (input and output as separate watches only),
// signals, child processes, and timers. Unfortunately support for the latter two is imperfect;
// it is not possible to reserve process watches in advance; timers can only be active, count
// down immediately when created, and cannot be reset to another time. For timers especially
// the problems are significant: we can't allocate timers in advance, and we can't even feasibly
// manage our own timer queue via a single kqueue-backed timer. Therefore, an alternate timer
// mechanism must be used together with kqueue.

namespace dasynq {

template <class Base> class kqueue_loop;

class kqueue_traits
{
    template <class Base> friend class kqueue_loop;

    public:

    class sigdata_t
    {
        template <class Base> friend class kqueue_loop;
        
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
        fd_s(int) { }

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
    constexpr static bool interrupt_after_fd_add = false;
    constexpr static bool interrupt_after_signal_add = false;
    constexpr static bool supports_non_oneshot_fd = false;
};

namespace dprivate {
namespace dkqueue {

#if _POSIX_REALTIME_SIGNALS > 0

static inline void prepare_signal(int signo) { }
static inline void unprep_signal(int signo) { }
// get_siginfo is not required in this case.

#else

// If we have no sigtimedwait implementation, we have to retrieve signal data by establishing a
// signal handler.

// We need to declare and define a non-static data variable, "siginfo_p", in this header, without
// violating the "one definition rule". The only way to do that (before C++17) is via a template,
// even though we don't otherwise need a template here:
template <typename T = decltype(nullptr)> class sig_capture_templ
{
    public:
    static siginfo_t * siginfo_p;

    static void signal_handler(int signo, siginfo_t *siginfo, void *v)
    {
        *siginfo_p = *siginfo;
    }
};
template <typename T> siginfo_t * sig_capture_templ<T>::siginfo_p = nullptr;

using sig_capture = sig_capture_templ<>;

inline void prepare_signal(int signo)
{
    struct sigaction the_action;
    the_action.sa_sigaction = sig_capture::signal_handler;
    the_action.sa_flags = SA_SIGINFO;
    sigfillset(&the_action.sa_mask);

    sigaction(signo, &the_action, nullptr);
}

inline void unprep_signal(int signo)
{
    signal(signo, SIG_DFL);
}

inline bool get_siginfo(int signo, siginfo_t *siginfo)
{
    sig_capture::siginfo_p = siginfo;

    sigset_t mask;
    sigfillset(&mask);
    sigdelset(&mask, signo);
    sigsuspend(&mask);
    return true;
}

#endif
} // namespace dkqueue
} // namespace dprivate

template <class Base> class kqueue_loop : public Base
{
    int kqfd = -1; // kqueue fd

    // The kqueue signal reporting mechanism *coexists* with the regular signal
    // delivery mechanism without having any connection to it. Whereas regular signals can be
    // queued (especially "realtime" signals, via sigqueue()), kqueue just maintains a counter
    // of delivery attempts and clears this when we read the event. What this means is that
    // kqueue won't necessarily tell us if signals are pending, in the case that:
    //  1) it already reported the attempted signal delivery and
    //  2) more than instance one of the same signal was pending at that time and
    //  3) no more deliveries of the same signal have been attempted in the meantime.
    // Also, kqueue won't tell us about signals that were pending at the time the signal filter
    // was added. Finally, because pending signals can be merged, the count of delivery attempts
    // provided by kqueue does not necessarily match the number of signals actually pending.
    //
    // Note that POSIX allows for multiple instances of a signal to be pending even on systems
    // that don't support queueing of signals.
    //
    // Ultimately, this means we need to check for pending signals independently of what kqueue
    // tells us.

    // Base contains:
    //   lock - a lock that can be used to protect internal structure.
    //          receive*() methods will be called with lock held.
    //   receive_signal(sigdata_t &, user *) noexcept
    //   receive_fd_event(fd_r, user *, int flags) noexcept
    
    using sigdata_t = kqueue_traits::sigdata_t;
    using fd_r = typename kqueue_traits::fd_r;
    
    // The flag to specify poll() semantics for regular file readiness: that is, we want
    // ready-for-read to be returned even at end of file:
#if defined(NOTE_FILE_POLL)
    // FreeBSD:
    constexpr static int POLL_SEMANTICS = NOTE_FILE_POLL;
#else
    // Note that macOS has an "EV_POLL" defined that looks like it should give poll semantics
    // when passed as a flag. However, it is filtered at the syscall entry so we cannot use it in
    // kqueue. (The kernel uses it internally to implement poll()).
    constexpr static int POLL_SEMANTICS = 0;
#endif

    void process_events(struct kevent *events, int r)
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        
        for (int i = 0; i < r; i++) {
            if (events[i].filter == EVFILT_SIGNAL) {
                bool reenable = pull_signal(events[i].ident, events[i].udata);
                events[i].flags = reenable ? EV_ENABLE : EV_DISABLE;
            }
            else if (events[i].filter == EVFILT_READ || events[i].filter == EVFILT_WRITE) {
                int flags = events[i].filter == EVFILT_READ ? IN_EVENTS : OUT_EVENTS;
                auto r = Base::receive_fd_event(*this, fd_r(events[i].ident), events[i].udata, flags);
                if (std::get<0>(r) == 0) {
                    // we use EV_CLEAR to clear the EOF status of fifos/pipes (and wait for
                    // another connection).
                    events[i].flags = EV_DISABLE | EV_CLEAR;
                }
                else {
                    events[i].flags = EV_ENABLE;
                }
            }
            else {
                events[i].flags = EV_DISABLE;
            }
        }
        
        // Now we disable all received events, to simulate EV_DISPATCH:
        kevent(kqfd, events, r, nullptr, 0, nullptr);
    }
    
    // Pull a signal from pending, and report it, until it is no longer pending or the watch
    // should be disabled. Call with lock held.
    // Returns:  true if watcher should be enabled, false if disabled.
    bool pull_signal(int signo, void *userdata)
    {
        bool enable_filt = true;
        sigdata_t siginfo;

#if _POSIX_REALTIME_SIGNALS > 0
        struct timespec timeout = {0, 0};
        sigset_t sigw_mask;
        sigemptyset(&sigw_mask);
        sigaddset(&sigw_mask, signo);
        int rsigno = sigtimedwait(&sigw_mask, &siginfo.info, &timeout);
        while (rsigno > 0) {
            if (Base::receive_signal(*this, siginfo, userdata)) {
                enable_filt = false;
                break;
            }
            rsigno = sigtimedwait(&sigw_mask, &siginfo.info, &timeout);
        }
#else
        // we have no sigtimedwait.
        sigset_t pending_sigs;
        sigpending(&pending_sigs);
        while (sigismember(&pending_sigs, signo)) {
            dprivate::dkqueue::get_siginfo(signo, &siginfo.info);
            if (Base::receive_signal(*this, siginfo, userdata)) {
                enable_filt = false;
                break;
            }
            sigpending(&pending_sigs);
        }
#endif
        return enable_filt;
    }

    public:
    
    /**
     * kqueue_loop constructor.
     *
     * Throws std::system_error or std::bad_alloc if the event loop cannot be initialised.
     */
    kqueue_loop()
    {
        init();
    }

    kqueue_loop(typename Base::delayed_init d) noexcept
    {
        // delayed initialisation
    }

    void init()
    {
        kqfd = kqueue();
        if (kqfd == -1) {
            throw std::system_error(errno, std::system_category());
        }
        try {
            Base::init(this);
        }
        catch (...) {
            close(kqfd);
            throw;
        }
    }
    
    ~kqueue_loop() noexcept
    {
        if (kqfd != -1) {
            Base::cleanup();
            close(kqfd);
        }
    }
    
    void set_filter_enabled(short filterType, uintptr_t ident, void *udata, bool enable)
    {
        // Note, on OpenBSD enabling or disabling filter will not alter the filter parameters (udata etc);
        // on OS X however, it will. Therefore we set udata here (to the same value as it was originally
        // set) in order to work correctly on both kernels.
        struct kevent kev;
        int fflags = (filterType == EVFILT_READ) ? POLL_SEMANTICS : 0;
        EV_SET(&kev, ident, filterType, enable ? EV_ENABLE : EV_DISABLE, fflags, 0, udata);
        kevent(kqfd, &kev, 1, nullptr, 0, nullptr);
    }
    
    void remove_filter(short filterType, uintptr_t ident)
    {
        struct kevent kev;
        EV_SET(&kev, ident, filterType, EV_DELETE, 0, 0, 0);
        kevent(kqfd, &kev, 1, nullptr, 0, nullptr);    
    }
    
    //        fd:  file descriptor to watch
    //  userdata:  data to associate with descriptor
    //     flags:  IN_EVENTS | OUT_EVENTS | ONE_SHOT
    //             (only one of IN_EVENTS/OUT_EVENTS can be specified)
    // soft_fail:  true if unsupported file descriptors should fail by returning false instead
    //             of throwing an exception
    // returns: true on success; false if file descriptor type isn't supported and emulate == true
    // throws:  std::system_error or std::bad_alloc on failure
    bool add_fd_watch(int fd, void *userdata, int flags, bool enabled = true, bool emulate = false)
    {
        short filter = (flags & IN_EVENTS) ? EVFILT_READ : EVFILT_WRITE;

        if (filter == EVFILT_READ && POLL_SEMANTICS == 0 && emulate) {
            // We can't request poll semantics, so check for regular file:
            struct stat statbuf;
            if (fstat(fd, &statbuf) == -1) {
                throw std::system_error(errno, std::system_category());
            }
            if ((statbuf.st_mode & S_IFMT) == S_IFREG) {
                // Regular file: emulation required
                return false;
            }
        }

        int fflags = (filter == EVFILT_READ) ? POLL_SEMANTICS : 0;

        struct kevent kev;
        EV_SET(&kev, fd, filter, EV_ADD | (enabled ? 0 : EV_DISABLE), fflags, 0, userdata);
        if (kevent(kqfd, &kev, 1, nullptr, 0, nullptr) == -1) {
            // Note that kqueue supports EVFILT_READ on regular file fd's, but not EVFILT_WRITE.
            if (filter == EVFILT_WRITE && errno == EINVAL && emulate) {
                return false; // emulate
            }
            throw std::system_error(errno, std::system_category());
        }
        return true;
    }

    // returns: 0 on success
    //          IN_EVENTS  if in watch requires emulation
    //          OUT_EVENTS if out watch requires emulation
    int add_bidi_fd_watch(int fd, void *userdata, int flags, bool emulate = false)
    {
#ifdef EV_RECEIPT
        struct kevent kev[2];
        struct kevent kev_r[2];
        short rflags = EV_ADD | ((flags & IN_EVENTS) ? 0 : EV_DISABLE) | EV_RECEIPT;
        short wflags = EV_ADD | ((flags & OUT_EVENTS) ? 0 : EV_DISABLE) | EV_RECEIPT;
        EV_SET(&kev[0], fd, EVFILT_READ, rflags, POLL_SEMANTICS, 0, userdata);
        EV_SET(&kev[1], fd, EVFILT_WRITE, wflags, 0, 0, userdata);

        int r = kevent(kqfd, kev, 2, kev_r, 2, nullptr);

        if (r == -1) {
            throw std::system_error(errno, std::system_category());
        }

        // Some possibilities:
        // - both ends failed. We'll throw an error rather than allowing emulation.
        // - read watch failed, write succeeded : should not happen.
        // - read watch added, write failed: if emulate == true, succeed;
        //                                   if emulate == false, remove read and fail.

        if (kev_r[0].data != 0) {
            // read failed
            throw std::system_error(kev_r[0].data, std::system_category());
        }

        if (kev_r[1].data != 0) {
            if (emulate) {
                // We can emulate, but, do we have correct semantics?
                if (POLL_SEMANTICS != 0) {
                    return OUT_EVENTS;
                }

                // if we can't get poll semantics, emulate for read as well:
                // first remove read watch:
                EV_SET(&kev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, userdata);
                kevent(kqfd, kev, 1, nullptr, 0, nullptr);
                return IN_EVENTS | OUT_EVENTS;
            }
            // remove read watch
            EV_SET(&kev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, userdata);
            kevent(kqfd, kev, 1, nullptr, 0, nullptr);
            // throw exception
            throw std::system_error(kev_r[1].data, std::system_category());
        }

        return 0;
#else
        // OpenBSD doesn't have EV_RECEIPT: install the watches one at a time
        struct kevent kev[1];

        short rflags = EV_ADD | ((flags & IN_EVENTS) ? 0 : EV_DISABLE);
        short wflags = EV_ADD | ((flags & OUT_EVENTS) ? 0 : EV_DISABLE);
        EV_SET(&kev[0], fd, EVFILT_READ, rflags, 0, 0, userdata);

        int r = kevent(kqfd, kev, 1, nullptr, 0, nullptr);

        if (r == -1) {
            throw std::system_error(errno, std::system_category());
        }

        EV_SET(&kev[0], fd, EVFILT_WRITE, wflags, 0, 0, userdata);

        r = kevent(kqfd, kev, 1, nullptr, 0, nullptr);

        if (r == -1) {
            if (emulate) {
                return OUT_EVENTS;
            }
            // remove read watch
            EV_SET(&kev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, userdata);
            kevent(kqfd, kev, 1, nullptr, 0, nullptr);
            // throw exception
            throw std::system_error(errno, std::system_category());
        }

        return 0;
#endif
    }
    
    // flags specifies which watch to remove; ignored if the loop doesn't support
    // separate read/write watches.
    void remove_fd_watch(int fd, int flags)
    {        
        remove_filter((flags & IN_EVENTS) ? EVFILT_READ : EVFILT_WRITE, fd);
    }
    
    void remove_fd_watch_nolock(int fd, int flags)
    {
        remove_fd_watch(fd, flags);
    }

    void remove_bidi_fd_watch(int fd) noexcept
    {
        struct kevent kev[2];
        EV_SET(&kev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        EV_SET(&kev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        
        kevent(kqfd, kev, 2, nullptr, 0, nullptr);
    }
    
    void enable_fd_watch(int fd, void *userdata, int flags)
    {
        set_filter_enabled((flags & IN_EVENTS) ? EVFILT_READ : EVFILT_WRITE, fd, userdata, true);
    }
    
    void enable_fd_watch_nolock(int fd, void *userdata, int flags)
    {
        enable_fd_watch(fd, userdata, flags);
    }
    
    void disable_fd_watch(int fd, int flags)
    {
        set_filter_enabled((flags & IN_EVENTS) ? EVFILT_READ : EVFILT_WRITE, fd, nullptr, false);
    }
    
    void disable_fd_watch_nolock(int fd, int flags)
    {
        disable_fd_watch(fd, flags);
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
        dprivate::dkqueue::prepare_signal(signo);

        // We need to register the filter with the kqueue early, to avoid a race where we miss
        // signals:
        struct kevent evt;
        EV_SET(&evt, signo, EVFILT_SIGNAL, EV_ADD | EV_DISABLE, 0, 0, userdata);
        if (kevent(kqfd, &evt, 1, nullptr, 0, nullptr) == -1) {
            throw std::system_error(errno, std::system_category());
        }
        // TODO use EV_DISPATCH if available (not on OpenBSD/OS X)
        
        // The signal might be pending already but won't be reported by kqueue in that case. We can queue
        // it immediately (note that it might be pending multiple times, so we need to re-check once signal
        // processing finishes if it is re-armed).

        bool enable_filt = pull_signal(signo, userdata);

        if (enable_filt) {
            evt.flags = EV_ENABLE;
            if (kevent(kqfd, &evt, 1, nullptr, 0, nullptr) == -1) {
                throw std::system_error(errno, std::system_category());
            }
        }
    }
    
    // Note, called with lock held:
    void rearm_signal_watch_nolock(int signo, void *userdata) noexcept
    {
        if (pull_signal(signo, userdata)) {
            struct kevent evt;
            EV_SET(&evt, signo, EVFILT_SIGNAL, EV_ENABLE, 0, 0, userdata);
            // TODO use EV_DISPATCH if available (not on OpenBSD)

            kevent(kqfd, &evt, 1, nullptr, 0, nullptr);
        }
    }
    
    void remove_signal_watch_nolock(int signo) noexcept
    {
        dprivate::dkqueue::unprep_signal(signo);
        
        struct kevent evt;
        EV_SET(&evt, signo, EVFILT_SIGNAL, EV_DELETE, 0, 0, 0);
        
        kevent(kqfd, &evt, 1, nullptr, 0, nullptr);
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
    void pull_events(bool do_wait)
    {
        struct kevent events[16];
        struct timespec ts;

        // wait_ts remains null for an infinite wait; it is later set to either a 0 timeout
        // if do_wait is false (or if we otherwise won't wait due to events being detected
        // early) or is set to an appropriate timeout for the next timer's timeout.
        struct timespec *wait_ts = nullptr;

        // Check whether any timers are pending, and what the next timeout is.
        Base::lock.lock();
        this->process_monotonic_timers(do_wait, ts, wait_ts);
        Base::lock.unlock();

        if (! do_wait) {
            ts.tv_sec = 0;
            ts.tv_nsec = 0;
            wait_ts = &ts;
        }

        int r = kevent(kqfd, nullptr, 0, events, 16, wait_ts);
        if (r == -1 || r == 0) {
            // signal or no events
            if (r == 0 && do_wait) {
                // timeout:
                Base::lock.lock();
                this->process_monotonic_timers();
                Base::lock.unlock();
            }
            return;
        }
        
        ts.tv_sec = 0;
        ts.tv_nsec = 0;

        do {
            process_events(events, r);
            r = kevent(kqfd, nullptr, 0, events, 16, &ts);
        } while (r > 0);
    }
};

} // namespace dasynq

#endif /* DASYNQ_KQUEUE_H_ */
