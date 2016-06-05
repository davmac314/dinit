#include <system_error>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <unistd.h>
#include <signal.h>

namespace dasync {

// Event type bits
constexpr unsigned int in_events = 1;
constexpr unsigned int out_events = 2;
constexpr unsigned int err_events = 4;

constexpr unsigned int one_shot = 8;


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
    };
};


template <class Base> class KqueueLoop : public Base
{
    int kqfd; // epoll fd

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
            if (events[i].filter = EVFILT_SIGNAL) {
                siginfo_t siginfo;
                sigset_t sset;
                sigemptyset(&sset);
                sigaddset(&sset, events[i].ident);
                //int r = sigwaitinfo(&sset, &siginfo);
                // OpenBSD doesn't have sigwaitinfo...
                int r = 0;
                if (r > 0) {
                    Base::receiveSignal(siginfo, (void *)events[i].udata);
                }
            }
            
            
            //else {
            //    int flags = 0;
            //    (events[i].events & EPOLLIN) && (flags |= in_events);
            //    (events[i].events & EPOLLHUP) && (flags |= in_events);
            //    (events[i].events & EPOLLOUT) && (flags |= out_events);
            //    (events[i].events & EPOLLERR) && (flags |= err_events);
            //    Base::receiveFdEvent(*this, FD_r(), ptr, flags);
            //}            
        }
    }
    
    public:
    
    /**
     * KqueueLoop constructor.
     *
     * Throws std::system_error or std::bad_alloc if the event loop cannot be initialised.
     */
    KqueueLoop()
    {
        //epfd = epoll_create1(EPOLL_CLOEXEC);
        //if (epfd == -1) {
        //    throw std::system_error(errno, std::system_category());
        //}
        //sigemptyset(&sigmask);
    }
    
    ~KqueueLoop()
    {
        //close(epfd);
        //if (sigfd != -1) {
        //    close(sigfd);
        //}
    }
    
    // flags:  in_events | out_events
    void addFdWatch(int fd, void *userdata, int flags)
    {
        //struct epoll_event epevent;
        // epevent.data.fd = fd;
        //epevent.data.ptr = userdata;
        //epevent.events = 0;
        
        //if (flags & one_shot) {
        //    epevent.events = EPOLLONESHOT;
        //}
        //if (flags & in_events) {
        //    epevent.events |= EPOLLIN;
        //}
        //if (flags & out_events) {
        //    epevent.events |= EPOLLOUT;
        //}

        //if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &epevent) == -1) {
        //    throw new std::system_error(errno, std::system_category());        
        //}
    }
    
    void removeFdWatch(int fd)
    {
        //epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    }
    
    void removeFdWatch_nolock(int fd)
    {
        removeFdWatch(fd);
    }
    
    // Note this will *replace* the old flags with the new, that is,
    // it can enable *or disable* read/write events.
    void enableFdWatch(int fd, void *userdata, int flags)
    {
        //struct epoll_event epevent;
        // epevent.data.fd = fd;
        //epevent.data.ptr = userdata;
        //epevent.events = 0;
        
        //if (flags & one_shot) {
        //    epevent.events = EPOLLONESHOT;
        //}
        //if (flags & in_events) {
        //    epevent.events |= EPOLLIN;
        //}
        //if (flags & out_events) {
        //    epevent.events |= EPOLLOUT;
        //}

        //if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &epevent) == -1) {
        //    throw new std::system_error(errno, std::system_category());        
        //}
    }
    
    void enableFdWatch_nolock(int fd, void *userdata, int flags)
    {
        enableFdWatch(fd, userdata, flags);
    }
    
    void disableFdWatch(int fd)
    {
        //struct epoll_event epevent;
        // epevent.data.fd = fd;
        //epevent.data.ptr = nullptr;
        //epevent.events = 0;
        
        // Epoll documentation says that hangup will still be reported, need to check
        // whether this is really the case. Suspect it is really only the case if
        // EPOLLIN is set.
        //if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &epevent) == -1) {
        //    throw new std::system_error(errno, std::system_category());        
        //}
    }
    
    // Note signal should be masked before call.
    void addSignalWatch(int signo, void *userdata)
    {
        //std::lock_guard<decltype(Base::lock)> guard(Base::lock);

        //sigdataMap[signo] = userdata;

        // Modify the signal fd to watch the new signal
        //bool was_no_sigfd = (sigfd == -1);
        //sigaddset(&sigmask, signo);
        //sigfd = signalfd(sigfd, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
        //if (sigfd == -1) {
        //    throw new std::system_error(errno, std::system_category());
        //}
        
        //if (was_no_sigfd) {
            // Add the signalfd to the epoll set.
        //    struct epoll_event epevent;
        //    epevent.data.ptr = &sigfd;
        //    epevent.events = EPOLLIN;
            // No need for EPOLLONESHOT - we can pull the signals out
            // as we see them.
        //    if (epoll_ctl(epfd, EPOLL_CTL_ADD, sigfd, &epevent) == -1) {
        //        close(sigfd);
        //        throw new std::system_error(errno, std::system_category());        
        //    }
        //}
    }
    
    // Note, called with lock held:
    void rearmSignalWatch_nolock(int signo) noexcept
    {
        //sigaddset(&sigmask, signo);
        //signalfd(sigfd, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
    }
    
    void removeSignalWatch_nolock(int signo) noexcept
    {
        //sigdelset(&sigmask, signo);
        //signalfd(sigfd, &sigmask, 0);
    }

    void removeSignalWatch(int signo) noexcept
    {
        //std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        //removeSignalWatch_nolock(signo);
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
        //epoll_event events[16];
        //int r = epoll_wait(epfd, events, 16, do_wait ? -1 : 0);
        //if (r == -1 || r == 0) {
            // signal or no events
        //    return;
        //}
    
        //processEvents(events, r);
    }

    // If events are pending, process one of them.
    // If no events are pending, wait until one event is received and
    // process this event.
    //
    //  do_wait - if false, returns immediately if no events are
    //            pending.    
    void pullOneEvent(bool do_wait)
    {
        //epoll_event events[1];
        //int r = epoll_wait(epfd, events, 1, do_wait ? -1 : 0);
        //if (r == -1 || r == 0) {
            // signal or no events
        //    return;
        //}
    
        //processEvents(events, r);    
    }
    
    // Interrupt any current poll operation (pullEvents/pullOneEvent), causing
    // it to to return immediately.
    void interruptWait()
    {
        // TODO
    }
};

// Map of pid_t to void *, with possibility of reserving entries so that mappings can
// be later added with no danger of allocator exhaustion (bad_alloc).
class pid_map
{
    using pair = std::pair<pid_t, void *>;
    std::unordered_map<pid_t, void *> base_map;
    std::vector<pair> backup_vector;
    
    // Number of entries in backup_vector that are actually in use (as opposed
    // to simply reserved):
    int backup_size = 0;
    
    public:
    using entry = std::pair<bool, void *>;

    entry get(pid_t key) noexcept
    {
        auto it = base_map.find(key);
        if (it == base_map.end()) {
            // Not in map; look in vector
            for (int i = 0; i < backup_size; i++) {
                if (backup_vector[i].first == key) {
                    return entry(true, backup_vector[i].second);
                }
            }
        
            return entry(false, nullptr);
        }
        
        return entry(true, it->second);
    }
    
    entry erase(pid_t key) noexcept
    {
        auto iter = base_map.find(key);
        if (iter != base_map.end()) {
            entry r(true, iter->second);
            base_map.erase(iter);
            return r;
        }
        for (int i = 0; i < backup_size; i++) {
            if (backup_vector[i].first == key) {
                entry r(true, backup_vector[i].second);
                backup_vector.erase(backup_vector.begin() + i);
                return r;
            }
        }
        return entry(false, nullptr);
    }
    
    // Throws bad_alloc on reservation failure
    void reserve()
    {
        backup_vector.resize(backup_vector.size() + 1);
    }
    
    void add(pid_t key, void *val) // throws std::bad_alloc
    {
        base_map[key] = val;
    }
    
    void add_from_reserve(pid_t key, void *val) noexcept
    {
        try {
            base_map[key] = val;
            backup_vector.resize(backup_vector.size() - 1);
        }
        catch (std::bad_alloc &) {
            // We couldn't add into the map, use the reserve:
            backup_vector[backup_size++] = pair(key, val);
        }
    }
};

template <class Base> class ChildProcEvents : public Base
{
    private:
    pid_map child_waiters;

    using SigInfo = typename Base::SigInfo;
    
    protected:
    void receiveSignal(SigInfo &siginfo, void *userdata)
    {
        if (siginfo.get_signo() == SIGCHLD) {
            int status;
            pid_t child;
            while ((child = waitpid(-1, &status, WNOHANG)) > 0) {
                pid_map::entry ent = child_waiters.erase(child);
                if (ent.first) {
                    Base::receiveChildStat(child, status, ent.second);
                }
            }
        }
        else {
            Base::receiveSignal(siginfo, userdata);
        }
    }
    
    public:
    void reserveChildWatch()
    {
        child_waiters.reserve();
    }
    
    void addChildWatch(pid_t child, void *val)
    {
        child_waiters.add(child, val);
    }
    
    void addReservedChildWatch(pid_t child, void *val) noexcept
    {
        child_waiters.add_from_reserve(child, val);
    }
};

} // end namespace
