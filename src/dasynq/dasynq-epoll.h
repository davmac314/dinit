#include <system_error>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <unistd.h>
#include <signal.h>

namespace dasynq {

template <class Base> class EpollLoop;

class EpollTraits
{
    template <class Base> friend class EpollLoop;

    public:

    class SigInfo
    {
        template <class Base> friend class EpollLoop;
        
        struct signalfd_siginfo info;
        
        public:
        int get_signo() { return info.ssi_signo; }
        int get_sicode() { return info.ssi_code; }
        int get_siint() { return info.ssi_int; }
        int get_ssiptr() { return info.ssi_ptr; }
        int get_ssiaddr() { return info.ssi_addr; }
        
        void set_signo(int signo) { info.ssi_signo = signo; }
    };    

    class FD_r;

    // File descriptor optional storage. If the mechanism can return the file descriptor, this
    // class will be empty, otherwise it can hold a file descriptor.
    class FD_s {
        friend class FD_r;
        
        // Epoll doesn't return the file descriptor (it can, but it can't return both file
        // descriptor and user data).
        int fd;
    };

    // File descriptor reference (passed to event callback). If the mechanism can return the
    // file descriptor, this class holds the file descriptor. Otherwise, the file descriptor
    // must be stored in an FD_s instance.
    class FD_r {
        public:
        int getFd(FD_s ss)
        {
            return ss.fd;
        }
    };
    
    const static bool has_bidi_fd_watch = true;
    const static bool has_separate_rw_fd_watches = false;
    const static bool supports_childwatch_reservation = true;
};


template <class Base> class EpollLoop : public Base
{
    int epfd; // epoll fd
    int sigfd; // signalfd fd; -1 if not initialised
    sigset_t sigmask;

    std::unordered_map<int, void *> sigdataMap;

    // Base contains:
    //   lock - a lock that can be used to protect internal structure.
    //          receive*() methods will be called with lock held.
    //   receiveSignal(SigInfo &, user *) noexcept
    //   receiveFdEvent(FD_r, user *, int flags) noexcept
    
    using SigInfo = EpollTraits::SigInfo;
    using FD_r = typename EpollTraits::FD_r;
    
    void processEvents(epoll_event *events, int r)
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        
        for (int i = 0; i < r; i++) {
            void * ptr = events[i].data.ptr;
            
            if (ptr == &sigfd) {
                // Signal
                SigInfo siginfo;
                while (true) {
                    int r = read(sigfd, &siginfo.info, sizeof(siginfo.info));
                    if (r == -1) break;
                    auto iter = sigdataMap.find(siginfo.get_signo());
                    if (iter != sigdataMap.end()) {
                        void *userdata = (*iter).second;
                        if (Base::receiveSignal(*this, siginfo, userdata)) {
                            sigdelset(&sigmask, siginfo.get_signo());
                        }
                    }
                }
                signalfd(sigfd, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
            }
            else {
                int flags = 0;
                (events[i].events & EPOLLIN) && (flags |= IN_EVENTS);
                (events[i].events & EPOLLHUP) && (flags |= IN_EVENTS);
                (events[i].events & EPOLLOUT) && (flags |= OUT_EVENTS);
                (events[i].events & EPOLLERR) && (flags |= IN_EVENTS | OUT_EVENTS | ERR_EVENTS);
                Base::receiveFdEvent(*this, FD_r(), ptr, flags);
            }            
        }
    }
    
    public:
    
    /**
     * EpollLoop constructor.
     *
     * Throws std::system_error or std::bad_alloc if the event loop cannot be initialised.
     */
    EpollLoop() : sigfd(-1)
    {
        epfd = epoll_create1(EPOLL_CLOEXEC);
        if (epfd == -1) {
            throw std::system_error(errno, std::system_category());
        }
        sigemptyset(&sigmask);
        Base::init(this);
    }
    
    ~EpollLoop()
    {
        close(epfd);
        if (sigfd != -1) {
            close(sigfd);
        }
    }
    
    // flags:  IN_EVENTS | OUT_EVENTS | ONE_SHOT
    void addFdWatch(int fd, void *userdata, int flags, bool enabled = true)
    {
        struct epoll_event epevent;
        // epevent.data.fd = fd;
        epevent.data.ptr = userdata;
        epevent.events = 0;
        
        if (flags & ONE_SHOT) {
            epevent.events = EPOLLONESHOT;
        }
        if ((flags & IN_EVENTS) && enabled) {
            epevent.events |= EPOLLIN;
        }
        if ((flags & OUT_EVENTS) && enabled) {
            epevent.events |= EPOLLOUT;
        }

        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &epevent) == -1) {
            throw new std::system_error(errno, std::system_category());        
        }
    }
    
    void addBidiFdWatch(int fd, void *userdata, int flags)
    {
        // No implementation.
        throw std::system_error(std::make_error_code(std::errc::not_supported));
    }
    
    // flags specifies which watch to remove; ignored if the loop doesn't support
    // separate read/write watches.
    void removeFdWatch(int fd, int flags) noexcept
    {
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    }
    
    void removeFdWatch_nolock(int fd, int flags) noexcept
    {
        removeFdWatch(fd, flags);
    }
    
    void removeBidiFdWatch(int fd) noexcept
    {
        // Shouldn't be called for epoll.
        removeFdWatch(fd, IN_EVENTS | OUT_EVENTS);
    }
    
    // Note this will *replace* the old flags with the new, that is,
    // it can enable *or disable* read/write events.
    void enableFdWatch(int fd, void *userdata, int flags) noexcept
    {
        struct epoll_event epevent;
        // epevent.data.fd = fd;
        epevent.data.ptr = userdata;
        epevent.events = 0;
        
        if (flags & ONE_SHOT) {
            epevent.events = EPOLLONESHOT;
        }
        if (flags & IN_EVENTS) {
            epevent.events |= EPOLLIN;
        }
        if (flags & OUT_EVENTS) {
            epevent.events |= EPOLLOUT;
        }
        
        if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &epevent) == -1) {
            // Shouldn't be able to fail
            // throw new std::system_error(errno, std::system_category());
        }
    }
    
    void enableFdWatch_nolock(int fd, void *userdata, int flags)
    {
        enableFdWatch(fd, userdata, flags);
    }
    
    void disableFdWatch(int fd, int flags) noexcept
    {
        struct epoll_event epevent;
        // epevent.data.fd = fd;
        epevent.data.ptr = nullptr;
        epevent.events = 0;
        
        // Epoll documentation says that hangup will still be reported, need to check
        // whether this is really the case. Suspect it is really only the case if
        // EPOLLIN is set.
        if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &epevent) == -1) {
            // Let's assume that this can't fail.
            // throw new std::system_error(errno, std::system_category());
        }
    }
    
    void disableFdWatch_nolock(int fd, int flags) noexcept
    {
        disableFdWatch(fd, flags);
    }
    
    // Note signal should be masked before call.
    void addSignalWatch(int signo, void *userdata)
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);

        sigdataMap[signo] = userdata;

        // Modify the signal fd to watch the new signal
        bool was_no_sigfd = (sigfd == -1);
        sigaddset(&sigmask, signo);
        sigfd = signalfd(sigfd, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (sigfd == -1) {
            throw new std::system_error(errno, std::system_category());
        }
        
        if (was_no_sigfd) {
            // Add the signalfd to the epoll set.
            struct epoll_event epevent;
            epevent.data.ptr = &sigfd;
            epevent.events = EPOLLIN;
            // No need for EPOLLONESHOT - we can pull the signals out
            // as we see them.
            if (epoll_ctl(epfd, EPOLL_CTL_ADD, sigfd, &epevent) == -1) {
                close(sigfd);
                throw new std::system_error(errno, std::system_category());        
            }
        }
    }
    
    // Note, called with lock held:
    void rearmSignalWatch_nolock(int signo) noexcept
    {
        sigaddset(&sigmask, signo);
        signalfd(sigfd, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
    }
    
    void removeSignalWatch_nolock(int signo) noexcept
    {
        sigdelset(&sigmask, signo);
        signalfd(sigfd, &sigmask, 0);
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
        epoll_event events[16];
        int r = epoll_wait(epfd, events, 16, do_wait ? -1 : 0);
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
        epoll_event events[1];
        int r = epoll_wait(epfd, events, 1, do_wait ? -1 : 0);
        if (r == -1 || r == 0) {
            // signal or no events
            return;
        }
    
        processEvents(events, r);    
    }
};

} // end namespace
