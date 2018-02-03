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

template <class Base> class epoll_loop;

class epoll_traits
{
    template <class Base> friend class epoll_loop;

    public:

    class sigdata_t
    {
        template <class Base> friend class epoll_loop;
        
        struct signalfd_siginfo info;
        
        public:
        // mandatory:
        int get_signo() { return info.ssi_signo; }
        int get_sicode() { return info.ssi_code; }
        pid_t get_sipid() { return info.ssi_pid; }
        uid_t get_siuid() { return info.ssi_uid; }
        void * get_siaddr() { return reinterpret_cast<void *>(info.ssi_addr); }
        int get_sistatus() { return info.ssi_status; }
        int get_sival_int() { return info.ssi_int; }
        void * get_sival_ptr() { return reinterpret_cast<void *>(info.ssi_ptr); }

        // XSI
        int get_sierrno() { return info.ssi_errno; }

        // XSR (streams) OB (obselete)
        int get_siband() { return info.ssi_band; }

        // Linux:
        int32_t get_sifd() { return info.ssi_fd; }
        uint32_t get_sittimerid() { return info.ssi_tid; }
        uint32_t get_sioverrun() { return info.ssi_overrun; }
        uint32_t get_sitrapno() { return info.ssi_trapno; }
        uint32_t get_siutime() { return info.ssi_utime; }
        uint32_t get_sistime() { return info.ssi_stime; }
        // Field exposed by Linux kernel but not Glibc:
        // uint16_t get_siaddr_lsb() { return info.ssi_addr_lsb; }
        
        void set_signo(int signo) { info.ssi_signo = signo; }
    };    

    class fd_r;

    // File descriptor optional storage. If the mechanism can return the file descriptor, this
    // class will be empty, otherwise it can hold a file descriptor.
    class fd_s {
        friend class fd_r;
        
        // Epoll doesn't return the file descriptor (it can, but it can't return both file
        // descriptor and user data).
        int fd;

        public:
        fd_s(int fd_p) noexcept : fd(fd_p) { }
    };

    // File descriptor reference (passed to event callback). If the mechanism can return the
    // file descriptor, this class holds the file descriptor. Otherwise, the file descriptor
    // must be stored in an fd_s instance.
    class fd_r {
        public:
        int get_fd(fd_s ss)
        {
            return ss.fd;
        }
    };
    
    constexpr static bool has_bidi_fd_watch = true;
    constexpr static bool has_separate_rw_fd_watches = false;
    constexpr static bool interrupt_after_fd_add = false;
    constexpr static bool interrupt_after_signal_add = false;
    constexpr static bool supports_non_oneshot_fd = true;
};


template <class Base> class epoll_loop : public Base
{
    int epfd; // epoll fd
    int sigfd; // signalfd fd; -1 if not initialised
    sigset_t sigmask;

    std::unordered_map<int, void *> sigdataMap;

    // Base contains:
    //   lock - a lock that can be used to protect internal structure.
    //          receive*() methods will be called with lock held.
    //   receive_signal(sigdata_t &, user *) noexcept
    //   receive_fd_event(fd_r, user *, int flags) noexcept
    
    using sigdata_t = epoll_traits::sigdata_t;
    using fd_r = typename epoll_traits::fd_r;
    
    void process_events(epoll_event *events, int r)
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        
        for (int i = 0; i < r; i++) {
            void * ptr = events[i].data.ptr;
            
            if (ptr == &sigfd) {
                // Signal
                sigdata_t siginfo;
                while (true) {
                    int r = read(sigfd, &siginfo.info, sizeof(siginfo.info));
                    if (r == -1) break;
                    auto iter = sigdataMap.find(siginfo.get_signo());
                    if (iter != sigdataMap.end()) {
                        void *userdata = (*iter).second;
                        if (Base::receive_signal(*this, siginfo, userdata)) {
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
                auto r = Base::receive_fd_event(*this, fd_r(), ptr, flags);
                if (std::get<0>(r) != 0) {
                    enable_fd_watch_nolock(fd_r().get_fd(std::get<1>(r)), ptr, std::get<0>(r));
                }
            }            
        }
    }
    
    public:
    
    /**
     * epoll_loop constructor.
     *
     * Throws std::system_error or std::bad_alloc if the event loop cannot be initialised.
     */
    epoll_loop() : sigfd(-1)
    {
        epfd = epoll_create1(EPOLL_CLOEXEC);
        if (epfd == -1) {
            throw std::system_error(errno, std::system_category());
        }
        sigemptyset(&sigmask);
        Base::init(this);
    }
    
    ~epoll_loop()
    {
        close(epfd);
        if (sigfd != -1) {
            close(sigfd);
        }
    }
    
    //        fd:  file descriptor to watch
    //  userdata:  data to associate with descriptor
    //     flags:  IN_EVENTS | OUT_EVENTS | ONE_SHOT
    // soft_fail:  true if unsupported file descriptors should fail by returning false instead
    //             of throwing an exception
    // returns: true on success; false if file descriptor type isn't supported and soft_fail == true
    // throws:  std::system_error or std::bad_alloc on failure
    bool add_fd_watch(int fd, void *userdata, int flags, bool enabled = true, bool soft_fail = false)
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
            if (soft_fail && errno == EPERM) {
                return false;
            }
            throw new std::system_error(errno, std::system_category());        
        }
        return true;
    }
    
    bool add_bidi_fd_watch(int fd, void *userdata, int flags, bool emulate)
    {
        // No implementation.
        throw std::system_error(std::make_error_code(std::errc::not_supported));
    }
    
    // flags specifies which watch to remove; ignored if the loop doesn't support
    // separate read/write watches.
    void remove_fd_watch(int fd, int flags) noexcept
    {
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    }
    
    void remove_fd_watch_nolock(int fd, int flags) noexcept
    {
        remove_fd_watch(fd, flags);
    }
    
    void remove_bidi_fd_watch(int fd) noexcept
    {
        // Shouldn't be called for epoll.
        remove_fd_watch(fd, IN_EVENTS | OUT_EVENTS);
    }
    
    // Note this will *replace* the old flags with the new, that is,
    // it can enable *or disable* read/write events.
    void enable_fd_watch(int fd, void *userdata, int flags) noexcept
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
    
    void enable_fd_watch_nolock(int fd, void *userdata, int flags)
    {
        enable_fd_watch(fd, userdata, flags);
    }
    
    void disable_fd_watch(int fd, int flags) noexcept
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
    
    void disable_fd_watch_nolock(int fd, int flags) noexcept
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
                sigfd = -1;
                throw new std::system_error(errno, std::system_category());        
            }
        }
    }
    
    // Note, called with lock held:
    void rearm_signal_watch_nolock(int signo, void *userdata) noexcept
    {
        sigaddset(&sigmask, signo);
        signalfd(sigfd, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
    }
    
    void remove_signal_watch_nolock(int signo) noexcept
    {
        sigdelset(&sigmask, signo);
        signalfd(sigfd, &sigmask, 0);
    }

    void remove_signal_watch(int signo) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        remove_signal_watch_nolock(signo);
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
    void pull_events(bool do_wait)
    {
        epoll_event events[16];
        int r = epoll_wait(epfd, events, 16, do_wait ? -1 : 0);
        if (r == -1 || r == 0) {
            // signal or no events
            return;
        }
    
        do {
            process_events(events, r);
            r = epoll_wait(epfd, events, 16, 0);
        } while (r > 0);
    }
};

} // end namespace
