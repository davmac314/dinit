#ifndef DASYNQ_H_INCLUDED
#define DASYNQ_H_INCLUDED

#include "dasynq-config.h"

#include "dasynq-flags.h"
#include "dasynq-binaryheap.h"
#include "dasynq-interrupt.h"

#if defined(DASYNQ_CUSTOM_LOOP_IMPLEMENTATION)
// Loop and LoopTraits defined already; used for testing
#elif defined(DASYNQ_HAVE_KQUEUE)
#include "dasynq-kqueue.h"
#include "dasynq-itimer.h"
#include "dasynq-childproc.h"
namespace dasynq {
    template <typename T> using Loop = KqueueLoop<interrupt_channel<ITimerEvents<ChildProcEvents<T>>>>;
    using LoopTraits = KqueueTraits;
}
#elif defined(DASYNQ_HAVE_EPOLL)
#include "dasynq-epoll.h"
#include "dasynq-timerfd.h"
#include "dasynq-childproc.h"
namespace dasynq {
    template <typename T> using Loop = EpollLoop<interrupt_channel<TimerFdEvents<ChildProcEvents<T>>>>;
    using LoopTraits = EpollTraits;
}
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <system_error>

#include <unistd.h>
#include <fcntl.h>

#include "dasynq-mutex.h"

namespace dasynq {

#ifdef __APPLE__
int pipe2(int filedes[2], int flags)
{
    if (pipe(filedes) == -1) {
        return -1;
    }

    if (flags & O_CLOEXEC) {
	fcntl(filedes[0], F_SETFD, FD_CLOEXEC);
	fcntl(filedes[1], F_SETFD, FD_CLOEXEC);
    }

    if (flags & O_NONBLOCK) {
	fcntl(filedes[0], F_SETFL, O_NONBLOCK);
	fcntl(filedes[1], F_SETFL, O_NONBLOCK);
    }

    return 0;
}
#endif

namespace dprivate {
    class BaseWatcher;
}

using PrioQueue = BinaryHeap<dprivate::BaseWatcher *, int>;

inline namespace {
    constexpr int DEFAULT_PRIORITY = 50;
}

/**
 * Values for rearm/disarm return from event handlers
 */
enum class rearm
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
template <typename T_Mutex, template <typename> class Loop = dasynq::Loop, typename LoopTraits = dasynq::LoopTraits>
class event_loop;

namespace dprivate {
    // (non-public API)
    
    template <typename T_Loop> class fd_watcher;
    template <typename T_Loop> class bidi_fd_watcher;
    template <typename T_Loop> class signal_watcher;
    template <typename T_Loop> class child_proc_watcher;
    template <typename T_Loop> class timer;
    
    enum class WatchType
    {
        SIGNAL,
        FD,
        CHILD,
        SECONDARYFD,
        TIMER
    };
    
    template <typename T_Mutex, typename Traits> class EventDispatch;

    // For FD watchers:
    // Use this watch flag to indicate that in and out events should be reported separately,
    // that is, watcher should not be disabled until all watched event types are queued.
    constexpr static int multi_watch = 4;
    
    // Represents a queued event notification. Various event watchers derive from this type.
    class BaseWatcher
    {
        template <typename T_Mutex, typename Traits> friend class EventDispatch;
        template <typename T_Mutex, template <typename> class, typename> friend class dasynq::event_loop;
        
        protected:
        WatchType watchType;
        int active : 1;    // currently executing handler?
        int deleteme : 1;  // delete when handler finished?
        
        PrioQueue::handle_t heap_handle;
        int priority;
        
        static void set_priority(BaseWatcher &p, int prio)
        {
            p.priority = prio;
        }
        
        public:
        
        // Perform initialisation necessary before registration with an event loop
        void init()
        {
            active = false;
            deleteme = false;
            PrioQueue::init_handle(heap_handle);
            priority = DEFAULT_PRIORITY;
        }
        
        BaseWatcher(WatchType wt) noexcept : watchType(wt) { }
        
        virtual ~BaseWatcher() noexcept { }
        
        // Called when the watcher has been removed.
        // It is guaranteed by the caller that:
        // - the dispatch method is not currently running
        // - the dispatch method will not be called.
        virtual void watch_removed() noexcept
        {
            // TODO this "delete" behaviour could be dependent on a flag, perhaps?
            // delete this;
        }
    };
    
    // Base signal event - not part of public API
    template <typename T_Mutex, typename Traits>
    class BaseSignalWatcher : public BaseWatcher
    {
        friend class EventDispatch<T_Mutex, Traits>;
        template <typename, template <typename> class, typename> friend class dasynq::event_loop;

        protected:
        typename Traits::SigInfo siginfo;
        BaseSignalWatcher() : BaseWatcher(WatchType::SIGNAL) { }

        public:
        using SigInfo = typename Traits::SigInfo;
        typedef SigInfo &SigInfo_p;
    };
    
    template <typename T_Mutex>
    class BaseFdWatcher : public BaseWatcher
    {
        template <typename, typename Traits> friend class EventDispatch;
        template <typename, template <typename> class, typename> friend class dasynq::event_loop;
        
        protected:
        int watch_fd;
        
        // These flags are protected by the loop's internal lock:
        int watch_flags;  // events being watched
        int event_flags;  // events pending (queued)
        
        // watch_flags: for a regular fd_watcher, this specifies the events that the watcher
        //              is watching (or was watching if disabled). For a bidi_fd_watcher, specifies
        //              the events that the watcher is currently watching (i.e. specifies which
        //              halves of the Bidi watcher are enabled).

        BaseFdWatcher() noexcept : BaseWatcher(WatchType::FD) { }
    };
    
    template <typename T_Mutex>
    class BaseBidiFdWatcher : public BaseFdWatcher<T_Mutex>
    {
        template <typename, typename Traits> friend class EventDispatch;
        template <typename, template <typename> class, typename> friend class dasynq::event_loop;

        protected:
        
        // The main instance is the "input" watcher only; we keep a secondary watcher
        // with a secondary set of flags for the "output" watcher:
        BaseWatcher outWatcher = BaseWatcher(WatchType::SECONDARYFD);
        
        int read_removed : 1; // read watch removed?
        int write_removed : 1; // write watch removed?
    };
    
    template <typename T_Mutex>
    class BaseChildWatcher : public BaseWatcher
    {
        template <typename, typename Traits> friend class EventDispatch;
        template <typename, template <typename> class, typename> friend class dasynq::event_loop;
        
        protected:
        pid_t watch_pid;
        int child_status;
        
        BaseChildWatcher() : BaseWatcher(WatchType::CHILD) { }
    };
    

    template <typename T_Mutex>
    class BaseTimerWatcher : public BaseWatcher
    {
        template <typename, typename Traits> friend class EventDispatch;
        template <typename, template <typename> class, typename> friend class dasynq::event_loop;
        
        protected:
        timer_handle_t timer_handle;
        int intervals;

        BaseTimerWatcher() : BaseWatcher(WatchType::TIMER)
        {
            init_timer_handle(timer_handle);
        }
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
        friend class waitqueue<NullMutex>;
        
        public:
        void wait(std::unique_lock<NullMutex> &ul) { }
        void signal() { }
        
        DASYNQ_EMPTY_BODY;
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

    template <> class waitqueue<NullMutex>
    {
        public:
        waitqueue_node<NullMutex> * unqueue()
        {
            return nullptr;
        }
        
        waitqueue_node<NullMutex> * getHead()
        {
            return nullptr;
        }
        
        bool checkHead(waitqueue_node<NullMutex> &node)
        {
            return true;
        }
        
        bool isEmpty()
        {
            return true;
        }
        
        void queue(waitqueue_node<NullMutex> *node)
        {
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
            if (head == nullptr) {
                tail = nullptr;
            }
            return head;
        }
        
        waitqueue_node<T_Mutex> * getHead()
        {
            return head;
        }
        
        bool checkHead(waitqueue_node<T_Mutex> &node)
        {
            return head == &node;
        }
        
        bool isEmpty()
        {
            return head == nullptr;
        }
        
        void queue(waitqueue_node<T_Mutex> *node)
        {
            if (tail) {
                tail->next = node;
            }
            else {
                head = node;
            }
            tail = node;
        }
    };
    
    // This class serves as the base class (mixin) for the AEN mechanism class.
    //
    // The EventDispatch class maintains the queued event data structures. It inserts watchers
    // into the queue when eventes are received (receiveXXX methods).
    template <typename T_Mutex, typename Traits> class EventDispatch : public Traits
    {
        template <typename, template <typename> class, typename> friend class dasynq::event_loop;

        // queue data structure/pointer
        PrioQueue event_queue;
        
        using BaseSignalWatcher = dasynq::dprivate::BaseSignalWatcher<T_Mutex,Traits>;
        using BaseFdWatcher = dasynq::dprivate::BaseFdWatcher<T_Mutex>;
        using BaseBidiFdWatcher = dasynq::dprivate::BaseBidiFdWatcher<T_Mutex>;
        using BaseChildWatcher = dasynq::dprivate::BaseChildWatcher<T_Mutex>;
        using BaseTimerWatcher = dasynq::dprivate::BaseTimerWatcher<T_Mutex>;
        
        // Add a watcher into the queuing system (but don't queue it)
        //   may throw: std::bad_alloc
        void prepare_watcher(BaseWatcher *bwatcher)
        {
            event_queue.allocate(bwatcher->heap_handle, bwatcher);
        }
        
        void queueWatcher(BaseWatcher *bwatcher) noexcept
        {
            event_queue.insert(bwatcher->heap_handle, bwatcher->priority);
        }
        
        bool isQueued(BaseWatcher *bwatcher)
        {
            return event_queue.is_queued(bwatcher->heap_handle);
        }

        void dequeueWatcher(BaseWatcher *bwatcher)
        {
            if (event_queue.is_queued(bwatcher->heap_handle)) {
                event_queue.remove(bwatcher->heap_handle);
            }
        }
        
        // Remove watcher from the queueing system
        void release_watcher(BaseWatcher *bwatcher)
        {
            event_queue.deallocate(bwatcher->heap_handle);
        }
        
        protected:
        T_Mutex lock;

        template <typename T> void init(T *loop) { }
        
        // Receive a signal; return true to disable signal watch or false to leave enabled
        template <typename T>
        bool receiveSignal(T &loop_mech, typename Traits::SigInfo & siginfo, void * userdata)
        {
            BaseSignalWatcher * bwatcher = static_cast<BaseSignalWatcher *>(userdata);
            bwatcher->siginfo = siginfo;
            queueWatcher(bwatcher);
            return true;
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
                bbdw->watch_flags &= ~flags;
                if (flags & IN_EVENTS && flags & OUT_EVENTS) {
                    // Queue the secondary watcher first:
                    queueWatcher(&bbdw->outWatcher);
                }
                else if (flags & OUT_EVENTS) {                
                    // Use the secondary watcher for queueing:
                    bwatcher = &(bbdw->outWatcher);
                }
            }

            queueWatcher(bwatcher);
            
            if (! LoopTraits::has_separate_rw_fd_watches) {
                // If this is a bidirectional fd-watch, it has been disabled in *both* directions
                // as the event was delivered. However, the other direction should not be disabled
                // yet, so we need to re-enable:
                int in_out_mask = IN_EVENTS | OUT_EVENTS;
                if (is_multi_watch && (bfdw->watch_flags & in_out_mask) != 0) {
                    // We need to re-enable the other channel now:
                    loop_mech.enableFdWatch_nolock(bfdw->watch_fd, userdata,
                        (bfdw->watch_flags & in_out_mask) | ONE_SHOT);
                }
            }
        }
        
        void receiveChildStat(pid_t child, int status, void * userdata)
        {
            BaseChildWatcher * watcher = static_cast<BaseChildWatcher *>(userdata);
            watcher->child_status = status;
            queueWatcher(watcher);
        }
        
        void receiveTimerExpiry(timer_handle_t & timer_handle, void * userdata, int intervals)
        {
            BaseTimerWatcher * watcher = static_cast<BaseTimerWatcher *>(userdata);
            watcher->intervals = intervals;
            queueWatcher(watcher);
        }
        
        // Pull a single event from the queue
        BaseWatcher * pullEvent()
        {
            if (event_queue.empty()) {
                return nullptr;
            }
            
            auto rhndl = event_queue.get_root();
            BaseWatcher *r = event_queue.node_data(rhndl);
            event_queue.pull_root();
            return r;
        }
        
        void issueDelete(BaseWatcher *watcher) noexcept
        {
            // This is only called when the attention lock is held, so if the watcher is not
            // active/queued now, it cannot become active (and will not be reported with an event)
            // during execution of this function.
            
            lock.lock();
            
            if (watcher->active) {
                // If the watcher is active, set deleteme true; the watcher will be removed
                // at the end of current processing (i.e. when active is set false).
                watcher->deleteme = true;
                release_watcher(watcher);
                lock.unlock();
            }
            else {
                // Actually do the delete.
                dequeueWatcher(watcher);
                release_watcher(watcher);
                
                lock.unlock();
                watcher->watch_removed();
            }
        }
        
        void issueDelete(BaseBidiFdWatcher *watcher) noexcept
        {
            lock.lock();
            
            if (watcher->active) {
                watcher->deleteme = true;
                release_watcher(watcher);
            }
            else {
                dequeueWatcher(watcher);
                release_watcher(watcher);
                watcher->read_removed = true;
            }
            
            BaseWatcher *secondary = &(watcher->outWatcher);
            if (secondary->active) {
                secondary->deleteme = true;
                release_watcher(watcher);
            }
            else {
                dequeueWatcher(secondary);
                release_watcher(watcher);
                watcher->write_removed = true;
            }
            
            if (watcher->read_removed && watcher->write_removed) {
                lock.unlock();
                watcher->watch_removed();
            }
            else {
                lock.unlock();
            }
        }

        public:
        using mutex_t = T_Mutex;
    };
}


template <typename T_Mutex, template <typename> class Loop, typename LoopTraits>
class event_loop
{
    using my_event_loop_t = event_loop<T_Mutex, Loop, LoopTraits>;
    friend class dprivate::fd_watcher<my_event_loop_t>;
    friend class dprivate::bidi_fd_watcher<my_event_loop_t>;
    friend class dprivate::signal_watcher<my_event_loop_t>;
    friend class dprivate::child_proc_watcher<my_event_loop_t>;
    friend class dprivate::timer<my_event_loop_t>;
    
    public:
    using loop_traits_t = LoopTraits;
    
    private:
    template <typename T, typename U> using EventDispatch = dprivate::EventDispatch<T,U>;
    template <typename T> using waitqueue = dprivate::waitqueue<T>;
    template <typename T> using waitqueue_node = dprivate::waitqueue_node<T>;
    using BaseWatcher = dprivate::BaseWatcher;
    using BaseSignalWatcher = dprivate::BaseSignalWatcher<T_Mutex,LoopTraits>;
    using BaseFdWatcher = dprivate::BaseFdWatcher<T_Mutex>;
    using BaseBidiFdWatcher = dprivate::BaseBidiFdWatcher<T_Mutex>;
    using BaseChildWatcher = dprivate::BaseChildWatcher<T_Mutex>;
    using BaseTimerWatcher = dprivate::BaseTimerWatcher<T_Mutex>;
    using WatchType = dprivate::WatchType;
    
    Loop<EventDispatch<T_Mutex, LoopTraits>> loop_mech;

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
        loop_mech.prepare_watcher(callBack);
        try {
            loop_mech.addSignalWatch(signo, callBack);
        }
        catch (...) {
            loop_mech.release_watcher(callBack);
            throw;
        }
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

    void registerFd(BaseFdWatcher *callback, int fd, int eventmask, bool enabled)
    {
        loop_mech.prepare_watcher(callback);
        try {
            loop_mech.addFdWatch(fd, callback, eventmask | ONE_SHOT, enabled);
        }
        catch (...) {
            loop_mech.release_watcher(callback);
            throw;
        }
    }
    
    void registerFd(BaseBidiFdWatcher *callback, int fd, int eventmask)
    {
        loop_mech.prepare_watcher(callback);
        try {
            loop_mech.prepare_watcher(&callback->outWatcher);
            try {
                if (LoopTraits::has_separate_rw_fd_watches) {
                    loop_mech.addBidiFdWatch(fd, callback, eventmask | ONE_SHOT);
                }
                else {
                    loop_mech.addFdWatch(fd, callback, eventmask | ONE_SHOT);
                }
            }
            catch (...) {
                loop_mech.release_watcher(&callback->outWatcher);
                throw;
            }
        }
        catch (...) {
            loop_mech.release_watcher(callback);
            throw;
        }
    }
    
    void setFdEnabled(BaseWatcher *watcher, int fd, int watch_flags, bool enabled)
    {
        if (enabled) {
            loop_mech.enableFdWatch(fd, watcher, watch_flags | ONE_SHOT);
        }
        else {
            loop_mech.disableFdWatch(fd, watch_flags);
        }
    }

    void setFdEnabled_nolock(BaseWatcher *watcher, int fd, int watch_flags, bool enabled)
    {
        if (enabled) {
            loop_mech.enableFdWatch_nolock(fd, watcher, watch_flags | ONE_SHOT);
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
            loop_mech.removeBidiFdWatch(fd);
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
    
    void reserveChildWatch(BaseChildWatcher *callback)
    {
        loop_mech.prepare_watcher(callback);
        try {
            loop_mech.reserveChildWatch();
        }
        catch (...) {
            loop_mech.release_watcher(callback);
            throw;
        }
    }
    
    void unreserve(BaseChildWatcher *callback)
    {
        loop_mech.unreserveChildWatch();
        loop_mech.release_watcher(callback);
    }
    
    void registerChild(BaseChildWatcher *callback, pid_t child)
    {
        loop_mech.prepare_watcher(callback);
        try {
            loop_mech.addChildWatch(child, callback);
        }
        catch (...) {
            loop_mech.release_watcher(callback);
            throw;
        }
    }
    
    void registerReservedChild(BaseChildWatcher *callBack, pid_t child) noexcept
    {
        loop_mech.addReservedChildWatch(child, callBack);
    }

    void registerReservedChild_nolock(BaseChildWatcher *callBack, pid_t child) noexcept
    {
        loop_mech.addReservedChildWatch_nolock(child, callBack);
    }
    
    void deregister(BaseChildWatcher *callback, pid_t child)
    {
        loop_mech.removeChildWatch(child);

        waitqueue_node<T_Mutex> qnode;
        getAttnLock(qnode);
        
        EventDispatch<T_Mutex, LoopTraits> & ed = (EventDispatch<T_Mutex, LoopTraits> &) loop_mech;
        ed.issueDelete(callback);
        
        releaseLock(qnode);
    }
    
    void registerTimer(BaseTimerWatcher *callback, clock_type clock)
    {
        loop_mech.prepare_watcher(callback);
        try {
            loop_mech.addTimer(callback->timer_handle, callback, clock);
        }
        catch (...) {
            loop_mech.release_watcher(callback);
        }
    }
    
    void setTimer(BaseTimerWatcher *callBack, struct timespec &timeout, clock_type clock)
    {
        struct timespec interval {0, 0};
        loop_mech.setTimer(callBack->timer_handle, timeout, interval, true, clock);
    }
    
    void setTimer(BaseTimerWatcher *callBack, struct timespec &timeout, struct timespec &interval, clock_type clock)
    {
        loop_mech.setTimer(callBack->timer_handle, timeout, interval, true, clock);
    }

    void setTimerRel(BaseTimerWatcher *callBack, struct timespec &timeout, clock_type clock)
    {
        struct timespec interval {0, 0};
        loop_mech.setTimerRel(callBack->timer_handle, timeout, interval, true, clock);
    }
    
    void setTimerRel(BaseTimerWatcher *callBack, struct timespec &timeout, struct timespec &interval, clock_type clock)
    {
        loop_mech.setTimerRel(callBack->timer_handle, timeout, interval, true, clock);
    }

    void deregister(BaseTimerWatcher *callback, clock_type clock)
    {
        loop_mech.removeTimer(callback->timer_handle, clock);
        
        waitqueue_node<T_Mutex> qnode;
        getAttnLock(qnode);
        
        EventDispatch<T_Mutex, LoopTraits> & ed = (EventDispatch<T_Mutex, LoopTraits> &) loop_mech;
        ed.issueDelete(callback);
        
        releaseLock(qnode);
    }
    
    void dequeueWatcher(BaseWatcher *watcher) noexcept
    {
        loop_mech.dequeueWatcher(watcher);
    }

    // Acquire the attention lock (when held, ensures that no thread is polling the AEN
    // mechanism).
    void getAttnLock(waitqueue_node<T_Mutex> &qnode)
    {
        std::unique_lock<T_Mutex> ulock(wait_lock);
        attn_waitqueue.queue(&qnode);        
        if (! attn_waitqueue.checkHead(qnode)) {
            loop_mech.interrupt_wait();
            while (! attn_waitqueue.checkHead(qnode)) {
                qnode.wait(ulock);
            }
        }
    }
    
    // Acquire the poll-wait lock (to be held when polling the AEN mechanism; lower
    // priority than the attention lock).
    void getPollwaitLock(waitqueue_node<T_Mutex> &qnode)
    {
        std::unique_lock<T_Mutex> ulock(wait_lock);
        if (attn_waitqueue.isEmpty()) {
            // Queue is completely empty:
            attn_waitqueue.queue(&qnode);
        }
        else {
            wait_waitqueue.queue(&qnode);
        }
        
        while (! attn_waitqueue.checkHead(qnode)) {
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
            if (! wait_waitqueue.isEmpty()) {
                auto nhead = wait_waitqueue.getHead();
                wait_waitqueue.unqueue();
                attn_waitqueue.queue(nhead);
                nhead->signal();
            }
        }                
    }
    
    void processSignalRearm(BaseSignalWatcher * bsw, rearm rearmType)
    {
        // Called with lock held
        if (rearmType == rearm::REARM) {
            loop_mech.rearmSignalWatch_nolock(bsw->siginfo.get_signo());
        }
        else if (rearmType == rearm::REMOVE) {
            loop_mech.removeSignalWatch_nolock(bsw->siginfo.get_signo());
        }
    }

    // Process rearm return for fd_watcher, including the primary watcher of a bidi_fd_watcher
    rearm processFdRearm(BaseFdWatcher * bfw, rearm rearmType, bool is_multi_watch)
    {
        // Called with lock held;
        //   bdfw->event_flags contains only with pending (queued) events
        
        if (is_multi_watch) {
            BaseBidiFdWatcher * bdfw = static_cast<BaseBidiFdWatcher *>(bfw);
            
            if (rearmType == rearm::REMOVE) {
                bdfw->read_removed = 1;
                
                if (LoopTraits::has_separate_rw_fd_watches) {
                    bdfw->watch_flags &= ~IN_EVENTS;
                    loop_mech.removeFdWatch_nolock(bdfw->watch_fd, IN_EVENTS);
                    return bdfw->write_removed ? rearm::REMOVE : rearm::NOOP;
                }
                else {
                    if (! bdfw->write_removed) {
                        if (bdfw->watch_flags & IN_EVENTS) {
                            bdfw->watch_flags &= ~IN_EVENTS;
                            loop_mech.enableFdWatch_nolock(bdfw->watch_fd, bdfw, bdfw->watch_flags);
                        }
                        return rearm::NOOP;
                    }
                    else {
                        // both removed: actually remove
                        loop_mech.removeFdWatch_nolock(bdfw->watch_fd, 0 /* not used */);
                        return rearm::REMOVE;
                    }
                }
            }
            else if (rearmType == rearm::DISARM) {
                // Nothing more to do
            }
            else if (rearmType == rearm::REARM) {
                bdfw->watch_flags |= IN_EVENTS;
                
                if (! LoopTraits::has_separate_rw_fd_watches) {
                    int watch_flags = bdfw->watch_flags;
                    loop_mech.enableFdWatch_nolock(bdfw->watch_fd,
                            static_cast<BaseWatcher *>(bdfw),
                            (watch_flags & (IN_EVENTS | OUT_EVENTS)) | ONE_SHOT);
                }
                else {
                    loop_mech.enableFdWatch_nolock(bdfw->watch_fd,
                            static_cast<BaseWatcher *>(bdfw),
                            IN_EVENTS | ONE_SHOT);
                }
            }
            return rearmType;
        }
        else { // Not multi-watch:
            if (rearmType == rearm::REARM) {
                loop_mech.enableFdWatch_nolock(bfw->watch_fd, bfw,
                        (bfw->watch_flags & (IN_EVENTS | OUT_EVENTS)) | ONE_SHOT);
            }
            else if (rearmType == rearm::REMOVE) {
                loop_mech.removeFdWatch_nolock(bfw->watch_fd, bfw->watch_flags);
            }
            return rearmType;
        }
    }

    // Process re-arm for the secondary (output) watcher in a Bi-direction Fd watcher.
    rearm processSecondaryRearm(BaseBidiFdWatcher * bdfw, rearm rearmType)
    {
        // Called with lock held
        if (rearmType == rearm::REMOVE) {
            bdfw->write_removed = 1;
            
            if (LoopTraits::has_separate_rw_fd_watches) {
                bdfw->watch_flags &= ~OUT_EVENTS;
                loop_mech.removeFdWatch_nolock(bdfw->watch_fd, OUT_EVENTS);
                return bdfw->read_removed ? rearm::REMOVE : rearm::NOOP;
            }
            else {
                if (! bdfw->read_removed) {
                    if (bdfw->watch_flags & OUT_EVENTS) {
                        bdfw->watch_flags &= ~OUT_EVENTS;
                        loop_mech.enableFdWatch_nolock(bdfw->watch_fd, bdfw, bdfw->watch_flags);
                    }
                    return rearm::NOOP;
                }
                else {
                    // both removed: actually remove
                    loop_mech.removeFdWatch_nolock(bdfw->watch_fd, 0 /* not used */);
                    return rearm::REMOVE;
                }
            }
        }
        else if (rearmType == rearm::DISARM) {
            // Nothing more to do
        }
        else if (rearmType == rearm::REARM) {
            bdfw->watch_flags |= OUT_EVENTS;
            
            if (! LoopTraits::has_separate_rw_fd_watches) {
                int watch_flags = bdfw->watch_flags;
                loop_mech.enableFdWatch_nolock(bdfw->watch_fd,
                        static_cast<BaseWatcher *>(bdfw),
                        (watch_flags & (IN_EVENTS | OUT_EVENTS)) | ONE_SHOT);
            }
            else {
                loop_mech.enableFdWatch_nolock(bdfw->watch_fd,
                        static_cast<BaseWatcher *>(bdfw),
                        OUT_EVENTS | ONE_SHOT);
            }
        }
        return rearmType;
    }
    
    void processTimerRearm(BaseTimerWatcher *btw, rearm rearmType)
    {
        // Called with lock held
        if (rearmType == rearm::REARM) {
            loop_mech.enableTimer_nolock(btw->timer_handle, true);
        }
        else if (rearmType == rearm::REMOVE) {
            loop_mech.removeTimer_nolock(btw->timer_handle);
        }
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
            
            rearm rearmType = rearm::NOOP;
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
                    bfw->watch_flags &= ~IN_EVENTS;
                }
            }
            else if (pqueue->watchType == WatchType::SECONDARYFD) {
                is_multi_watch = true;
                // construct a pointer to the main watcher:
                char * rp = (char *)pqueue;
                rp -= offsetof(BaseBidiFdWatcher, outWatcher);
                bbfw = (BaseBidiFdWatcher *)rp;
                if (! LoopTraits::has_separate_rw_fd_watches) {
                    bbfw->watch_flags &= ~OUT_EVENTS;
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
                rearmType = ((signal_watcher *)bsw)->received(*this, bsw->siginfo.get_signo(), bsw->siginfo);
                break;
            }
            case WatchType::FD: {
                BaseFdWatcher *bfw = static_cast<BaseFdWatcher *>(pqueue);
                if (is_multi_watch) {
                    // The primary watcher for a multi-watch watcher is queued for
                    // read events.
                    rearmType = ((bidi_fd_watcher *)bbfw)->read_ready(*this, bfw->watch_fd);
                    bbfw->event_flags &= ~IN_EVENTS;
                }
                else {
                    rearmType = ((fd_watcher *)bfw)->fd_event(*this, bfw->watch_fd, bfw->event_flags);
                    bfw->event_flags = 0;
                }
                break;
            }
            case WatchType::CHILD: {
                BaseChildWatcher *bcw = static_cast<BaseChildWatcher *>(pqueue);
                rearmType = ((child_proc_watcher *)bcw)->child_status(*this, bcw->watch_pid, bcw->child_status);
                break;
            }
            case WatchType::SECONDARYFD: {
                rearmType = ((bidi_fd_watcher *)bbfw)->write_ready(*this, bbfw->watch_fd);
                bbfw->event_flags &= ~OUT_EVENTS;
                break;
            }
            case WatchType::TIMER: {
                BaseTimerWatcher *btw = static_cast<BaseTimerWatcher *>(pqueue);
                rearmType = ((timer *)btw)->timer_expiry(*this, btw->intervals);
                break;
            }
            default: ;
            }

            ed.lock.lock();
            
            // (if REMOVED, we must not touch pqueue at all)
            if (rearmType != rearm::REMOVED) {
                
                pqueue->active = false;
                if (pqueue->deleteme) {
                    // We don't want a watch that is marked "deleteme" to re-arm itself.
                    rearmType = rearm::REMOVE;
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
                case WatchType::TIMER:
                    processTimerRearm(static_cast<BaseTimerWatcher *>(pqueue), rearmType);
                    break;
                default: ;
                }
                
                if (rearmType == rearm::REMOVE) {
                    ed.lock.unlock();
                    // Note that for BidiFd watches, watch_removed is only called on the primary watch.
                    // The process function called above only returns Rearm::REMOVE if both primary and
                    // secondary watches have been removed.
                    (is_multi_watch ? bbfw : pqueue)->watch_removed();
                    ed.lock.lock();
                }
            }
            
            pqueue = ed.pullEvent();
        }
        
        ed.lock.unlock();
        return active;
    }

    
    public:
    using mutex_t = T_Mutex;
    
    using fd_watcher = dprivate::fd_watcher<my_event_loop_t>;
    using bidi_fd_watcher = dprivate::bidi_fd_watcher<my_event_loop_t>;
    using signal_watcher = dprivate::signal_watcher<my_event_loop_t>;
    using child_proc_watcher = dprivate::child_proc_watcher<my_event_loop_t>;
    using timer = dprivate::timer<my_event_loop_t>;
    
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

typedef event_loop<NullMutex> NEventLoop;
typedef event_loop<std::mutex> TEventLoop;

// from dasync.cc:
TEventLoop & getSystemLoop();

namespace dprivate {

// Posix signal event watcher
template <typename EventLoop>
class signal_watcher : private dprivate::BaseSignalWatcher<typename EventLoop::mutex_t, typename EventLoop::loop_traits_t>
{
    using BaseWatcher = dprivate::BaseWatcher;
    using T_Mutex = typename EventLoop::mutex_t;
    
public:
    using SigInfo_p = typename dprivate::BaseSignalWatcher<T_Mutex, typename EventLoop::loop_traits_t>::SigInfo_p;

    // Register this watcher to watch the specified signal.
    // If an attempt is made to register with more than one event loop at
    // a time, behaviour is undefined. The signal should be masked before
    // call.
    inline void add_watch(EventLoop &eloop, int signo, int prio = DEFAULT_PRIORITY)
    {
        BaseWatcher::init();
        this->priority = prio;
        this->siginfo.set_signo(signo);
        eloop.registerSignal(this, signo);
    }
    
    inline void deregister(EventLoop &eloop) noexcept
    {
        eloop.deregister(this, this->siginfo.get_signo());
    }
    
    template <typename T>
    static signal_watcher<EventLoop> *add_watch(EventLoop &eloop, int signo, T watchHndlr)
    {
        class LambdaSigWatcher : public signal_watcher<EventLoop>
        {
            private:
            T watchHndlr;

            public:
            LambdaSigWatcher(T watchHandlr_a) : watchHndlr(watchHandlr_a)
            {
                //
            }

            rearm received(EventLoop &eloop, int signo, SigInfo_p siginfo) override
            {
                return watchHndlr(eloop, signo, siginfo);
            }

            void watch_removed() noexcept override
            {
                delete this;
            }
        };

        LambdaSigWatcher * lsw = new LambdaSigWatcher(watchHndlr);
        lsw->add_watch(eloop, signo);
        return lsw;
    }

    virtual rearm received(EventLoop &eloop, int signo, SigInfo_p siginfo) = 0;
};

// Posix file descriptor event watcher
template <typename EventLoop>
class fd_watcher : private dprivate::BaseFdWatcher<typename EventLoop::mutex_t>
{
    using BaseWatcher = dprivate::BaseWatcher;
    using T_Mutex = typename EventLoop::mutex_t;

    protected:
    
    // Set the types of event to watch. Only supported if LoopTraits::has_bidi_fd_watch
    // is true; otherwise has unspecified behavior.
    // Only safe to call from within the callback handler (fdEvent). Might not take
    // effect until the current callback handler returns with REARM.
    void set_watch_flags(int newFlags)
    {
        this->watch_flags = newFlags;
    }
    
    public:
    
    // Register a file descriptor watcher with an event loop. Flags
    // can be any combination of dasynq::IN_EVENTS / dasynq::OUT_EVENTS.
    // Exactly one of IN_EVENTS/OUT_EVENTS must be specified if the event
    // loop does not support bi-directional fd watchers (i.e. if
    // ! LoopTraits::has_bidi_fd_watch).
    //
    // Mechanisms supporting dual watchers allow for two watchers for a
    // single file descriptor (one watching read status and the other
    // write status). Others mechanisms support only a single watcher
    // per file descriptor. Adding a watcher beyond what is supported
    // causes undefined behavior.
    //
    // Can fail with std::bad_alloc or std::system_error.
    void add_watch(EventLoop &eloop, int fd, int flags, bool enabled = true, int prio = DEFAULT_PRIORITY)
    {
        BaseWatcher::init();
        this->priority = prio;
        this->watch_fd = fd;
        this->watch_flags = flags;
        eloop.registerFd(this, fd, flags, enabled);
    }
    
    int get_watched_fd()
    {
        return this->watch_fd;
    }
    
    // Deregister a file descriptor watcher.
    //
    // If other threads may be polling the event loop, it is not safe to assume
    // the watcher is unregistered until the watch_removed() callback is issued
    // (which will not occur until the event handler returns, if it is active).
    // In a single threaded environment, it is safe to delete the watcher after
    // calling this method as long as the handler (if it is active) accesses no
    // internal state and returns rearm::REMOVED.
    void deregister(EventLoop &eloop) noexcept
    {
        eloop.deregister(this, this->watch_fd);
    }
    
    void set_enabled(EventLoop &eloop, bool enable) noexcept
    {
        std::lock_guard<T_Mutex> guard(eloop.getBaseLock());
        eloop.setFdEnabled_nolock(this, this->watch_fd, this->watch_flags, enable);
        if (! enable) {
            eloop.dequeueWatcher(this);
        }
    }
    
    // Add an Fd watch via a lambda. The watch is allocated dynamically and destroys
    // itself when removed from the event loop.
    template <typename T>
    static fd_watcher<EventLoop> *add_watch(EventLoop &eloop, int fd, int flags, T watchHndlr)
    {
        class LambdaFdWatcher : public fd_watcher<EventLoop>
        {
            private:
            T watchHndlr;
            
            public:
            LambdaFdWatcher(T watchHandlr_a) : watchHndlr(watchHandlr_a)
            {
                //
            }
            
            rearm fd_event(EventLoop &eloop, int fd, int flags) override
            {
                return watchHndlr(eloop, fd, flags);
            }
            
            void watch_removed() noexcept override
            {
                delete this;
            }
        };
        
        LambdaFdWatcher * lfd = new LambdaFdWatcher(watchHndlr);
        lfd->add_watch(eloop, fd, flags);
        return lfd;
    }
    
    virtual rearm fd_event(EventLoop &eloop, int fd, int flags) = 0;
};

// A Bi-directional file descriptor watcher with independent read- and write- channels.
// This watcher type has two event notification methods which can both potentially be
// active at the same time.
template <typename EventLoop>
class bidi_fd_watcher : private dprivate::BaseBidiFdWatcher<typename EventLoop::mutex_t>
{
    using BaseWatcher = dprivate::BaseWatcher;
    using T_Mutex = typename EventLoop::mutex_t;
    
    void set_watch_enabled(EventLoop &eloop, bool in, bool b)
    {
        int events = in ? IN_EVENTS : OUT_EVENTS;
        
        if (b) {
            this->watch_flags |= events;
        }
        else {
            this->watch_flags &= ~events;
        }
        if (EventLoop::loop_traits_t::has_separate_rw_fd_watches) {
            dprivate::BaseWatcher * watcher = in ? this : &this->outWatcher;
            eloop.setFdEnabled_nolock(watcher, this->watch_fd, events | ONE_SHOT, b);
            if (! b) {
                eloop.dequeueWatcher(watcher);
            }
        }
        else {
            eloop.setFdEnabled_nolock(this, this->watch_fd,
                    (this->watch_flags & (IN_EVENTS | OUT_EVENTS)) | ONE_SHOT,
                    (this->watch_flags & (IN_EVENTS | OUT_EVENTS)) != 0);
            if (! b) {
                dprivate::BaseWatcher * watcher = in ? this : &this->outWatcher;
                eloop.dequeueWatcher(watcher);
            }
        }
    }
    
    public:

    // This should never actually get called:
    /*
    rearm fdEvent(EventLoop &eloop, int fd, int flags) final
    {
        return rearm::REARM; // should not be reachable.
    };
    */

    void set_in_watch_enabled(EventLoop &eloop, bool b) noexcept
    {
        eloop.getBaseLock().lock();
        set_watch_enabled(eloop, true, b);
        eloop.getBaseLock().unlock();
    }
    
    void set_out_watch_enabled(EventLoop &eloop, bool b) noexcept
    {
        eloop.getBaseLock().lock();
        set_watch_enabled(eloop, false, b);
        eloop.getBaseLock().unlock();
    }
    
    // Set the watch flags, which enables/disables both the in-watch and the out-watch accordingly.
    //
    // Concurrency: this method can only be called if
    //  - it does not enable a watcher that might currently be active
    ///   - unless the event loop will not be polled while the watcher is active.
    // (i.e. it is ok to call setWatchFlags from within the readReady/writeReady handlers if no other
    //  thread will poll the event loop; it is always ok to *dis*able a watcher that might be active,
    //  though the re-arm action returned by the callback may undo the effect).
    void set_watches(EventLoop &eloop, int newFlags)
    {
        std::lock_guard<T_Mutex> guard(eloop.getBaseLock());
        if (EventLoop::loop_traits_t::has_separate_rw_fd_watches) {
            set_watch_enabled(eloop, true, (newFlags & IN_EVENTS) != 0);
            set_watch_enabled(eloop, false, (newFlags & OUT_EVENTS) != 0);
        }
        else {
            this->watch_flags = (this->watch_flags & ~IO_EVENTS) | newFlags;
            eloop.setFdEnabled((dprivate::BaseWatcher *) this, this->watch_fd, this->watch_flags & IO_EVENTS, true);
        }
    }
    
    // Register a bi-direction file descriptor watcher with an event loop. Flags
    // can be any combination of dasynq::IN_EVENTS / dasynq::OUT_EVENTS.
    //
    // Can fail with std::bad_alloc or std::system_error.
    void add_watch(EventLoop &eloop, int fd, int flags, int inprio = DEFAULT_PRIORITY, int outprio = DEFAULT_PRIORITY)
    {
        BaseWatcher::init();
        this->outWatcher.BaseWatcher::init();
        this->watch_fd = fd;
        this->watch_flags = flags | dprivate::multi_watch;
        this->read_removed = false;
        this->write_removed = false;
        this->priority = inprio;
        this->set_priority(this->outWatcher, outprio);
        eloop.registerFd(this, fd, flags);
    }
    
    int get_watched_fd()
    {
        return this->watch_fd;
    }
    
    // Deregister a bi-direction file descriptor watcher.
    //
    // If other threads may be polling the event loop, it is not safe to assume
    // the watcher is unregistered until the watch_removed() callback is issued
    // (which will not occur until the event handler returns, if it is active).
    // In a single threaded environment, it is safe to delete the watcher after
    // calling this method as long as the handler (if it is active) accesses no
    // internal state and returns rearm::REMOVED.
    void deregister(EventLoop &eloop) noexcept
    {
        eloop.deregister(this, this->watch_fd);
    }
    
    virtual rearm read_ready(EventLoop &eloop, int fd) noexcept = 0;
    virtual rearm write_ready(EventLoop &eloop, int fd) noexcept = 0;
};

// Child process event watcher
template <typename EventLoop>
class child_proc_watcher : private dprivate::BaseChildWatcher<typename EventLoop::mutex_t>
{
    using BaseWatcher = dprivate::BaseWatcher;
    using T_Mutex = typename EventLoop::mutex_t;

    public:
    // Reserve resources for a child watcher with the given event loop.
    // Reservation can fail with std::bad_alloc. Some backends do not support
    // reservation (it will always fail) - check LoopTraits::supports_childwatch_reservation.
    void reserve_watch(EventLoop &eloop)
    {
        eloop.reserveChildWatch(this);
    }
    
    void unreserve(EventLoop &eloop)
    {
        eloop.unreserve(this);
    }
    
    // Register a watcher for the given child process with an event loop.
    // Registration can fail with std::bad_alloc.
    // Note that in multi-threaded programs, use of this function may be prone to a
    // race condition such that the child terminates before the watcher is registered.
    void add_watch(EventLoop &eloop, pid_t child, int prio = DEFAULT_PRIORITY)
    {
        BaseWatcher::init();
        this->watch_pid = child;
        this->priority = prio;
        eloop.registerChild(this, child);
    }
    
    // Register a watcher for the given child process with an event loop,
    // after having reserved resources previously (using reserveWith).
    // Registration cannot fail.
    // Note that in multi-threaded programs, use of this function may be prone to a
    // race condition such that the child terminates before the watcher is registered;
    // use the "fork" member function to avoid this.
    void add_reserved(EventLoop &eloop, pid_t child, int prio = DEFAULT_PRIORITY) noexcept
    {
        BaseWatcher::init();
        this->watch_pid = child;
        this->priority = prio;
        eloop.registerReservedChild(this, child);
    }
    
    void deregister(EventLoop &eloop, pid_t child) noexcept
    {
        eloop.deregister(this, child);
    }
    
    // Fork and watch the child with this watcher on the given event loop.
    // If resource limitations prevent the child process from being watched, it is
    // terminated immediately (or if the implementation allows, never started),
    // and a suitable std::system_error or std::bad_alloc exception is thrown.
    // Returns:
    // - the child pid in the parent
    // - 0 in the child
    pid_t fork(EventLoop &eloop)
    {
        if (EventLoop::loop_traits_t::supports_childwatch_reservation) {
            // Reserve a watch, fork, then claim reservation
            reserve_watch(eloop);
            
            auto &lock = eloop.getBaseLock();
            lock.lock();
            
            pid_t child = ::fork();
            if (child == -1) {
                // Unreserve watch.
                lock.unlock();
                unreserve(eloop);
                throw std::system_error(errno, std::system_category());
            }
            
            if (child == 0) {
                // I am the child
                lock.unlock(); // may not really be necessary
                return 0;
            }
            
            // Register this watcher.
            eloop.registerReservedChild_nolock(this, child);
            lock.unlock();
            return child;
        }
        else {
            int pipefds[2];
            if (pipe2(pipefds, O_CLOEXEC) == -1) {
                throw std::system_error(errno, std::system_category());
            }
            
            std::lock_guard<T_Mutex> guard(eloop.getBaseLock());
            
            pid_t child = ::fork();
            if (child == -1) {
                throw std::system_error(errno, std::system_category());
            }
            
            if (child == 0) {
                // I am the child
                
                // Wait for message from parent before continuing:
                int rr;
                int r = read(pipefds[0], &rr, sizeof(rr));
                while (r == -1 && errno == EINTR) {
                    read(pipefds[0], &rr, sizeof(rr));
                }
                
                if (r == -1) _exit(0);
                
                close(pipefds[0]);
                return 0;
            }
            
            close(pipefds[0]); // close read end
            
            // Register this watcher.
            try {
                eloop.registerChild(this, child);
                
                // Continue in child (it doesn't matter what is written):
                write(pipefds[1], &pipefds, sizeof(int));
                close(pipefds[1]);
                
                return child;
            }
            catch (...) {
                close(pipefds[1]);
                throw;
            }
        }
    }
    
    virtual rearm child_status(EventLoop &eloop, pid_t child, int status) = 0;
};

template <typename EventLoop>
class timer : private BaseTimerWatcher<typename EventLoop::mutex_t>
{
    private:
    clock_type clock;

    public:
    
    void add_timer(EventLoop &eloop, clock_type clock = clock_type::MONOTONIC, int prio = DEFAULT_PRIORITY)
    {
        this->priority = prio;
        this->clock = clock;
        eloop.registerTimer(this, clock);
    }
    
    void arm_timer(EventLoop &eloop, struct timespec &timeout) noexcept
    {
        eloop.setTimer(this, timeout, clock);
    }
    
    void arm_timer(EventLoop &eloop, struct timespec &timeout, struct timespec &interval) noexcept
    {
        eloop.setTimer(this, timeout, interval, clock);
    }

    // Arm timer, relative to now:
    void arm_timer_rel(EventLoop &eloop, struct timespec &timeout) noexcept
    {
        eloop.setTimerRel(this, timeout, clock);
    }
    
    void arm_timer_rel(EventLoop &eloop, struct timespec &timeout, struct timespec &interval) noexcept
    {
        eloop.setTimerRel(this, timeout, interval, clock);
    }
    
    void deregister(EventLoop &eloop) noexcept
    {
        eloop.deregister(this, clock);
    }

    // Timer expired, and the given number of intervals have elapsed before
    // expiry evenet was queued. Normally intervals == 1 to indicate no
    // overrun.
    virtual rearm timer_expiry(EventLoop &eloop, int intervals) = 0;
};

}  // namespace dasynq::dprivate
}  // namespace dasynq

#endif
