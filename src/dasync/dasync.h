#ifndef DASYNC_H_INCLUDED
#define DASYNC_H_INCLUDED

#if defined(__OpenBSD__)
#define HAVE_KQUEUE 1
#endif

#if defined(__linux__)
#define HAVE_EPOLL 1
#endif

#include "dasync-flags.h"

#if defined(HAVE_KQUEUE)
#include "dasync-kqueue.h"
#include "dasync-childproc.h"
namespace dasync {
    template <typename T> using Loop = KqueueLoop<T>;
    using LoopTraits = KqueueTraits;
}
#elif defined(HAVE_EPOLL)
#include "dasync-epoll.h"
#include "dasync-childproc.h"
namespace dasync {
    template <typename T> using Loop = EpollLoop<T>;
    using LoopTraits = EpollTraits;
}
#endif
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstddef>

#include "dmutex.h"



// TODO consider using atomic variables instead of explicit locking where appropriate

// Allow optimisation of empty classes by including this in the body:
// May be included as the last entry for a class which is only
// _potentially_ empty.

/*
#ifdef __GNUC__
#ifdef __clang__
#define EMPTY_BODY private: char empty_fill[0];
#else
#define EMPTY_BODY private: char empty_fill[0];
#endif
#else
#define EMPTY_BODY
#endif
*/

namespace dasync {


/**
 * Values for rearm/disarm return from event handlers
 */
enum class Rearm
{
    /** Re-arm the event watcher so that it receives further events */
    REARM,
    /** Disarm the event watcher so that it receives no further events, until it is re-armed explicitly */
    DISARM,
    /** Leave in current armed/disarmed state */
    NOOP,
    /** Remove the event watcher (and call "removed" callback) */
    REMOVE,
    /** The watcher has been removed - don't touch it! */
    REMOVED
// TODO: add a REQUEUE option, which means, "I didn't complete input/output, run me again soon"
};


// Forward declarations:
template <typename T_Mutex> class EventLoop;
template <typename T_Mutex> class PosixFdWatcher;
template <typename T_Mutex> class PosixBidiFdWatcher;
template <typename T_Mutex> class PosixSignalWatcher;
template <typename T_Mutex> class PosixChildWatcher;

// Information about a received signal.
// This is essentially a wrapper for the POSIX siginfo_t; its existence allows for mechanisms that receive
// equivalent signal information in a different format (eg signalfd on Linux).
using SigInfo = LoopTraits::SigInfo;

namespace dprivate {
    // (non-public API)

    enum class WatchType
    {
        SIGNAL,
        FD,
        CHILD,
        SECONDARYFD
    };
    
    template <typename T_Mutex, typename Traits> class EventDispatch;

    // For FD watchers:
    // Use this watch flag to indicate that in and out events should be reported separately,
    // that is, watcher should not be disabled until all watched event types are queued.
    constexpr static int multi_watch = 4;
    
    // Represents a queued event notification
    class BaseWatcher
    {
        template <typename T_Mutex, typename Traits> friend class EventDispatch;
        template <typename T_Mutex> friend class dasync::EventLoop;
        
        protected:
        WatchType watchType;
        int active : 1;
        int deleteme : 1;
        
        BaseWatcher * prev;
        BaseWatcher * next;
        
        public:
        
        // Perform initialisation necessary before registration with an event loop
        void init()
        {
            active = false;
            deleteme = false;
            prev = nullptr;
            next = nullptr;
        }
        
        BaseWatcher(WatchType wt) noexcept : watchType(wt) { }
        
        virtual ~BaseWatcher() noexcept { }
        
        // Called when the watcher has been removed.
        // It is guaranteed by the caller that:
        // - the dispatch method is not currently running
        // - the dispatch method will not be called.
        virtual void watchRemoved() noexcept
        {
            // TODO this "delete" behaviour could be dependent on a flag, perhaps?
            // delete this;
        }
    };
    
    // Base signal event - not part of public API
    template <typename T_Mutex>
    class BaseSignalWatcher : public BaseWatcher
    {
        template <typename M, typename Traits> friend class EventDispatch;
        friend class dasync::EventLoop<T_Mutex>;

        protected:
        SigInfo siginfo;
        BaseSignalWatcher() : BaseWatcher(WatchType::SIGNAL) { }

        public:
        typedef SigInfo &SigInfo_p;
        
        virtual Rearm gotSignal(EventLoop<T_Mutex> * eloop, int signo, SigInfo_p siginfo) = 0;
    };
    
    template <typename T_Mutex>
    class BaseFdWatcher : public BaseWatcher
    {
        template <typename, typename Traits> friend class EventDispatch;
        friend class dasync::EventLoop<T_Mutex>;
        
        protected:
        int watch_fd;
        int watch_flags;
        int event_flags;
        
        BaseFdWatcher() noexcept : BaseWatcher(WatchType::FD) { }
        
        public:
        virtual Rearm gotEvent(EventLoop<T_Mutex> * eloop, int fd, int flags) = 0;
    };
    
    template <typename T_Mutex>
    class BaseBidiFdWatcher : public BaseFdWatcher<T_Mutex>
    {
        template <typename, typename Traits> friend class EventDispatch;
        friend class dasync::EventLoop<T_Mutex>;
        
        // This should never actually get called:
        Rearm gotEvent(EventLoop<T_Mutex> * eloop, int fd, int flags) final
        {
            return Rearm::REARM; // should not be reachable.
        };
        
        protected:
        
        // The main instance is the "input" watcher only; we keep a secondary watcher
        // with a secondary set of flags for the "output" watcher:
        BaseWatcher outWatcher = BaseWatcher(WatchType::SECONDARYFD);
        
        int read_removed : 1; // read watch removed?
        int write_removed : 1; // write watch removed?
        
        public:
        virtual Rearm readReady(EventLoop<T_Mutex> * eloop, int fd) noexcept = 0;
        virtual Rearm writeReady(EventLoop<T_Mutex> * eloop, int fd) noexcept = 0;
    };
    
    template <typename T_Mutex>
    class BaseChildWatcher : public BaseWatcher
    {
        template <typename, typename Traits> friend class EventDispatch;
        friend class dasync::EventLoop<T_Mutex>;
        
        protected:
        pid_t watch_pid;
        int child_status;
        
        BaseChildWatcher() : BaseWatcher(WatchType::CHILD) { }
        
        public:
        virtual void gotTermStat(EventLoop<T_Mutex> * eloop, pid_t child, int status) = 0;
    };

    // Classes for implementing a fair(ish) wait queue.
    // A queue node can be signalled when it reaches the head of
    // the queue.

    template <typename T_Mutex> class waitqueue;
    template <typename T_Mutex> class waitqueue_node;

    // Select an appropriate conditiona variable type for a mutex:
    // condition_variable if mutex is std::mutex, or condition_variable_any
    // otherwise.
    template <class T_Mutex> class condvarSelector;

    template <> class condvarSelector<std::mutex>
    {
        public:
        typedef std::condition_variable condvar;
    };

    template <class T_Mutex> class condvarSelector
    {
        public:
        typedef std::condition_variable_any condvar;
    };

    template <> class waitqueue_node<NullMutex>
    {
        // Specialised waitqueue_node for NullMutex.
        // TODO can this be reduced to 0 data members?
        friend class waitqueue<NullMutex>;
        waitqueue_node * next = nullptr;
        
        public:
        void wait(std::unique_lock<NullMutex> &ul) { }
        void signal() { }
    };

    template <typename T_Mutex> class waitqueue_node
    {
        typename condvarSelector<T_Mutex>::condvar condvar;
        friend class waitqueue<T_Mutex>;
        waitqueue_node * next = nullptr;
        
        public:
        void signal()
        {
            condvar.notify_one();
        }
        
        void wait(std::unique_lock<T_Mutex> &mutex_lock)
        {
            condvar.wait(mutex_lock);
        }
    };

    template <typename T_Mutex> class waitqueue
    {
        waitqueue_node<T_Mutex> * tail = nullptr;
        waitqueue_node<T_Mutex> * head = nullptr;

        public:
        waitqueue_node<T_Mutex> * unqueue()
        {
            head = head->next;
            return head;
        }
        
        waitqueue_node<T_Mutex> * getHead()
        {
            return head;
        }
        
        void queue(waitqueue_node<T_Mutex> *node)
        {
            if (tail) {
                tail->next = node;
            }
            else {
                head = node;
            }
        }
    };

    // This class serves as the base class (mixin) for the AEN mechanism class.
    // Note that EventDispatch, here, and EventLoop (below) are really two sides of one coin;
    // they do not work independently. The mixin pattern that we use to avoid dynamic dispatch
    // forces them to be two seperate classes, however.
    //
    // The EventDispatch class maintains the queued event data structures. It inserts watchers
    // into the queue when eventes are received (receiveXXX methods).
    template <typename T_Mutex, typename Traits> class EventDispatch : public Traits
    {
        friend class EventLoop<T_Mutex>;

        // queue data structure/pointer
        BaseWatcher * first;
        
        using BaseSignalWatcher = dasync::dprivate::BaseSignalWatcher<T_Mutex>;
        using BaseFdWatcher = dasync::dprivate::BaseFdWatcher<T_Mutex>;
        using BaseBidiFdWatcher = dasync::dprivate::BaseBidiFdWatcher<T_Mutex>;
        using BaseChildWatcher = dasync::dprivate::BaseChildWatcher<T_Mutex>;
        
        void queueWatcher(BaseWatcher *bwatcher)
        {
            // Put in queue:
            if (first == nullptr) {
                bwatcher->prev = bwatcher;
                bwatcher->next = bwatcher;
                first = bwatcher;
            }
            else {
                first->prev->next = bwatcher;
                bwatcher->prev = first->prev;
                first->prev = bwatcher;
                bwatcher->next = first;
            }
        }
        
        bool isQueued(BaseWatcher *bwatcher)
        {
            return bwatcher->prev != nullptr;
        }

        void dequeueWatcher(BaseWatcher *bwatcher)
        {
            if (bwatcher->prev == bwatcher) {
                // Only item in queue
                first = nullptr;
            }
            else {
                if (first == bwatcher) first = first->next;
                bwatcher->prev->next = bwatcher->next;
                bwatcher->next->prev = bwatcher->prev;
            }
            
            bwatcher->prev = nullptr;
            bwatcher->next = nullptr;
        }
        
        protected:
        T_Mutex lock;
        
        void receiveSignal(typename Traits::SigInfo & siginfo, void * userdata)
        {
            BaseSignalWatcher * bwatcher = static_cast<BaseSignalWatcher *>(userdata);
            bwatcher->siginfo = siginfo;
            queueWatcher(bwatcher);
        }
        
        template <typename T>
        void receiveFdEvent(T &loop_mech, typename Traits::FD_r fd_r, void * userdata, int flags)
        {
            BaseFdWatcher * bfdw = static_cast<BaseFdWatcher *>(userdata);
            
            bfdw->event_flags |= flags;
            
            BaseWatcher * bwatcher = bfdw;
            
            bool is_multi_watch = bfdw->watch_flags & multi_watch;
            if (is_multi_watch) {                
                BaseBidiFdWatcher *bbdw = static_cast<BaseBidiFdWatcher *>(bwatcher);
                if (flags & in_events && flags & out_events) {
                    // Queue the secondary watcher first:
                    queueWatcher(&bbdw->outWatcher);
                }
                else if (flags & out_events) {                
                    // Use the secondary watcher for queueing:
                    bwatcher = &(bbdw->outWatcher);
                }
            }

            queueWatcher(bwatcher);
            
            if (! LoopTraits::has_separate_rw_fd_watches) {
                // If this is a bidirectional fd-watch, it has been disabled in *both* directions
                // as the event was delivered. However, the other direction should not be disabled
                // yet, so we need to re-enable:
                int in_out_mask = in_events | out_events;
                if (is_multi_watch && bfdw->event_flags != (bfdw->watch_flags & in_out_mask)) {
                    // We need to re-enable the other channel now:
                    loop_mech.enableFdWatch_nolock(bfdw->watch_fd, userdata,
                        (bfdw->watch_flags & ~(bfdw->event_flags)) | one_shot);
                }
            }
        }
        
        void receiveChildStat(pid_t child, int status, void * userdata)
        {
            BaseChildWatcher * watcher = static_cast<BaseChildWatcher *>(userdata);
            watcher->child_status = status;
            queueWatcher(watcher);
        }
        
        // Pull a single event from the queue
        BaseWatcher * pullEvent()
        {
            BaseWatcher * r = first;
            if (r != nullptr) {
                dequeueWatcher(r);
            }
            return r;
        }
        
        void issueDelete(BaseWatcher *watcher) noexcept
        {
            // This is only called when the attention lock is held, so if the watcher is not
            // active/queued now, it cannot become active (and will not be reported with an event)
            // during execution of this function.
            
            lock.lock();
            
            // TODO this needs to handle multi-watch (BidiFdWatcher) properly
            
            if (watcher->active) {
                // If the watcher is active, set deleteme true; the watcher will be removed
                // at the end of current processing (i.e. when active is set false).
                watcher->deleteme = true;
                lock.unlock();
            }
            else {
                // Actually do the delete.
                if (isQueued(watcher)) {
                    dequeueWatcher(watcher);
                }
                
                lock.unlock();
                watcher->watchRemoved();
            }
        }
        
        void issueDelete(BaseBidiFdWatcher *watcher) noexcept
        {
            lock.lock();
            
            if (watcher->active) {
                watcher->deleteme = true;
            }
            else {
                if (isQueued(watcher)) {
                    dequeueWatcher(watcher);
                }
                
                watcher->read_removed = true;
            }
            
            BaseWatcher *secondary = &(watcher->outWatcher);
            if (secondary->active) {
                secondary->deleteme = true;
            }
            else {
                if (isQueued(secondary)) {
                    dequeueWatcher(secondary);
                }
                
                watcher->write_removed = true;
            }
            
            if (watcher->read_removed && watcher->write_removed) {
                lock.unlock();
                watcher->watchRemoved();
            }
            else {
                lock.unlock();
            }
        }
    };
}


template <typename T_Mutex> class EventLoop
{
    friend class PosixFdWatcher<T_Mutex>;
    friend class PosixBidiFdWatcher<T_Mutex>;
    friend class PosixSignalWatcher<T_Mutex>;
    friend class PosixChildWatcher<T_Mutex>;
    
    template <typename T, typename U> using EventDispatch = dprivate::EventDispatch<T,U>;
    template <typename T> using waitqueue = dprivate::waitqueue<T>;
    template <typename T> using waitqueue_node = dprivate::waitqueue_node<T>;
    using BaseWatcher = dprivate::BaseWatcher;
    using BaseSignalWatcher = dprivate::BaseSignalWatcher<T_Mutex>;
    using BaseFdWatcher = dprivate::BaseFdWatcher<T_Mutex>;
    using BaseBidiFdWatcher = dprivate::BaseBidiFdWatcher<T_Mutex>;
    using BaseChildWatcher = dprivate::BaseChildWatcher<T_Mutex>;
    using WatchType = dprivate::WatchType;
    
    Loop<ChildProcEvents<EventDispatch<T_Mutex, LoopTraits>>> loop_mech;

    // There is a complex problem with most asynchronous event notification mechanisms
    // when used in a multi-threaded environment. Generally, a file descriptor or other
    // event type that we are watching will be associated with some data used to manage
    // that event source. For example a web server needs to maintain information about
    // each client connection, such as the state of the connection (what protocol version
    // has been negotiated, etc; if a transfer is taking place, what file is being
    // transferred etc).
    //
    // However, sometimes we want to remove an event source (eg webserver wants to drop
    // a connection) and delete the associated data. The problem here is that it is
    // difficult to be sure when it is ok to actually remove the data, since when
    // requesting to unwatch the source in one thread it is still possible that an
    // event from that source is just being reported to another thread (in which case
    // the data will be needed).
    //
    // To solve that, we:
    // - allow only one thread to poll for events at a time, using a lock
    // - use the same lock to prevent polling, if we want to unwatch an event source
    // - generate an event to interrupt any polling that may already be occurring in
    //   another thread
    // - mark handlers as active if they are currently executing, and
    // - when removing an active handler, simply set a flag which causes it to be
    //   removed once the current processing is finished, rather than removing it
    //   immediately.
    //
    // In particular the lock mechanism for preventing multiple threads polling and
    // for allowing polling to be interrupted is tricky. We can't use a simple mutex
    // since there is significant chance that it will be highly contended and there
    // are no guarantees that its acquisition will be fair. In particular, we don't
    // want a thread that is trying to unwatch a source being starved while another
    // thread polls the event source.
    //
    // So, we use two wait queues protected by a single mutex. The "attn_waitqueue"
    // (attention queue) is the high-priority queue, used for threads wanting to
    // unwatch event sources. The "wait_waitquueue" is the queue used by threads
    // that wish to actually poll for events.
    // - The head of the "attn_waitqueue" is always the holder of the lock
    // - Therefore, a poll-waiter must be moved from the wait_waitqueue to the
    //   attn_waitqueue to actually gain the lock. This is only done if the
    //   attn_waitqueue is otherwise empty.
    // - The mutex only protects manipulation of the wait queues, and so should not
    //   be highly contended.
    
    T_Mutex wait_lock;  // wait lock, used to prevent multiple threads from waiting
                        // on the event queue simultaneously.
    waitqueue<T_Mutex> attn_waitqueue;
    waitqueue<T_Mutex> wait_waitqueue;
    
    T_Mutex &getBaseLock()
    {
        return loop_mech.lock;
    }
    
    void registerSignal(BaseSignalWatcher *callBack, int signo)
    {
        loop_mech.addSignalWatch(signo, callBack);
    }
    
    void deregister(BaseSignalWatcher *callBack, int signo) noexcept
    {
        loop_mech.removeSignalWatch(signo);
        
        waitqueue_node<T_Mutex> qnode;
        getAttnLock(qnode);
        
        EventDispatch<T_Mutex, LoopTraits> & ed = (EventDispatch<T_Mutex, LoopTraits> &) loop_mech;
        ed.issueDelete(callBack);
        
        releaseLock(qnode);
    }

    void registerFd(BaseFdWatcher *callback, int fd, int eventmask)
    {
        loop_mech.addFdWatch(fd, callback, eventmask | one_shot);
    }
    
    void registerFd(BaseBidiFdWatcher *callback, int fd, int eventmask)
    {
        if (LoopTraits::has_separate_rw_fd_watches) {
            // TODO
        }
        else {
            loop_mech.addFdWatch(fd, callback, eventmask | one_shot);
        }
    }
    
    void setFdEnabled(BaseWatcher *watcher, int fd, int watch_flags, bool enabled)
    {
        if (enabled) {
            loop_mech.enableFdWatch(fd, watcher, watch_flags | one_shot);
        }
        else {
            loop_mech.disableFdWatch(fd, watch_flags);
        }
    }

    void setFdEnabled_nolock(BaseWatcher *watcher, int fd, int watch_flags, bool enabled)
    {
        if (enabled) {
            loop_mech.enableFdWatch_nolock(fd, watcher, watch_flags | one_shot);
        }
        else {
            loop_mech.disableFdWatch_nolock(fd, watch_flags);
        }
    }
    
    void deregister(BaseFdWatcher *callback, int fd)
    {
        loop_mech.removeFdWatch(fd, callback->watch_flags);
        
        waitqueue_node<T_Mutex> qnode;
        getAttnLock(qnode);
        
        EventDispatch<T_Mutex, LoopTraits> & ed = (EventDispatch<T_Mutex, LoopTraits> &) loop_mech;
        ed.issueDelete(callback);
        
        releaseLock(qnode);        
    }
    
    void deregister(BaseBidiFdWatcher *callback, int fd)
    {
        if (LoopTraits::has_separate_rw_fd_watches) {
            // TODO
        }
        else {
            loop_mech.removeFdWatch(fd, callback->watch_flags);
        }
        
        waitqueue_node<T_Mutex> qnode;
        getAttnLock(qnode);
        
        EventDispatch<T_Mutex, LoopTraits> & ed = (EventDispatch<T_Mutex, LoopTraits> &) loop_mech;
        ed.issueDelete(callback);
        
        releaseLock(qnode);
    }
    
    void reserveChildWatch(BaseChildWatcher *callBack)
    {
        loop_mech.reserveChildWatch();
    }
    
    void registerChild(BaseChildWatcher *callBack, pid_t child)
    {
        loop_mech.addChildWatch(child, callBack);
    }
    
    void registerReservedChild(BaseChildWatcher *callBack, pid_t child) noexcept
    {
        loop_mech.addReservedChildWatch(child, callBack);
    }
    
    void dequeueWatcher(BaseWatcher *watcher) noexcept
    {
        if (loop_mech.isQueued(watcher)) {
            loop_mech.dequeueWatcher(watcher);
        }
    }

    // Acquire the attention lock (when held, ensures that no thread is polling the AEN
    // mechanism).
    void getAttnLock(waitqueue_node<T_Mutex> &qnode)
    {
        std::unique_lock<T_Mutex> ulock(wait_lock);
        attn_waitqueue.queue(&qnode);        
        if (attn_waitqueue.getHead() != &qnode) {
            loop_mech.interruptWait();
            while (attn_waitqueue.getHead() != &qnode) {
                qnode.wait(ulock);
            }
        }
    }
    
    // Acquire the poll-wait lock (to be held when polling the AEN mechanism; lower
    // priority than the attention lock).
    void getPollwaitLock(waitqueue_node<T_Mutex> &qnode)
    {
        std::unique_lock<T_Mutex> ulock(wait_lock);
        if (attn_waitqueue.getHead() == nullptr) {
            // Queue is completely empty:
            attn_waitqueue.queue(&qnode);
        }
        else {
            wait_waitqueue.queue(&qnode);
        }
        
        while (attn_waitqueue.getHead() != &qnode) {
            qnode.wait(ulock);
        }    
    }
    
    // Release the poll-wait/attention lock.
    void releaseLock(waitqueue_node<T_Mutex> &qnode)
    {
        std::unique_lock<T_Mutex> ulock(wait_lock);
        waitqueue_node<T_Mutex> * nhead = attn_waitqueue.unqueue();
        if (nhead != nullptr) {
            nhead->signal();
        }
        else {
            nhead = wait_waitqueue.getHead();
            if (nhead != nullptr) {
                attn_waitqueue.queue(nhead);
                nhead->signal();
            }
        }                
    }
    
    void processSignalRearm(BaseSignalWatcher * bsw, Rearm rearmType)
    {
        // Called with lock held
        if (rearmType == Rearm::REARM) {
            loop_mech.rearmSignalWatch_nolock(bsw->siginfo.get_signo());
        }
        else if (rearmType == Rearm::REMOVE) {
            loop_mech.removeSignalWatch_nolock(bsw->siginfo.get_signo());
        }
    }

    Rearm processFdRearm(BaseFdWatcher * bfw, Rearm rearmType, bool is_multi_watch)
    {
        // Called with lock held
        if (is_multi_watch) {
            BaseBidiFdWatcher * bdfw = static_cast<BaseBidiFdWatcher *>(bfw);
            
            if (rearmType == Rearm::REMOVE) {
                bdfw->read_removed = 1;
                bdfw->watch_flags &= ~in_events;
                
                if (! LoopTraits::has_separate_rw_fd_watches) {
                    if (! bdfw->write_removed) {
                        return Rearm::NOOP;
                    }
                    else {
                        // both removed: actually remove
                        loop_mech.removeFdWatch_nolock(bdfw->watch_fd, 0 /* not used */);
                        return Rearm::REMOVE;
                    }
                }
                else {
                    // TODO this will need flags for such a loop, since it can't
                    // otherwise distinguish which channel watch to remove
                    loop_mech.removeFdWatch_nolock(bdfw->watch_fd, bdfw->watch_flags);
                }
            }
            else if (rearmType == Rearm::DISARM) {
                // Nothing more to do
            }
            else if (rearmType == Rearm::REARM) {
                bdfw->watch_flags |= in_events;
                if (! LoopTraits::has_separate_rw_fd_watches) {
                    loop_mech.enableFdWatch_nolock(bdfw->watch_fd,
                            static_cast<BaseWatcher *>(bdfw),
                            (bdfw->watch_flags & (in_events | out_events)) | one_shot);
                }
                else {
                    loop_mech.enableFdWatch_nolock(bdfw->watch_fd,
                            static_cast<BaseWatcher *>(bdfw),
                            in_events | one_shot);
                }
            }
            return rearmType;
        }
        else {
            if (rearmType == Rearm::REARM) {
                loop_mech.enableFdWatch_nolock(bfw->watch_fd, bfw,
                        (bfw->watch_flags & (in_events | out_events)) | one_shot);
            }
            else if (rearmType == Rearm::REMOVE) {
                loop_mech.removeFdWatch_nolock(bfw->watch_fd, bfw->watch_flags);
            }
            return rearmType;
        }
    }

    Rearm processSecondaryRearm(BaseBidiFdWatcher * bdfw, Rearm rearmType)
    {
        // Called with lock held
        if (rearmType == Rearm::REMOVE) {
            bdfw->write_removed = 1;
            bdfw->watch_flags &= ~out_events;
            
            if (LoopTraits::has_separate_rw_fd_watches) {
                // TODO this will need flags for such a loop, since it can't
                // otherwise distinguish which channel watch to remove
                loop_mech.removeFdWatch_nolock(bdfw->watch_fd, bdfw->watch_flags);
                return bdfw->read_removed ? Rearm::REMOVE : Rearm::NOOP;
            }
            else {
                if (! bdfw->read_removed) {
                    return Rearm::NOOP;
                }
                else {
                    // both removed: actually remove
                    loop_mech.removeFdWatch_nolock(bdfw->watch_fd, 0 /* not used */);
                    return Rearm::REMOVE;
                }
            }
        }
        else if (rearmType == Rearm::DISARM) {
            // Nothing more to do
        }
        else if (rearmType == Rearm::REARM) {
            bdfw->watch_flags |= out_events;
            if (! LoopTraits::has_separate_rw_fd_watches) {
                loop_mech.enableFdWatch_nolock(bdfw->watch_fd,
                        static_cast<BaseWatcher *>(bdfw),
                        (bdfw->watch_flags & (in_events | out_events)) | one_shot);
            }
            else {
                loop_mech.enableFdWatch_nolock(bdfw->watch_fd,
                        static_cast<BaseWatcher *>(bdfw),
                        out_events | one_shot);
            }
        }
        return rearmType;
    }

    bool processEvents() noexcept
    {
        EventDispatch<T_Mutex, LoopTraits> & ed = (EventDispatch<T_Mutex, LoopTraits> &) loop_mech;
        ed.lock.lock();
        
        // So this pulls *all* currently pending events and processes them in the current thread.
        // That's probably good for throughput, but maybe the behavior should be configurable.
        
        BaseWatcher * pqueue = ed.pullEvent();
        bool active = false;
        
        while (pqueue != nullptr) {
        
            pqueue->active = true;
            active = true;
            
            Rearm rearmType = Rearm::NOOP;
            bool is_multi_watch = false;
            BaseBidiFdWatcher *bbfw = nullptr;
            
            // (Above variables are initialised only to silence compiler warnings).
            
            // Read/manipulate watch_flags (if necessary) *before* we release the lock:
            if (pqueue->watchType == WatchType::FD) {
                BaseFdWatcher *bfw = static_cast<BaseFdWatcher *>(pqueue);
                bbfw = static_cast<BaseBidiFdWatcher *>(bfw);
                is_multi_watch = bfw->watch_flags & dprivate::multi_watch;
                if (! LoopTraits::has_separate_rw_fd_watches && is_multi_watch) {
                    // Clear the input watch flags to avoid enabling read watcher while active:
                    bfw->watch_flags &= ~in_events;
                }
            }
            else if (pqueue->watchType == WatchType::SECONDARYFD) {
                is_multi_watch = true;
                char * rp = (char *)pqueue;
                rp -= offsetof(BaseBidiFdWatcher, outWatcher);
                bbfw = (BaseBidiFdWatcher *)rp;
                if (! LoopTraits::has_separate_rw_fd_watches) {
                    bbfw->watch_flags &= ~out_events;
                }
            }
            
            ed.lock.unlock();
            
            // Note that we select actions based on the type of the watch, as determined by the watchType
            // member. In some ways this screams out for polmorphism; a virtual function could be overridden
            // by each of the watcher types. I've instead used switch/case because I think it will perform
            // slightly better without the overhead of a virtual function dispatch, but it's got to be a
            // close call; I might be guilty of premature optimisation here.
            
            switch (pqueue->watchType) {
            case WatchType::SIGNAL: {
                BaseSignalWatcher *bsw = static_cast<BaseSignalWatcher *>(pqueue);
                rearmType = bsw->gotSignal(this, bsw->siginfo.get_signo(), bsw->siginfo);
                break;
            }
            case WatchType::FD: {
                BaseFdWatcher *bfw = static_cast<BaseFdWatcher *>(pqueue);
                if (is_multi_watch) {
                    // The primary watcher for a multi-watch watcher is queued for
                    // read events.
                    rearmType = bbfw->readReady(this, bfw->watch_fd);
                }
                else {
                    rearmType = bfw->gotEvent(this, bfw->watch_fd, bfw->event_flags);
                }
                break;
            }
            case WatchType::CHILD: {
                BaseChildWatcher *bcw = static_cast<BaseChildWatcher *>(pqueue);
                bcw->gotTermStat(this, bcw->watch_pid, bcw->child_status);
                // Child watches automatically remove:
                // TODO what if they want to return REMOVED...
                rearmType = Rearm::REMOVE;
                break;
            }
            case WatchType::SECONDARYFD: {
                // first construct a pointer to the main watcher:
                rearmType = bbfw->writeReady(this, bbfw->watch_fd);
                break;
            }
            default: ;
            }

            ed.lock.lock();
            
            // (if REMOVED, we must not touch pqueue at all)
            if (rearmType != Rearm::REMOVED) {
                
                pqueue->active = false;
                if (pqueue->deleteme) {
                    // We don't want a watch that is marked "deleteme" to re-arm itself.
                    rearmType = Rearm::REMOVE;
                }
                switch (pqueue->watchType) {
                case WatchType::SIGNAL:
                    processSignalRearm(static_cast<BaseSignalWatcher *>(pqueue), rearmType);
                    break;
                case WatchType::FD:
                    rearmType = processFdRearm(static_cast<BaseFdWatcher *>(pqueue), rearmType, is_multi_watch);
                    break;
                case WatchType::SECONDARYFD:
                    rearmType = processSecondaryRearm(bbfw, rearmType);
                    break;
                default: ;
                }
                
                if (pqueue->deleteme || rearmType == Rearm::REMOVE) {
                    ed.lock.unlock();
                    (is_multi_watch ? bbfw : pqueue)->watchRemoved();
                    ed.lock.lock();
                }
            }
            
            pqueue = ed.pullEvent();
        }
        
        ed.lock.unlock();
        return active;
    }

    
    public:
    void run() noexcept
    {
        while (! processEvents()) {
            waitqueue_node<T_Mutex> qnode;
            
            // We only allow one thread to poll the mechanism at any time, since otherwise
            // removing event watchers is a nightmare beyond comprehension.
            getPollwaitLock(qnode);
            
            // Pull events from the AEN mechanism and insert them in our internal queue:
            loop_mech.pullEvents(true);
            
            // Now release the wait lock:
            releaseLock(qnode);
        }
    }
};


typedef EventLoop<NullMutex> NEventLoop;
typedef EventLoop<std::mutex> TEventLoop;

// from dasync.cc:
TEventLoop & getSystemLoop();

// Posix signal event watcher
template <typename T_Mutex>
class PosixSignalWatcher : private dprivate::BaseSignalWatcher<T_Mutex>
{
    using BaseWatcher = dprivate::BaseWatcher;
    
public:
    using SigInfo_p = typename dprivate::BaseSignalWatcher<T_Mutex>::SigInfo_p;

    // Register this watcher to watch the specified signal.
    // If an attempt is made to register with more than one event loop at
    // a time, behaviour is undefined.
    inline void registerWatch(EventLoop<T_Mutex> *eloop, int signo)
    {
        BaseWatcher::init();
        this->siginfo.set_signo(signo);
        eloop->registerSignal(this, signo);
    }
    
    inline void deregisterWatch(EventLoop<T_Mutex> *eloop) noexcept
    {
        eloop->deregister(this, this->siginfo.get_signo());
    }
    
    // virtual Rearm gotSignal(EventLoop<T_Mutex> *, int signo, SigInfo_p info) = 0;
};

// Posix file descriptor event watcher
template <typename T_Mutex>
class PosixFdWatcher : private dprivate::BaseFdWatcher<T_Mutex>
{
    using BaseWatcher = dprivate::BaseWatcher;

    protected:
    
    // Set the types of event to watch. Only supported if LoopTraits::has_bidi_fd_watch
    // is true; otherwise has unspecified behavior.
    // Only safe to call from within the callback handler (gotEvent). Might not take
    // effect until the current callback handler returns with REARM.
    void setWatchFlags(int newFlags)
    {
        this->watch_flags = newFlags;
    }
    
    public:
    
    // Register a file descriptor watcher with an event loop. Flags
    // can be any combination of dasync::in_events / dasync::out_events.
    // Exactly one of in_events/out_events must be specified if the event
    // loop does not support bi-directional fd watchers.
    //
    // Mechanisms supporting dual watchers allow for two watchers for a
    // single file descriptor (one watching read status and the other
    // write status). Others mechanisms support only a single watcher
    // per file descriptor. Adding a watcher beyond what is supported
    // causes undefined behavior.
    //
    // Can fail with std::bad_alloc or std::system_error.
    void registerWith(EventLoop<T_Mutex> *eloop, int fd, int flags)
    {
        BaseWatcher::init();
        this->watch_fd = fd;
        this->watch_flags = flags;
        eloop->registerFd(this, fd, flags);
    }
    
    // Deregister a file descriptor watcher.
    //
    // If other threads may be polling the event loop, it is not safe to assume
    // the watcher is unregistered until the watchRemoved() callback is issued
    // (which will not occur until the event handler returns, if it is active).
    // In a single threaded environment, it is safe to delete the watcher after
    // calling this method as long as the handler (if it is active) accesses no
    // internal state and returns Rearm::REMOVED.
    //   TODO: implement REMOVED, or correct above statement.
    void deregisterWatch(EventLoop<T_Mutex> *eloop) noexcept
    {
        eloop->deregister(this, this->watch_fd);
    }
    
    void setEnabled(EventLoop<T_Mutex> *eloop, bool enable) noexcept
    {
        std::lock_guard<T_Mutex> guard(eloop->getBaseLock());
        eloop->setFdEnabled_nolock(this, this->watch_fd, this->watch_flags, enable);
        if (! enable) {
            eloop->dequeueWatcher(this);
        }
    }
    
    // virtual Rearm gotEvent(EventLoop<T_Mutex> *, int fd, int flags) = 0;
};

// A Bi-directional file descriptor watcher with independent read- and write- channels.
// This watcher type has two event notification methods which can both potentially be
// active at the same time.
template <typename T_Mutex>
class PosixBidiFdWatcher : private dprivate::BaseBidiFdWatcher<T_Mutex>
{
    using BaseWatcher = dprivate::BaseWatcher;
    
    void setWatchEnabled(EventLoop<T_Mutex> *eloop, bool in, bool b)
    {
        int events = in ? in_events : out_events;
        
        if (b) {
            this->watch_flags |= events;
        }
        else {
            this->watch_flags &= ~events;
        }
        if (LoopTraits::has_separate_rw_fd_watches) {
            dprivate::BaseWatcher * watcher = in ? this : &this->outWatcher;
            eloop->setFdEnabled_nolock(watcher, this->watch_fd, events | one_shot, b);
            if (! b) {
                eloop->dequeueWatcher(watcher);
            }
        }
        else {
            eloop->setFdEnabled_nolock(this, this->watch_fd,
                    (this->watch_flags & (in_events | out_events)) | one_shot,
                    (this->watch_flags & (in_events | out_events)) != 0);
            if (! b) {
                dprivate::BaseWatcher * watcher = in ? this : &this->outWatcher;
                eloop->dequeueWatcher(watcher);
            }
        }
    }
    
    protected:
    
    // TODO if a watch is disabled and currently queued, we should de-queue it.
    
    void setInWatchEnabled(EventLoop<T_Mutex> *eloop, bool b) noexcept
    {
        eloop->getBaseLock().lock();
        setWatchEnabled(eloop, true, b);
        eloop->getBaseLock().unlock();
    }
    
    void setOutWatchEnabled(EventLoop<T_Mutex> *eloop, bool b) noexcept
    {
        eloop->getBaseLock().lock();
        setWatchEnabled(eloop, false, b);
        eloop->getBaseLock().unlock();
    }
    
    // Set the watch flags, which enables/disables both the in-watch and the out-watch accordingly.
    //
    // Concurrency: this method can only be called if
    //  - it does not enable a watcher that might currently be active
    ///   - unless the event loop will not be polled while the watcher is active.
    // (i.e. it is ok to call setWatchFlags from within the readReady/writeReady handlers if no other
    //  thread will poll the event loop; it is ok to *dis*able a watcher that might be active).
    void setWatchFlags(EventLoop<T_Mutex> * eloop, int newFlags)
    {
        std::lock_guard<T_Mutex> guard(eloop->getBaseLock());
        if (LoopTraits::has_separate_rw_fd_watches) {
            setWatchEnabled(eloop, true, (newFlags & in_events) != 0);
            setWatchEnabled(eloop, false, (newFlags & out_events) != 0);
        }
        else {
            this->watch_flags = (this->watch_flags & ~IO_EVENTS) | newFlags;
            eloop->setFdEnabled((dprivate::BaseWatcher *) this, this->watch_fd, this->watch_flags & IO_EVENTS, true);
        }
    }
    
    public:
    
    // Register a bi-direction file descriptor watcher with an event loop. Flags
    // can be any combination of dasync::in_events / dasync::out_events.
    //
    // Can fail with std::bad_alloc or std::system_error.
    void registerWith(EventLoop<T_Mutex> *eloop, int fd, int flags)
    {
        BaseWatcher::init();
        this->outWatcher.BaseWatcher::init();
        this->watch_fd = fd;
        this->watch_flags = flags | dprivate::multi_watch;
        eloop->registerFd(this, fd, flags);
    }
    
    // Deregister a bi-direction file descriptor watcher.
    //
    // If other threads may be polling the event loop, it is not safe to assume
    // the watcher is unregistered until the watchRemoved() callback is issued
    // (which will not occur until the event handler returns, if it is active).
    // In a single threaded environment, it is safe to delete the watcher after
    // calling this method as long as the handler (if it is active) accesses no
    // internal state and returns Rearm::REMOVED.
    //   TODO: implement REMOVED, or correct above statement.
    void deregisterWatch(EventLoop<T_Mutex> *eloop) noexcept
    {
        eloop->deregister(this, this->watch_fd);
    }
    
    // Rearm readReady(EventLoop<T_Mutex> * eloop, int fd) noexcept
    // Rearm writeReady(EventLoop<T_Mutex> * eloop, int fd) noexcept
};

// Posix child process event watcher
template <typename T_Mutex>
class PosixChildWatcher : private dprivate::BaseChildWatcher<T_Mutex>
{
    using BaseWatcher = dprivate::BaseWatcher;

    public:
    // Reserve resources for a child watcher with the given event loop.
    // Reservation can fail with std::bad_alloc.
    void reserveWith(EventLoop<T_Mutex> *eloop)
    {
        eloop->reserveChildWatch();
    }
    
    // Register a watcher for the given child process with an event loop.
    // Registration can fail with std::bad_alloc.
    void registerWith(EventLoop<T_Mutex> *eloop, pid_t child)
    {
        BaseWatcher::init();
        this->watch_pid = child;
        eloop->registerChild(this, child);
    }
    
    // Register a watcher for the given child process with an event loop,
    // after having reserved resources previously (using reserveWith).
    // Registration cannot fail.
    void registerReserved(EventLoop<T_Mutex> *eloop, pid_t child) noexcept
    {
        BaseWatcher::init();
        eloop->registerReservedChild(this, child);
    }
    
    // virtual void gotTermStat(EventLoop<T_Mutex> *, pid_t child, int status) = 0;
};

}  // namespace dasync

#endif
