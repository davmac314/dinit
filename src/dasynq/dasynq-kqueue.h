#include <system_error>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>

#ifdef __OpenBSD__
#include <sys/signal.h> // for __thrsigdivert aka sigtimedwait
#include <sys/syscall.h>
extern "C" { 
    int __thrsigdivert(sigset_t set, siginfo_t *info, const struct timespec * timeout);
}
#endif

#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <unistd.h>
#include <signal.h>

namespace dasynq {

template <class Base> class KqueueLoop;

class KqueueTraits
{
    template <class Base> friend class KqueueLoop;

    public:

    class SigInfo
    {
        template <class Base> friend class KqueueLoop;
        
        siginfo_t info;
        
        public:
        int get_signo() { return info.si_signo; }
        int get_sicode() { return info.si_code; }
        char * get_ssiaddr() { return info.si_addr; }
        
        void set_signo(int signo) { info.si_signo = signo; }
    };    

    class FD_r;

    // File descriptor optional storage. If the mechanism can return the file descriptor, this
    // class will be empty, otherwise it can hold a file descriptor.
    class FD_s {
        // Epoll doesn't return the file descriptor (it can, but it can't return both file
        // descriptor and user data).
        // TODO make true empty.
    };

    // File descriptor reference (passed to event callback). If the mechanism can return the
    // file descriptor, this class holds the file descriptor. Otherwise, the file descriptor
    // must be stored in an FD_s instance.
    class FD_r {
        int fd;
        public:
        int getFd(FD_s ss)
        {
            return fd;
        }
        FD_r(int nfd) : fd(nfd)
        {
        }
    };
    
    const static bool has_bidi_fd_watch = false;
    const static bool has_separate_rw_fd_watches = true;
    const static bool supports_childwatch_reservation = true;
};

#if defined(__OpenBSD__)
// OpenBSD has no sigtimedwait (or sigwaitinfo) but does have "__thrsigdivert", which is
// essentially an incomplete version of the same thing. Discussion with OpenBSD developer
// Ted Unangst suggested that the siginfo_t structure returned might not always have all
// fields set correctly. Furthermore there is a bug such that specifying a zero timeout (or
// indeed any timeout less than a tick) results in NO timeout.
static inline int sigtimedwait(const sigset_t *ssp, siginfo_t *info, struct timespec *timeout)
{
    // We know that we're only called with a timeout of 0 (which doesn't work properly) and
    // that we safely overwrite the timeout. So, we set tv_nsec to an invalid value, which
    // will cause EINVAL to be returned, but will still pick up any pending signals *first*.
    timeout->tv_nsec = 1000000001;
    return __thrsigdivert(*ssp, info, timeout);
}
#endif

template <class Base> class KqueueLoop : public Base
{
    int kqfd; // kqueue fd
    sigset_t sigmask; // enabled signal watch mask

    // Map of signal number to user data pointer. If kqueue had been better thought-through,
    // we shouldn't need this. Although we can associate user data with an EVFILT_SIGNAL kqueue
    // filter, the problem is that the kqueue signal report *coexists* with the regular signal
    // delivery mechanism without having any connection to it. Whereas regular signals can be
    // queued (especially "realtime" signals, via sigqueue()), kqueue just maintains a counter
    // of delivery attempts and clears this when we read the event. What this means is that
    // kqueue won't necessarily tell us if signals are pending, in the case that:
    //  1) it already reported the attempted signal delivery and
    //  2) more than one of the same signal was pending at that time and
    //  3) no more deliveries of the same signal have been attempted in the meantime.
    // Of course, if kqueue doesn't report the signal, then it doesn't give us the data associated
    // with the event, so we need to maintain that separately too:
    std::unordered_map<int, void *> sigdataMap;
    
    // Base contains:
    //   lock - a lock that can be used to protect internal structure.
    //          receive*() methods will be called with lock held.
    //   receiveSignal(SigInfo &, user *) noexcept
    //   receiveFdEvent(FD_r, user *, int flags) noexcept
    
    using SigInfo = KqueueTraits::SigInfo;
    using FD_r = typename KqueueTraits::FD_r;
    
    void processEvents(struct kevent *events, int r)
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        
        for (int i = 0; i < r; i++) {
            if (events[i].filter == EVFILT_SIGNAL) {
                SigInfo siginfo;
                sigset_t sset;
                sigemptyset(&sset);
                sigaddset(&sset, events[i].ident);
                struct timespec timeout;
                timeout.tv_sec = 0;
                timeout.tv_nsec = 0;
                if (sigtimedwait(&sset, &siginfo.info, &timeout) > 0) {
                    Base::receiveSignal(siginfo, (void *)events[i].udata);
                }
                
                if (events[i].ident != SIGCHLD) {
                    sigdelset(&sigmask, events[i].ident);
                    events[i].flags = EV_DISABLE;
                }
                else {
                    // TODO can we remove this SIGCHLD hack?
                    events[i].flags = EV_ENABLE;
                }
            }
            else if (events[i].filter == EVFILT_READ || events[i].filter == EVFILT_WRITE) {
                int flags = events[i].filter == EVFILT_READ ? IN_EVENTS : OUT_EVENTS;
                Base::receiveFdEvent(*this, FD_r(events[i].ident), events[i].udata, flags);
                events[i].flags = EV_DISABLE | EV_CLEAR;
                // we use EV_CLEAR to clear the EOF status of fifos/pipes (and wait for
                // another connection).
            }
            else {
                events[i].flags = EV_DISABLE;
            }
        }
        
        // Now we disable all received events, to simulate EV_DISPATCH:
        kevent(kqfd, events, r, nullptr, 0, nullptr);
    }
    
    public:
    
    /**
     * KqueueLoop constructor.
     *
     * Throws std::system_error or std::bad_alloc if the event loop cannot be initialised.
     */
    KqueueLoop()
    {
        kqfd = kqueue();
        if (kqfd == -1) {
            throw std::system_error(errno, std::system_category());
        }
        sigemptyset(&sigmask);
        Base::init(this);
    }
    
    ~KqueueLoop()
    {
        close(kqfd);
    }
    
    void setFilterEnabled(short filterType, uintptr_t ident, bool enable)
    {
        struct kevent kev;
        EV_SET(&kev, ident, filterType, enable ? EV_ENABLE : EV_DISABLE, 0, 0, 0);
        kevent(kqfd, &kev, 1, nullptr, 0, nullptr);
    }
    
    void removeFilter(short filterType, uintptr_t ident)
    {
        struct kevent kev;
        EV_SET(&kev, ident, filterType, EV_DELETE, 0, 0, 0);
        kevent(kqfd, &kev, 1, nullptr, 0, nullptr);    
    }
    
    // flags:  IN_EVENTS | OUT_EVENTS
    void addFdWatch(int fd, void *userdata, int flags)
    {
        // TODO kqueue doesn't support EVFILT_WRITE on file fd's :/
        // Presumably they cause the kevent call to fail. We could maintain
        // a separate set and use poll() (urgh).
        
        short filter = (flags & IN_EVENTS) ? EVFILT_READ : EVFILT_WRITE;
        
        struct kevent kev;
        EV_SET(&kev, fd, filter, EV_ADD, 0, 0, userdata);
        if (kevent(kqfd, &kev, 1, nullptr, 0, nullptr) == -1) {
            throw new std::system_error(errno, std::system_category());
        }
    }
    
    void addBidiFdWatch(int fd, void *userdata, int flags)
    {
        struct kevent kev[2];
        short rflags = EV_ADD | ((flags & IN_EVENTS) ? 0 : EV_DISABLE);
        short wflags = EV_ADD | ((flags & OUT_EVENTS) ? 0 : EV_DISABLE);
        EV_SET(&kev[0], fd, EVFILT_READ, rflags, 0, 0, userdata);
        EV_SET(&kev[1], fd, EVFILT_WRITE, wflags, 0, 0, userdata);
        
        if (kevent(kqfd, kev, 2, nullptr, 0, nullptr) == -1) {
            throw new std::system_error(errno, std::system_category());
        }        
    }
    
    void removeFdWatch(int fd, int flags)
    {        
        removeFilter((flags & IN_EVENTS) ? EVFILT_READ : EVFILT_WRITE, fd);
    }
    
    void removeFdWatch_nolock(int fd, int flags)
    {
        removeFdWatch(fd, flags);
    }

    void removeBidiFdWatch(int fd) noexcept
    {
        struct kevent kev[2];
        EV_SET(&kev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        EV_SET(&kev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        
        kevent(kqfd, kev, 2, nullptr, 0, nullptr);
    }
    
    void enableFdWatch(int fd, void *userdata, int flags)
    {
        setFilterEnabled((flags & IN_EVENTS) ? EVFILT_READ : EVFILT_WRITE, fd, true);
    }
    
    void enableFdWatch_nolock(int fd, void *userdata, int flags)
    {
        enableFdWatch(fd, userdata, flags);
    }
    
    void disableFdWatch(int fd, int flags)
    {
        setFilterEnabled((flags & IN_EVENTS) ? EVFILT_READ : EVFILT_WRITE, fd, false);
    }
    
    void disableFdWatch_nolock(int fd, int flags)
    {
        disableFdWatch(fd, flags);
    }
    
    // Note signal should be masked before call.
    void addSignalWatch(int signo, void *userdata)
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        
        sigdataMap[signo] = userdata;
        sigaddset(&sigmask, signo);
        
        struct kevent evt;
        EV_SET(&evt, signo, EVFILT_SIGNAL, EV_ADD, 0, 0, userdata);
        // TODO use EV_DISPATCH if available (not on OpenBSD)
        
        if (kevent(kqfd, &evt, 1, nullptr, 0, nullptr) == -1) {
            throw new std::system_error(errno, std::system_category());
        }
    }
    
    // Note, called with lock held:
    void rearmSignalWatch_nolock(int signo) noexcept
    {
        sigaddset(&sigmask, signo);
        
        struct kevent evt;
        EV_SET(&evt, signo, EVFILT_SIGNAL, EV_ENABLE, 0, 0, 0);
        // TODO use EV_DISPATCH if available (not on OpenBSD)
        
        kevent(kqfd, &evt, 1, nullptr, 0, nullptr);
    }
    
    void removeSignalWatch_nolock(int signo) noexcept
    {
        sigdelset(&sigmask, signo);
        
        struct kevent evt;
        EV_SET(&evt, signo, EVFILT_SIGNAL, EV_DELETE, 0, 0, 0);
        
        kevent(kqfd, &evt, 1, nullptr, 0, nullptr);
    }

    void removeSignalWatch(int signo) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        removeSignalWatch_nolock(signo);
    }
    
    // If events are pending, process an unspecified number of them.
    // If no events are pending, wait until one event is received and
    // process this event (and possibly any other events received
    // simultaneously).
    // If processing an event removes a watch, there is a possibility
    // that the watched event will still be reported (if it has
    // occurred) before pullEvents() returns.
    //
    //  do_wait - if false, returns immediately if no events are
    //            pending.
    void pullEvents(bool do_wait)
    {
        // We actually need to check pending signals, since
        // kqueue can count signals as they are delivered but the count is
        // cleared when we poll the kqueue, meaning that signals might still
        // be pending if they were queued multiple times at the last poll.
        
        // TODO we should only poll for signals that *have* been reported
        // as being raised more than once prior via kevent, rather than all
        // signals that have been registered - in many cases that will allow
        // us to skip the sigtimedwait call altogether.
        
        {
            std::lock_guard<decltype(Base::lock)> guard(Base::lock);
            struct timespec timeout;
            timeout.tv_sec = 0;
            timeout.tv_nsec = 0;
            SigInfo siginfo;
            int rsigno = sigtimedwait(&sigmask, &siginfo.info, &timeout);
            while (rsigno > 0) {
                // TODO avoid this hack for SIGCHLD somehow
                if (rsigno != SIGCHLD) {
                    sigdelset(&sigmask, rsigno);
                    // TODO accumulate and disable multiple filters with a single kevents call
                    //      rather than disabling each individually
                    setFilterEnabled(EVFILT_SIGNAL, rsigno, false);
                }
                Base::receiveSignal(siginfo, sigdataMap[rsigno]);
                rsigno = sigtimedwait(&sigmask, &siginfo.info, &timeout);
            }
        }
        
        struct kevent events[16];
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
        int r = kevent(kqfd, nullptr, 0, events, 16, do_wait ? nullptr : &ts);
        if (r == -1 || r == 0) {
            // signal or no events
            return;
        }
        
        processEvents(events, r);
    }

    // If events are pending, process one of them.
    // If no events are pending, wait until one event is received and
    // process this event.
    //
    //  do_wait - if false, returns immediately if no events are
    //            pending.    
    void pullOneEvent(bool do_wait)
    {
        // TODO must check for pending signals as per pullEvents()
        struct kevent events[1];
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
        int r = kevent(kqfd, nullptr, 0, events, 1, do_wait ? nullptr : &ts);
        if (r == -1 || r == 0) {
            // signal or no events
            return;
        }
    
        processEvents(events, r);
    }
    
    // Interrupt any current poll operation (pullEvents/pullOneEvent), causing
    // it to to return immediately.
    void interruptWait()
    {
        // TODO
    }
};

} // end namespace
