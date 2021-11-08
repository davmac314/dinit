#ifndef DASYNQ_H_
#define DASYNQ_H_

#include "dasynq/config.h"

#include "dasynq/flags.h"
#include "dasynq/stableheap.h"
#include "dasynq/interrupt.h"
#include "dasynq/util.h"

// Dasynq uses a "mix-in" pattern to produce an event loop implementation incorporating selectable
// implementations of various components (main backend, timers, child process watch mechanism etc). In C++
// this can be achieved by a template for some component which extends its own type parameter:
//
//     template <typename Base> class X : public B { .... }
//
// (Note that in a sense this is actually the opposite of the so-called "Curiously Recurring Template"
// pattern, which can be used to achieve a similar goal). We can chain several such components together to
// "mix in" the functionality of each into the final class, eg:
//
//     template <typename T> using loop_t =
//         epoll_loop<interrupt_channel<timer_fd_events<child_proc_events<T>>>>;
//
// (which defines an alias template "loop_t", whose implementation will use the epoll backend, a standard
// interrupt channel implementation, a timerfd-based timer implementation, and the standard child process
// watch implementation). We sometimes need the base class to be able to call derived-class members: to do
// this we pass a reference to the derived instance into a template member function in the base, for example
// the "init" function:
//
//     template <typename T> void init(T *derived)
//     {
//         // can call method on derived:
//         derived->add_listener();
//         // chain to next class:
//         Base::init(derived);
//     }
//
// The 'loop_t' defined above is a template for a usable backend mechanism for the event_loop template
// class. At the base all this is the event_dispatch class, defined below, which receives event
// notifications and inserts them into a queue for processing. The event_loop class, also below, wraps this
// (via composition) in an interface which can be used to register/de-register/enable/disable event
// watchers, and which can process the queued events by calling the watcher callbacks. The event_loop class
// also provides some synchronisation to ensure thread-safety, and abstracts away some differences between
// backends.
//
// The differences are exposed as traits, partly via a separate traits class (loop_traits_t as defined
// below, which contains the "main" traits, particularly the sigdata_t, fd_r and fd_s types). Note that the
// event_dispatch class exposes the loop traits as traits_t, and these are then potentially augmented at
// each stage of the mechanism inheritance chain (i.e. the final traits are exposed as
// `loop_t<event_dispatch>::traits_t'.
//
// The trait members are:
//   sigdata_t  - a wrapper for the siginfo_t type or equivalent used to pass signal parameters
//   fd_r       - a file descriptor wrapper, if the backend is able to retrieve the file descriptor when
//                it receives an fd event. Not all backends can do this.
//   fd_s       - a file descriptor storage wrapper. If the backend can retrieve file descriptors, this
//                will be empty (and ideally zero-size), otherwise it stores a file descriptor.
//                With an fd_r and fd_s instance you can always retrieve the file descriptor:
//                `fdr.get_fd(fds)' will return it.
//   has_bidi_fd_watch
//              - boolean indicating whether a single watch can support watching for both input and output
//                events simultaneously
//   has_separate_rw_fd_watches
//              - boolean indicating whether it is possible to add separate input and output watches for the
//                same fd. Either this or has_bidi_fd_watch must be true.
//   interrupt_after_fd_add
//              - boolean indicating if a loop interrupt must be forced after adding/enabling an fd watch.
//   interrupt_after_signal_add
//              - boolean indicating if a loop interrupt must be forced after adding or enabling a signal
//                watch.
//   supports_non_oneshot_fd
//              - boolean; if true, event_dispatch can arm an fd watch without ONESHOT and returning zero
//                events from receive_fd_event (the event notification function) will leave the descriptor
//                armed. If false, all fd watches are effectively ONESHOT (they can be re-armed immediately
//                after delivery by returning an appropriate event flag mask).
//   full_timer_support
//              - boolean indicating that the monotonic and system clocks are actually different clocks and
//                that timers against the system clock will work correctly if the system clock time is
//                adjusted. If false, the monotonic clock may not be present at all (monotonic clock will map
//                to system clock), and timers against either clock are not guaranteed to work correctly if
//                the system clock is adjusted.

#if DASYNQ_HAVE_EPOLL <= 0
#if _POSIX_TIMERS > 0
#include "dasynq/posixtimer.h"
namespace dasynq {
    template <typename T, bool provide_mono_timer = true> using timer_events = posix_timer_events<T, provide_mono_timer>;
} // namespace dasynq
#else
#include "dasynq/itimer.h"
namespace dasynq {
    template <typename T, bool provide_mono_timer = true> using timer_events = itimer_events<T, provide_mono_timer>;
} // namespace dasynq
#endif
#endif

#if DASYNQ_HAVE_KQUEUE
#if DASYNQ_KQUEUE_MACOS_WORKAROUND
#include "dasynq/kqueue-macos.h"
#include "dasynq/childproc.h"
namespace dasynq {
    template <typename T> using loop_t = macos_kqueue_loop<timer_events<child_proc_events<interrupt_channel<T>>, false>>;
    using loop_traits_t = macos_kqueue_traits;
} // namespace dasynq
#else
#include "dasynq/kqueue.h"
#include "dasynq/childproc.h"
namespace dasynq {
    template <typename T> using loop_t = kqueue_loop<timer_events<child_proc_events<interrupt_channel<T>>, false>>;
    using loop_traits_t = kqueue_traits;
} // namespace dasynq
#endif
#elif DASYNQ_HAVE_EPOLL
#include "dasynq/epoll.h"
#include "dasynq/timerfd.h"
#include "dasynq/childproc.h"
namespace dasynq {
    template <typename T> using loop_t = epoll_loop<interrupt_channel<timer_fd_events<child_proc_events<T>>>>;
    using loop_traits_t = epoll_traits;
} // namespace dasynq
#else
#include "dasynq/childproc.h"
#if DASYNQ_HAVE_PSELECT
#include "dasynq/pselect.h"
namespace dasynq {
    template <typename T> using loop_t = pselect_events<timer_events<interrupt_channel<child_proc_events<T>>, false>>;
    using loop_traits_t = select_traits;
} // namespace dasynq
#else
#include "dasynq/select.h"
namespace dasynq {
    template <typename T> using loop_t = select_events<timer_events<interrupt_channel<child_proc_events<T>>, false>>;
    using loop_traits_t = select_traits;
} // namespace dasynq
#endif
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <system_error>

#include <unistd.h>
#include <fcntl.h>

#include "dasynq/mutex.h"

#include "dasynq/basewatchers.h"

namespace dasynq {

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
    REMOVED,
    /** RE-queue the watcher to have its notification called again */
    REQUEUE
};

// Tag type to specify that initialisation should be delayed
class delayed_init {
    DASYNQ_EMPTY_BODY
};

namespace dprivate {

    // Classes for implementing a fair(ish) wait queue.
    // A queue node can be signalled when it reaches the head of
    // the queue.

    template <typename T_Mutex> class waitqueue;
    template <typename T_Mutex> class waitqueue_node;

    // Select an appropriate condition variable type for a mutex:
    // condition_variable if mutex is std::mutex, or condition_variable_any
    // otherwise.
    template <class T_Mutex> class condvar_selector;

    template <> class condvar_selector<std::mutex>
    {
        public:
        typedef std::condition_variable condvar;
    };

    template <class T_Mutex> class condvar_selector
    {
        public:
        typedef std::condition_variable_any condvar;
    };

    // For a single-threaded loop, the waitqueue is a no-op:
    template <> class waitqueue_node<null_mutex>
    {
        // Specialised waitqueue_node for null_mutex.
        friend class waitqueue<null_mutex>;
        
        public:
        void wait(std::unique_lock<null_mutex> &ul) { }
        void signal() { }
        
        DASYNQ_EMPTY_BODY
    };

    template <typename T_Mutex> class waitqueue_node
    {
        typename condvar_selector<T_Mutex>::condvar condvar;
        friend class waitqueue<T_Mutex>;

        // ptr to next node in queue, set to null when added to queue tail:
        waitqueue_node * next;
        
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

    template <> class waitqueue<null_mutex>
    {
        public:
        // remove current head of queue, return new head:
        waitqueue_node<null_mutex> * unqueue()
        {
            return nullptr;
        }
        
        waitqueue_node<null_mutex> * get_head()
        {
            return nullptr;
        }
        
        waitqueue_node<null_mutex> * get_second()
        {
            return nullptr;
        }

        bool check_head(waitqueue_node<null_mutex> &node)
        {
            return true;
        }
        
        bool is_empty()
        {
            return true;
        }
        
        void queue(waitqueue_node<null_mutex> *node)
        {
        }
    };

    template <typename T_Mutex> class waitqueue
    {
        waitqueue_node<T_Mutex> * tail = nullptr;
        waitqueue_node<T_Mutex> * head = nullptr;

        public:
        // remove current head of queue, return new head:
        waitqueue_node<T_Mutex> * unqueue()
        {
            head = head->next;
            if (head == nullptr) {
                tail = nullptr;
            }
            return head;
        }
        
        waitqueue_node<T_Mutex> * get_head()
        {
            return head;
        }
        
        waitqueue_node<T_Mutex> * get_second()
        {
            return head->next;
        }

        bool check_head(waitqueue_node<T_Mutex> &node)
        {
            return head == &node;
        }
        
        bool is_empty()
        {
            return head == nullptr;
        }
        
        void queue(waitqueue_node<T_Mutex> *node)
        {
            node->next = nullptr;
            if (tail) {
                tail->next = node;
            }
            else {
                head = node;
            }
            tail = node;
        }
    };
    
    // friend of event_loop for giving access to various private members
    class loop_access {
        public:
        template <typename Loop>
        static typename Loop::mutex_t &get_base_lock(Loop &loop) noexcept
        {
            return loop.get_base_lock();
        }

        template <typename Loop>
        static rearm process_fd_rearm(Loop &loop, typename Loop::base_fd_watcher *bfw,
                rearm rearm_type) noexcept
        {
            return loop.process_fd_rearm(bfw, rearm_type);
        }

        template <typename Loop>
        static rearm process_primary_rearm(Loop &loop, typename Loop::base_bidi_fd_watcher *bdfw,
                rearm rearm_type) noexcept
        {
            return loop.process_primary_rearm(bdfw, rearm_type);
        }

        template <typename Loop>
        static rearm process_secondary_rearm(Loop &loop, typename Loop::base_bidi_fd_watcher * bdfw,
                base_watcher * outw, rearm rearm_type) noexcept
        {
            return loop.process_secondary_rearm(bdfw, outw, rearm_type);
        }

        template <typename Loop>
        static void process_signal_rearm(Loop &loop, typename Loop::base_signal_watcher * bsw,
                rearm rearm_type) noexcept
        {
            loop.process_signal_rearm(bsw, rearm_type);
        }

        template <typename Loop>
        static void process_child_watch_rearm(Loop &loop, typename Loop::base_child_watcher *bcw,
                rearm rearm_type) noexcept
        {
            loop.process_child_watch_rearm(bcw, rearm_type);
        }

        template <typename Loop>
        static void process_timer_rearm(Loop &loop, typename Loop::base_timer_watcher *btw,
                rearm rearm_type) noexcept
        {
            loop.process_timer_rearm(btw, rearm_type);
        }

        template <typename Loop>
        static void requeue_watcher(Loop &loop, base_watcher *watcher) noexcept
        {
            loop.requeue_watcher(watcher);
        }

        template <typename Loop>
        static void release_watcher(Loop &loop, base_watcher *watcher) noexcept
        {
            loop.release_watcher(watcher);
        }
    };

    // Do standard post-dispatch processing for a watcher. This handles the case of removing or
    // re-queueing watchers depending on the rearm type. This is called from the individual
    // watcher dispatch functions to handle REMOVE or REQUEUE re-arm values.
    template <typename Loop> void post_dispatch(Loop &loop, base_watcher *watcher, rearm rearm_type)
    {
        if (rearm_type == rearm::REMOVE) {
            loop_access::get_base_lock(loop).unlock();
            loop_access::release_watcher(loop, watcher);
            watcher->watch_removed();
            loop_access::get_base_lock(loop).lock();
        }
        else if (rearm_type == rearm::REQUEUE) {
            loop_access::requeue_watcher(loop, watcher);
        }
    }

    // Post-dispatch handling for bidi fd watchers.
    template <typename Loop> void post_dispatch(Loop &loop, bidi_fd_watcher<Loop> *bdfd_watcher,
            base_watcher *out_watcher, rearm rearm_type)
    {
        base_watcher *watcher = (base_watcher *)bdfd_watcher;
        if (rearm_type == rearm::REMOVE) {
            loop_access::get_base_lock(loop).unlock();
            loop_access::release_watcher(loop, watcher);
            loop_access::release_watcher(loop, out_watcher);
            watcher->watch_removed();
            loop_access::get_base_lock(loop).lock();
        }
        else if (rearm_type == rearm::REQUEUE) {
            loop_access::requeue_watcher(loop, watcher);
        }
    }

    // The event_dispatch class serves as the base class (mixin) for the backend mechanism. It
    // mostly manages queing and dequeing of events and maintains/owns the relevant data
    // structures, including a mutex lock.
    //
    // The backend mechanism should call one of the receiveXXX functions to notify of an event
    // received. The watcher will then be queued.
    //
    // In general the functions should be called with lock held. In practice this means that the
    // event loop backend implementations (that deposit received events here) must obtain the
    // lock; they are also free to use it to protect their own internal data structures.
    template <typename Traits, typename LoopTraits> class event_dispatch
    {
        friend class dasynq::event_loop<typename LoopTraits::mutex_t, LoopTraits>;;

        public:
        using mutex_t = typename LoopTraits::mutex_t;
        using traits_t = Traits;
        using delayed_init = dasynq::delayed_init;

        private:

        // queue data structure/pointer
        prio_queue event_queue;
        
        using base_signal_watcher = dprivate::base_signal_watcher<typename traits_t::sigdata_t>;
        using base_child_watcher = dprivate::base_child_watcher;
        using base_timer_watcher = dprivate::base_timer_watcher;
        
        // Add a watcher into the queueing system (but don't queue it). Call with lock held.
        //   may throw: std::bad_alloc
        void prepare_watcher(base_watcher *bwatcher)
        {
            allocate_handle(event_queue, bwatcher->heap_handle, bwatcher);
        }
        
        void queue_watcher(base_watcher *bwatcher) noexcept
        {
            event_queue.insert(bwatcher->heap_handle, bwatcher->priority);
        }
        
        void dequeue_watcher(base_watcher *bwatcher) noexcept
        {
            if (event_queue.is_queued(bwatcher->heap_handle)) {
                event_queue.remove(bwatcher->heap_handle);
            }
        }

        // Remove watcher from the queueing system
        void release_watcher(base_watcher *bwatcher) noexcept
        {
            event_queue.deallocate(bwatcher->heap_handle);
        }
        
        protected:
        mutex_t lock;

        template <typename T> void init(T *loop) noexcept { }
        void cleanup() noexcept { }
        
        void sigmaskf(int how, const sigset_t *set, sigset_t *oset)
        {
            LoopTraits::sigmaskf(how, set, oset);
        }

        // Receive a signal; return true to disable signal watch or false to leave enabled.
        // Called with lock held.
        template <typename T>
        bool receive_signal(T &loop_mech, typename Traits::sigdata_t & siginfo, void * userdata) noexcept
        {
            base_signal_watcher * bwatcher = static_cast<base_signal_watcher *>(userdata);
            bwatcher->siginfo = siginfo;
            queue_watcher(bwatcher);
            return true;
        }
        
        // Receive fd event delivered from backend mechansim. Returns the desired watch mask, as per
        // set_fd_enabled, which can be used to leave the watch disabled, re-enable it or re-enable
        // one direction of a bi-directional watcher.
        template <typename T>
        std::tuple<int, typename Traits::fd_s> receive_fd_event(T &loop_mech, typename Traits::fd_r fd_r,
                void * userdata, int flags) noexcept
        {
            base_fd_watcher * bfdw = static_cast<base_fd_watcher *>(userdata);
            
            bfdw->event_flags |= flags;
            typename Traits::fd_s watch_fd_s {bfdw->watch_fd};
            
            base_watcher * bwatcher = bfdw;
            
            bool is_multi_watch = bfdw->watch_flags & multi_watch;
            if (is_multi_watch) {                
                base_bidi_fd_watcher *bbdw = static_cast<base_bidi_fd_watcher *>(bwatcher);
                bbdw->watch_flags &= ~flags;
                if ((flags & IN_EVENTS) && (flags & OUT_EVENTS)) {
                    // Queue the secondary watcher first:
                    queue_watcher(&bbdw->out_watcher);
                }
                else if (flags & OUT_EVENTS) {                
                    // Use the secondary watcher for queueing:
                    bwatcher = &(bbdw->out_watcher);
                }
            }

            queue_watcher(bwatcher);
            
            if (is_multi_watch && ! traits_t::has_separate_rw_fd_watches) {
                // If this is a bidirectional fd-watch, it has been disabled in *both* directions
                // as the event was delivered. However, the other direction should not be disabled
                // yet, so we need to re-enable:
                int in_out_mask = IN_EVENTS | OUT_EVENTS;
                if ((bfdw->watch_flags & in_out_mask) != 0) {
                    // We need to re-enable the other channel now:
                    return std::make_tuple((bfdw->watch_flags & in_out_mask) | ONE_SHOT, watch_fd_s);
                    // We are the polling thread: don't need to interrupt polling, even if it would
                    // normally be required.
                }
            }

            return std::make_tuple(0, watch_fd_s);
        }
        
        // Child process terminated. Called with both the main lock and the reaper lock held.
        void receive_child_stat(pid_t child, int status, void * userdata) noexcept
        {
            base_child_watcher * watcher = static_cast<base_child_watcher *>(userdata);
            watcher->child_status = status;
            watcher->child_termd = true;
            queue_watcher(watcher);
        }
        
        void receive_timer_expiry(timer_handle_t & timer_handle, void * userdata, int intervals) noexcept
        {
            base_timer_watcher * watcher = static_cast<base_timer_watcher *>(userdata);
            watcher->intervals += intervals;
            queue_watcher(watcher);
        }
        
        // Pull a single event from the queue; returns nullptr if the queue is empty.
        // Call with lock held.
        base_watcher * pull_queued_event() noexcept
        {
            if (event_queue.empty()) {
                return nullptr;
            }
            
            auto & rhndl = event_queue.get_root();
            base_watcher *r = dprivate::get_watcher(event_queue, rhndl);
            event_queue.pull_root();
            return r;
        }
        
        size_t num_queued_events() noexcept
        {
            return event_queue.size();
        }

        // Queue a watcher for removal, or issue "removed" callback to it.
        // Call with lock free.
        void issue_delete(base_watcher *watcher) noexcept
        {
            // This is only called when the attention lock is held, so if the watcher is not
            // active/queued now, it cannot become active (and will not be reported with an event)
            // during execution of this function.
            
            lock.lock();
            
            if (watcher->active) {
                // If the watcher is active, set deleteme true; the watcher will be removed
                // at the end of current processing (i.e. when active is set false).
                watcher->deleteme = true;
                lock.unlock();
            }
            else {
                // Actually do the delete.
                dequeue_watcher(watcher);
                release_watcher(watcher);
                
                lock.unlock();
                watcher->watch_removed();
            }
        }
        
        // Queue a watcher for removal, or issue "removed" callback to it.
        // Call with lock free.
        void issue_delete(base_bidi_fd_watcher *watcher) noexcept
        {
            lock.lock();
            
            if (watcher->active) {
                watcher->deleteme = true;
                release_watcher(watcher);
            }
            else {
                dequeue_watcher(watcher);
                release_watcher(watcher);
                watcher->read_removed = true;
            }
            
            base_watcher *secondary = &(watcher->out_watcher);
            if (secondary->active) {
                secondary->deleteme = true;
                release_watcher(watcher);
            }
            else {
                dequeue_watcher(secondary);
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

        event_dispatch() {  }
        event_dispatch(const event_dispatch &) = delete;
    };

} // namespace dasynq

// This is the main event_loop implementation. It serves as an interface to the event loop backend (of which
// it maintains an internal instance). It also serialises polling the backend and provides safe deletion of
// watchers (see comments inline).
//
// The T_Mutex type parameter specifies the mutex type. A null_mutex can be used for a single-threaded event
// loop; std::mutex, or any mutex providing a compatible interface, can be used for a thread-safe event
// loop.
//
// The Traits type parameter specifies any required traits for the event loop.  This specifies the back-end
// to use (backend_t, a template) and the basic back-end traits (backend_traits_t).
// The default is `default_traits<T_Mutex>'.
//
template <typename T_Mutex, typename Traits>
class event_loop
{
    using my_event_loop_t = event_loop<T_Mutex, Traits>;

    friend class dprivate::fd_watcher<my_event_loop_t>;
    friend class dprivate::bidi_fd_watcher<my_event_loop_t>;
    friend class dprivate::signal_watcher<my_event_loop_t>;
    friend class dprivate::child_proc_watcher<my_event_loop_t>;
    friend class dprivate::timer<my_event_loop_t>;
    
    friend class dprivate::loop_access;

    using backend_traits_t = typename Traits::backend_traits_t;

    template <typename T> using event_dispatch = dprivate::event_dispatch<T,Traits>;
    using dispatch_t = event_dispatch<backend_traits_t>;
    using loop_mech_t = typename Traits::template backend_t<dispatch_t>;
    using reaper_mutex_t = typename loop_mech_t::reaper_mutex_t;

    public:
    using traits_t = Traits;
    using loop_traits_t = typename loop_mech_t::traits_t;
    using mutex_t = T_Mutex;
    
    private:
    template <typename T> using waitqueue = dprivate::waitqueue<T>;
    template <typename T> using waitqueue_node = dprivate::waitqueue_node<T>;
    using base_watcher = dprivate::base_watcher;
    using base_signal_watcher = dprivate::base_signal_watcher<typename loop_traits_t::sigdata_t>;
    using base_fd_watcher = dprivate::base_fd_watcher;
    using base_bidi_fd_watcher = dprivate::base_bidi_fd_watcher;
    using base_child_watcher = dprivate::base_child_watcher;
    using base_timer_watcher = dprivate::base_timer_watcher;
    using watch_type_t = dprivate::watch_type_t;

    loop_mech_t loop_mech;

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
    // that wish to actually poll for events, while they are waiting for the main
    // queue to become quiet.
    // - The head of the "attn_waitqueue" is always the holder of the lock
    // - Therefore, a poll-waiter must be moved from the wait_waitqueue to the
    //   attn_waitqueue to actually gain the lock. This is only done if the
    //   attn_waitqueue is otherwise empty.
    // - The mutex only protects manipulation of the wait queues, and so should not
    //   be highly contended.
    //
    // To claim the lock for a poll-wait, the procedure is:
    //    - check if the attn_waitqueue is empty;
    //    - if it is, insert node at the head, thus claiming the lock, and return
    //    - otherwise, insert node in the wait_waitqueue, and wait
    // To claim the lock for an unwatch, the procedure is:
    //    - insert node in the attn_waitqueue
    //    - if the node is at the head of the queue, lock is claimed; return
    //    - otherwise, if a poll is in progress, interrupt it
    //    - wait until our node is at the head of the attn_waitqueue
    
    mutex_t wait_lock;  // protects the wait/attention queues
    bool long_poll_running = false;  // whether any thread is polling the backend (with non-zero timeout)
    waitqueue<mutex_t> attn_waitqueue;
    waitqueue<mutex_t> wait_waitqueue;
    
    mutex_t &get_base_lock() noexcept
    {
        return loop_mech.lock;
    }
    
    reaper_mutex_t &get_reaper_lock() noexcept
    {
        return loop_mech.get_reaper_lock();
    }

    void register_signal(base_signal_watcher *callBack, int signo)
    {
        std::lock_guard<mutex_t> guard(loop_mech.lock);

        loop_mech.prepare_watcher(callBack);
        try {
            loop_mech.add_signal_watch_nolock(signo, callBack);
            if (backend_traits_t::interrupt_after_signal_add) {
                interrupt_if_necessary();
            }
        }
        catch (...) {
            loop_mech.release_watcher(callBack);
            throw;
        }
    }
    
    void deregister(base_signal_watcher *callBack, int signo) noexcept
    {
        loop_mech.remove_signal_watch(signo);
        
        waitqueue_node<T_Mutex> qnode;
        get_attn_lock(qnode);
        
        loop_mech.issue_delete(callBack);
        
        release_lock(qnode);
    }

    void register_fd(base_fd_watcher *callback, int fd, int eventmask, bool enabled, bool emulate = false)
    {
        std::lock_guard<mutex_t> guard(loop_mech.lock);

        loop_mech.prepare_watcher(callback);

        try {
            if (! loop_mech.add_fd_watch(fd, callback, eventmask | ONE_SHOT, enabled, emulate)) {
                callback->emulatefd = true;
                callback->emulate_enabled = enabled;
                if (enabled) {
                    callback->event_flags = eventmask & IO_EVENTS;
                    if (eventmask & IO_EVENTS) {
                        requeue_watcher(callback);
                    }
                }
            }
            else if (enabled && backend_traits_t::interrupt_after_fd_add) {
                interrupt_if_necessary();
            }
        }
        catch (...) {
            loop_mech.release_watcher(callback);
            throw;
        }
    }
    
    // Register a bidi fd watcher. The watch_flags should already be set to the eventmask to watch
    // (i.e. eventmask == callback->watch_flags is a pre-condition).
    void register_fd(base_bidi_fd_watcher *callback, int fd, int eventmask, bool emulate = false)
    {
        std::lock_guard<mutex_t> guard(loop_mech.lock);

        loop_mech.prepare_watcher(callback);
        try {
            loop_mech.prepare_watcher(&callback->out_watcher);
            try {
                bool do_interrupt = false;
                if (backend_traits_t::has_separate_rw_fd_watches) {
                    int r = loop_mech.add_bidi_fd_watch(fd, callback, eventmask | ONE_SHOT, emulate);
                    if (r & IN_EVENTS) {
                        callback->emulatefd = true;
                        if (eventmask & IN_EVENTS) {
                            callback->watch_flags &= ~IN_EVENTS;
                            requeue_watcher(callback);
                        }
                    }
                    else if ((eventmask & IN_EVENTS) && backend_traits_t::interrupt_after_fd_add) {
                        do_interrupt = true;
                    }

                    if (r & OUT_EVENTS) {
                        callback->out_watcher.emulatefd = true;
                        if (eventmask & OUT_EVENTS) {
                            callback->watch_flags &= ~OUT_EVENTS;
                            requeue_watcher(&callback->out_watcher);
                        }
                    }
                    else if ((eventmask & OUT_EVENTS) && backend_traits_t::interrupt_after_fd_add) {
                        do_interrupt = true;
                    }
                }
                else {
                    if (! loop_mech.add_fd_watch(fd, callback, eventmask | ONE_SHOT, true, emulate)) {
                        callback->emulatefd = true;
                        callback->out_watcher.emulatefd = true;
                        if (eventmask & IN_EVENTS) {
                            callback->watch_flags &= ~IN_EVENTS;
                            requeue_watcher(callback);
                        }
                        if (eventmask & OUT_EVENTS) {
                            callback->watch_flags &= ~OUT_EVENTS;
                            requeue_watcher(&callback->out_watcher);
                        }
                    }
                    else if (backend_traits_t::interrupt_after_fd_add) {
                        do_interrupt = true;
                    }
                }

                if (do_interrupt) {
                    interrupt_if_necessary();
                }
            }
            catch (...) {
                loop_mech.release_watcher(&callback->out_watcher);
                throw;
            }
        }
        catch (...) {
            loop_mech.release_watcher(callback);
            throw;
        }
    }
    
    void set_fd_enabled(base_watcher *watcher, int fd, int watch_flags, bool enabled) noexcept
    {
        if (enabled) {
            loop_mech.enable_fd_watch(fd, watcher, watch_flags | ONE_SHOT);
            if (backend_traits_t::interrupt_after_fd_add) {
                interrupt_if_necessary();
            }
        }
        else {
            loop_mech.disable_fd_watch(fd, watch_flags);
        }
    }

    void set_fd_enabled_nolock(base_watcher *watcher, int fd, int watch_flags, bool enabled) noexcept
    {
        if (enabled) {
            loop_mech.enable_fd_watch_nolock(fd, watcher, watch_flags | ONE_SHOT);
            if (backend_traits_t::interrupt_after_fd_add) {
                interrupt_if_necessary();
            }
        }
        else {
            loop_mech.disable_fd_watch_nolock(fd, watch_flags);
        }
    }
    
    void deregister(base_fd_watcher *callback, int fd) noexcept
    {
        if (callback->emulatefd) {
            auto & ed = (dispatch_t &) loop_mech;
            ed.issue_delete(callback);
            return;
        }
        
        loop_mech.remove_fd_watch(fd, callback->watch_flags);

        waitqueue_node<T_Mutex> qnode;
        get_attn_lock(qnode);
        
        auto & ed = (dispatch_t &) loop_mech;
        ed.issue_delete(callback);
        
        release_lock(qnode);        
    }
    
    void deregister(base_bidi_fd_watcher *callback, int fd) noexcept
    {
        if (backend_traits_t::has_separate_rw_fd_watches) {
            loop_mech.remove_bidi_fd_watch(fd);
        }
        else {
            loop_mech.remove_fd_watch(fd, callback->watch_flags);
        }
        
        waitqueue_node<T_Mutex> qnode;
        get_attn_lock(qnode);
        
        dispatch_t & ed = (dispatch_t &) loop_mech;
        ed.issue_delete(callback);
        
        release_lock(qnode);
    }
    
    void reserve_child_watch(base_child_watcher *callback)
    {
        std::lock_guard<mutex_t> guard(loop_mech.lock);

        loop_mech.prepare_watcher(callback);
        try {
            loop_mech.reserve_child_watch_nolock(callback->watch_handle);
        }
        catch (...) {
            loop_mech.release_watcher(callback);
            throw;
        }
    }
    
    void unreserve(base_child_watcher *callback) noexcept
    {
        std::lock_guard<mutex_t> guard(loop_mech.lock);

        loop_mech.unreserve_child_watch(callback->watch_handle);
        loop_mech.release_watcher(callback);
    }
    
    void register_child(base_child_watcher *callback, pid_t child)
    {
        std::lock_guard<mutex_t> guard(loop_mech.lock);
        
        loop_mech.prepare_watcher(callback);
        try {
            loop_mech.add_child_watch_nolock(callback->watch_handle, child, callback);
        }
        catch (...) {
            loop_mech.release_watcher(callback);
            throw;
        }
    }
    
    void register_reserved_child(base_child_watcher *callback, pid_t child) noexcept
    {
        loop_mech.add_reserved_child_watch(callback->watch_handle, child, callback);
    }

    void register_reserved_child_nolock(base_child_watcher *callback, pid_t child) noexcept
    {
        loop_mech.add_reserved_child_watch_nolock(callback->watch_handle, child, callback);
    }
    
    void deregister(base_child_watcher *callback, pid_t child) noexcept
    {
        loop_mech.remove_child_watch(callback->watch_handle);

        waitqueue_node<T_Mutex> qnode;
        get_attn_lock(qnode);
        
        loop_mech.issue_delete(callback);
        
        release_lock(qnode);
    }
    
    // Stop watching a child process, but retain watch reservation so that another child can be
    // watched without running into resource allocation issues.
    void stop_watch(base_child_watcher *callback) noexcept
    {
        loop_mech.stop_child_watch(callback->watch_handle);
    }

    void register_timer(base_timer_watcher *callback, clock_type clock)
    {
        std::lock_guard<mutex_t> guard(loop_mech.lock);
    
        loop_mech.prepare_watcher(callback);
        try {
            loop_mech.add_timer_nolock(callback->timer_handle, callback, clock);
        }
        catch (...) {
            loop_mech.release_watcher(callback);
        }
    }
    
    void set_timer(base_timer_watcher *callBack, const timespec &timeout, clock_type clock) noexcept
    {
        struct timespec interval {0, 0};
        loop_mech.set_timer(callBack->timer_handle, timeout, interval, true, clock);
    }
    
    void set_timer(base_timer_watcher *callBack, const timespec &timeout, const timespec &interval,
            clock_type clock) noexcept
    {
        loop_mech.set_timer(callBack->timer_handle, timeout, interval, true, clock);
    }

    void set_timer_rel(base_timer_watcher *callBack, const timespec &timeout, clock_type clock) noexcept
    {
        struct timespec interval {0, 0};
        loop_mech.set_timer_rel(callBack->timer_handle, timeout, interval, true, clock);
    }
    
    void set_timer_rel(base_timer_watcher *callBack, const timespec &timeout,
            const timespec &interval, clock_type clock) noexcept
    {
        loop_mech.set_timer_rel(callBack->timer_handle, timeout, interval, true, clock);
    }

    void set_timer_enabled(base_timer_watcher *callback, clock_type clock, bool enabled) noexcept
    {
        loop_mech.enable_timer(callback->timer_handle, enabled, clock);
    }

    void set_timer_enabled_nolock(base_timer_watcher *callback, clock_type clock, bool enabled) noexcept
    {
        loop_mech.enable_timer_nolock(callback->timer_handle, enabled, clock);
    }

    void stop_timer(base_timer_watcher *callback, clock_type clock) noexcept
    {
        loop_mech.stop_timer(callback->timer_handle, clock);
    }

    void deregister(base_timer_watcher *callback, clock_type clock) noexcept
    {
        loop_mech.remove_timer(callback->timer_handle, clock);
        
        waitqueue_node<T_Mutex> qnode;
        get_attn_lock(qnode);
        
        loop_mech.issue_delete(callback);
        
        release_lock(qnode);
    }
    
    void dequeue_watcher(base_watcher *watcher) noexcept
    {
        loop_mech.dequeue_watcher(watcher);
    }

    void requeue_watcher(base_watcher *watcher) noexcept
    {
        loop_mech.queue_watcher(watcher);
        interrupt_if_necessary();
    }

    void release_watcher(base_watcher *watcher) noexcept
    {
        loop_mech.release_watcher(watcher);
    }

    // Interrupt the current poll-waiter, if necessary - that is, if the loop is multi-thread safe, and if
    // there is currently another thread polling the backend event mechanism.
    void interrupt_if_necessary()
    {
        wait_lock.lock();
        bool attn_q_empty = attn_waitqueue.is_empty(); // (always false for single-threaded loops)
        wait_lock.unlock();

        if (! attn_q_empty) {
            loop_mech.interrupt_wait();
        }
    }

    // Acquire the attention lock (when held, ensures that no thread is polling the AEN
    // mechanism). This can be used to safely remove watches, since it is certain that
    // notification callbacks won't be run while the attention lock is held. Any in-progress
    // poll will be interrupted so that the lock should be acquired quickly.
    void get_attn_lock(waitqueue_node<T_Mutex> &qnode) noexcept
    {
        std::unique_lock<T_Mutex> ulock(wait_lock);
        attn_waitqueue.queue(&qnode);        
        if (! attn_waitqueue.check_head(qnode)) {
            if (long_poll_running) {
                // We want to interrupt any in-progress poll so that the attn queue will progress
                // but we don't want to do that unnecessarily. If we are 2nd in the queue then the
                // head must be doing the poll; interrupt it. Otherwise, we assume the 2nd has
                // already interrupted it.
                if (attn_waitqueue.get_second() == &qnode) {
                    loop_mech.interrupt_wait();
                }
            }
            while (! attn_waitqueue.check_head(qnode)) {
                qnode.wait(ulock);
            }
        }
    }
    
    // Acquire the attention lock, but without interrupting any poll that's in progress
    // (prefer to fail in that case).
    bool poll_attn_lock(waitqueue_node<T_Mutex> &qnode) noexcept
    {
        std::unique_lock<T_Mutex> ulock(wait_lock);
        if (long_poll_running) {
            // There are poll-waiters, bail out
            return false;
        }

        // Nobody's doing a long poll, wait until we're at the head of the attn queue and return
        // success:
        attn_waitqueue.queue(&qnode);
        while (! attn_waitqueue.check_head(qnode)) {
            qnode.wait(ulock);
        }

        return true;
    }

    // Acquire the poll-wait lock (to be held when polling the AEN mechanism; lower priority than
    // the attention lock). The poll-wait lock is used to prevent more than a single thread from
    // polling the event loop mechanism at a time; if this is not done, it is basically
    // impossible to safely deregister watches.
    void get_pollwait_lock(waitqueue_node<T_Mutex> &qnode) noexcept
    {
        std::unique_lock<T_Mutex> ulock(wait_lock);
        if (attn_waitqueue.is_empty()) {
            // Queue is completely empty:
            attn_waitqueue.queue(&qnode);
        }
        else {
            wait_waitqueue.queue(&qnode);
        }
        
        while (! attn_waitqueue.check_head(qnode)) {
            qnode.wait(ulock);
        }

        long_poll_running = true;
    }
    
    // Release the poll-wait/attention lock.
    void release_lock(waitqueue_node<T_Mutex> &qnode) noexcept
    {
        std::unique_lock<T_Mutex> ulock(wait_lock);
        long_poll_running = false;
        waitqueue_node<T_Mutex> * nhead = attn_waitqueue.unqueue();
        if (nhead != nullptr) {
            // Someone else now owns the lock, signal them to wake them up
            nhead->signal();
        }
        else {
            // Nobody is waiting in attn_waitqueue (the high-priority queue) so check in
            // wait_waitqueue (the low-priority queue)
            if (! wait_waitqueue.is_empty()) {
                auto nhead = wait_waitqueue.get_head();
                wait_waitqueue.unqueue();
                attn_waitqueue.queue(nhead);
                long_poll_running = true;
                nhead->signal();
            }
        }                
    }
    
    void process_signal_rearm(base_signal_watcher * bsw, rearm rearm_type) noexcept
    {
        // Called with lock held
        if (rearm_type == rearm::REARM) {
            loop_mech.rearm_signal_watch_nolock(bsw->siginfo.get_signo(), bsw);
            if (backend_traits_t::interrupt_after_signal_add) {
                interrupt_if_necessary();
            }
        }
        else if (rearm_type == rearm::REMOVE) {
            loop_mech.remove_signal_watch_nolock(bsw->siginfo.get_signo());
        }
        // Note that signal watchers cannot (currently) be disarmed
    }

    // Process rearm return from an fd_watcher, including the primary watcher of a bidi_fd_watcher.
    // Depending on the rearm value, we re-arm, remove, or disarm the watcher, etc.
    rearm process_fd_rearm(base_fd_watcher * bfw, rearm rearm_type) noexcept
    {
        bool emulatedfd = static_cast<base_watcher *>(bfw)->emulatefd;

        if (emulatedfd) {
            if (rearm_type == rearm::REARM) {
                bfw->emulate_enabled = true;
                rearm_type = rearm::REQUEUE;
            }
            else if (rearm_type == rearm::DISARM) {
                bfw->emulate_enabled = false;
            }
            else if (rearm_type == rearm::NOOP) {
                if (bfw->emulate_enabled) {
                    rearm_type = rearm::REQUEUE;
                }
            }
        }
        else  if (rearm_type == rearm::REARM) {
            set_fd_enabled_nolock(bfw, bfw->watch_fd,
                    bfw->watch_flags & (IN_EVENTS | OUT_EVENTS), true);
        }
        else if (rearm_type == rearm::DISARM) {
            loop_mech.disable_fd_watch_nolock(bfw->watch_fd, bfw->watch_flags);
        }
        else if (rearm_type == rearm::REMOVE) {
            loop_mech.remove_fd_watch_nolock(bfw->watch_fd, bfw->watch_flags);
        }
        return rearm_type;
    }

    // Process rearm option from the primary watcher in bidi_fd_watcher
    rearm process_primary_rearm(base_bidi_fd_watcher * bdfw, rearm rearm_type) noexcept
    {
        bool emulatedfd = static_cast<base_watcher *>(bdfw)->emulatefd;

        // Called with lock held
        if (rearm_type == rearm::REMOVE) {
            bdfw->read_removed = 1;

            if (backend_traits_t::has_separate_rw_fd_watches) {
                bdfw->watch_flags &= ~IN_EVENTS;
                if (! emulatedfd) {
                    loop_mech.remove_fd_watch_nolock(bdfw->watch_fd, IN_EVENTS);
                }
                return bdfw->write_removed ? rearm::REMOVE : rearm::NOOP;
            }
            else {
                if (! bdfw->write_removed) {
                    if (bdfw->watch_flags & IN_EVENTS) {
                        bdfw->watch_flags &= ~IN_EVENTS;
                        if (! emulatedfd) {
                            set_fd_enabled_nolock(bdfw, bdfw->watch_fd, bdfw->watch_flags,
                                    bdfw->watch_flags != 0);
                        }
                    }
                    return rearm::NOOP;
                }
                else {
                    // both removed: actually remove
                    if (! emulatedfd) {
                        loop_mech.remove_fd_watch_nolock(bdfw->watch_fd, 0 /* not used */);
                    }
                    return rearm::REMOVE;
                }
            }
        }
        else if (rearm_type == rearm::DISARM) {
            bdfw->watch_flags &= ~IN_EVENTS;

            if (! emulatedfd) {
                if (! backend_traits_t::has_separate_rw_fd_watches) {
                    int watch_flags = bdfw->watch_flags  & (IN_EVENTS | OUT_EVENTS);
                    set_fd_enabled_nolock(bdfw, bdfw->watch_fd, watch_flags, watch_flags != 0);
                }
                else {
                    loop_mech.disable_fd_watch_nolock(bdfw->watch_fd, IN_EVENTS);
                }
            }
        }
        else if (rearm_type == rearm::REARM) {
            if (! emulatedfd) {
                bdfw->watch_flags |= IN_EVENTS;
                if (! backend_traits_t::has_separate_rw_fd_watches) {
                    int watch_flags = bdfw->watch_flags;
                    set_fd_enabled_nolock(bdfw, bdfw->watch_fd,
                            watch_flags & (IN_EVENTS | OUT_EVENTS), true);
                }
                else {
                    set_fd_enabled_nolock(bdfw, bdfw->watch_fd, IN_EVENTS, true);
                }
            }
            else {
                bdfw->watch_flags &= ~IN_EVENTS;
                rearm_type = rearm::REQUEUE;
            }
        }
        else if (rearm_type == rearm::NOOP) {
            if (bdfw->emulatefd) {
                if (bdfw->watch_flags & IN_EVENTS) {
                    bdfw->watch_flags &= ~IN_EVENTS;
                    rearm_type = rearm::REQUEUE;
                }
            }
        }
        return rearm_type;
    }

    // Process re-arm for the secondary (output) watcher in a Bi-direction Fd watcher.
    rearm process_secondary_rearm(base_bidi_fd_watcher * bdfw, base_watcher * outw, rearm rearm_type) noexcept
    {
        bool emulatedfd = outw->emulatefd;

        // Called with lock held
        if (emulatedfd) {
            if (rearm_type == rearm::REMOVE) {
                bdfw->write_removed = 1;
                bdfw->watch_flags &= ~OUT_EVENTS;
                rearm_type = bdfw->read_removed ? rearm::REMOVE : rearm::NOOP;
            }
            else if (rearm_type == rearm::DISARM) {
                bdfw->watch_flags &= ~OUT_EVENTS;
            }
            else if (rearm_type == rearm::REARM) {
                bdfw->watch_flags &= ~OUT_EVENTS;
                rearm_type = rearm::REQUEUE;
            }
            else if (rearm_type == rearm::NOOP) {
                if (bdfw->watch_flags & OUT_EVENTS) {
                    bdfw->watch_flags &= ~OUT_EVENTS;
                    rearm_type = rearm::REQUEUE;
                }
            }
            return rearm_type;
        }
        else if (rearm_type == rearm::REMOVE) {
            bdfw->write_removed = 1;

            if (backend_traits_t::has_separate_rw_fd_watches) {
                bdfw->watch_flags &= ~OUT_EVENTS;
                loop_mech.remove_fd_watch_nolock(bdfw->watch_fd, OUT_EVENTS);
                return bdfw->read_removed ? rearm::REMOVE : rearm::NOOP;
            }
            else {
                if (! bdfw->read_removed) {
                    if (bdfw->watch_flags & OUT_EVENTS) {
                        bdfw->watch_flags &= ~OUT_EVENTS;
                        set_fd_enabled_nolock(bdfw, bdfw->watch_fd, bdfw->watch_flags, true);
                    }
                    return rearm::NOOP;
                }
                else {
                    // both removed: actually remove
                    loop_mech.remove_fd_watch_nolock(bdfw->watch_fd, 0 /* not used */);
                    return rearm::REMOVE;
                }
            }
        }
        else if (rearm_type == rearm::DISARM) {
            bdfw->watch_flags &= ~OUT_EVENTS;

            if (! backend_traits_t::has_separate_rw_fd_watches) {
                int watch_flags = bdfw->watch_flags;
                set_fd_enabled_nolock(bdfw, bdfw->watch_fd, watch_flags & (IN_EVENTS | OUT_EVENTS), true);
            }
            else {
                loop_mech.disable_fd_watch_nolock(bdfw->watch_fd, OUT_EVENTS);
            }
        }
        else if (rearm_type == rearm::REARM) {
            bdfw->watch_flags |= OUT_EVENTS;
            
            if (! backend_traits_t::has_separate_rw_fd_watches) {
                int watch_flags = bdfw->watch_flags;
                set_fd_enabled_nolock(bdfw, bdfw->watch_fd, watch_flags & (IN_EVENTS | OUT_EVENTS), true);
            }
            else {
                set_fd_enabled_nolock(bdfw, bdfw->watch_fd, OUT_EVENTS | ONE_SHOT, true);
            }
        }
        return rearm_type;
    }
    
    void process_child_watch_rearm(base_child_watcher *bcw, rearm rearm_type) noexcept
    {
        if (rearm_type == rearm::REMOVE || rearm_type == rearm::DISARM) {
            loop_mech.unreserve_child_watch_nolock(bcw->watch_handle);
        }
    }

    void process_timer_rearm(base_timer_watcher *btw, rearm rearm_type) noexcept
    {
        // Called with lock held
        if (rearm_type == rearm::REARM) {
            loop_mech.enable_timer_nolock(btw->timer_handle, true, btw->clock);
        }
        else if (rearm_type == rearm::REMOVE) {
            loop_mech.remove_timer_nolock(btw->timer_handle, btw->clock);
        }
        else if (rearm_type == rearm::DISARM) {
            loop_mech.enable_timer_nolock(btw->timer_handle, false, btw->clock);
        }
    }

    // Process queued events; returns true if any events were processed.
    //   limit - maximum number of events to process before returning; -1 for
    //           no limit.
    bool process_events(int limit) noexcept
    {
        loop_mech.lock.lock();
        
        if (limit == 0) {
            return false;
        }
        
        // limit processing to the number of events currently queued, to avoid prolonged processing
        // of watchers which requeueu themselves immediately (including file watchers which are using
        // emulation for watching regular files)
        //
        // If limit is -1 (no limit) we rely on this being always larger than/equal to the number of
        // queued events when cast to size_t (which is unsigned).
        limit = std::min(size_t(limit), loop_mech.num_queued_events());

        base_watcher * pqueue = loop_mech.pull_queued_event();
        bool active = false;
        
        while (pqueue != nullptr) {
        
            pqueue->active = true;
            active = true;
            
            base_bidi_fd_watcher *bbfw = nullptr;
            
            // (Above variables are initialised only to silence compiler warnings).
            
            if (pqueue->watchType == watch_type_t::SECONDARYFD) {
                // construct a pointer to the main watcher, using integer arithmetic to avoid undefined
                // pointer arithmetic:
                uintptr_t rp = (uintptr_t)pqueue;

                // Here we take the offset of a member from a non-standard-layout class, which is
                // specified to have undefined result by the C++ language standard, but which
                // in practice works fine:
                _Pragma ("GCC diagnostic push")
                _Pragma ("GCC diagnostic ignored \"-Winvalid-offsetof\"")
                rp -= offsetof(base_bidi_fd_watcher, out_watcher);
                _Pragma ("GCC diagnostic pop")
                bbfw = (base_bidi_fd_watcher *)rp;

                // issue a secondary dispatch:
                bbfw->dispatch_second(this);
            }
            else {
                pqueue->dispatch(this);
            }

            if (limit > 0) {
                limit--;
                if (limit == 0) break;
            }
            pqueue = loop_mech.pull_queued_event();
        }
        
        loop_mech.lock.unlock();
        return active;
    }

    public:
    
    using fd_watcher = dprivate::fd_watcher<my_event_loop_t>;
    using bidi_fd_watcher = dprivate::bidi_fd_watcher<my_event_loop_t>;
    using signal_watcher = dprivate::signal_watcher<my_event_loop_t>;
    using child_proc_watcher = dprivate::child_proc_watcher<my_event_loop_t>;
    using timer = dprivate::timer<my_event_loop_t>;
    
    template <typename D> using fd_watcher_impl = dprivate::fd_watcher_impl<my_event_loop_t, D>;
    template <typename D> using bidi_fd_watcher_impl = dprivate::bidi_fd_watcher_impl<my_event_loop_t, D>;
    template <typename D> using signal_watcher_impl = dprivate::signal_watcher_impl<my_event_loop_t, D>;
    template <typename D> using child_proc_watcher_impl = dprivate::child_proc_watcher_impl<my_event_loop_t, D>;
    template <typename D> using timer_impl = dprivate::timer_impl<my_event_loop_t, D>;

    // Poll the event loop and process any pending events (up to a limit). If no events are pending, wait
    // for and process at least one event.
    void run(int limit = -1) noexcept
    {
        // Poll the mechanism first, in case high-priority events are pending:
        waitqueue_node<T_Mutex> qnode;
        get_pollwait_lock(qnode);
        loop_mech.pull_events(false);
        release_lock(qnode);

        while (! process_events(limit)) {
            // Pull events from the AEN mechanism and insert them in our internal queue:
            get_pollwait_lock(qnode);
            loop_mech.pull_events(true);
            release_lock(qnode);
        }
    }

    // Poll the event loop and process any pending events (up to a limit).
    void poll(int limit = -1) noexcept
    {
        waitqueue_node<T_Mutex> qnode;
        if (poll_attn_lock(qnode)) {
            loop_mech.pull_events(false);
            release_lock(qnode);
        }

        process_events(limit);
    }

    // Get the current time corresponding to a specific clock.
    //   ts - the timespec variable to receive the time
    //   clock - specifies the clock
    //   force_update (default = false) - if true, the time returned will be updated from
    //       the system rather than being a previously cached result. It may be more
    //       accurate, but note that reading from a system clock may be relatively expensive.
    void get_time(timespec &ts, clock_type clock, bool force_update = false) noexcept
    {
        loop_mech.get_time(ts, clock, force_update);
    }

    void get_time(time_val &tv, clock_type clock, bool force_update = false) noexcept
    {
        loop_mech.get_time(tv, clock, force_update);
    }

    event_loop() { }
    event_loop(delayed_init d) noexcept : loop_mech(d) { }
    event_loop(const event_loop &other) = delete;

    // Perform delayed initialisation, if constructed with delayed_init
    void init()
    {
        loop_mech.init();
    }
};

typedef event_loop<null_mutex> event_loop_n;
typedef event_loop<std::mutex> event_loop_th;

namespace dprivate {

// Posix signal event watcher
template <typename EventLoop>
class signal_watcher : private dprivate::base_signal_watcher<typename EventLoop::loop_traits_t::sigdata_t>
{
    template <typename, typename> friend class signal_watcher_impl;

    using base_watcher = dprivate::base_watcher;
    using T_Mutex = typename EventLoop::mutex_t;
    
    public:
    using event_loop_t = EventLoop;
    using siginfo_p = typename signal_watcher::siginfo_p;

    // Register this watcher to watch the specified signal.
    // If an attempt is made to register with more than one event loop at
    // a time, behaviour is undefined. The signal should be masked before
    // call.
    inline void add_watch(event_loop_t &eloop, int signo, int prio = DEFAULT_PRIORITY)
    {
        base_watcher::init();
        this->priority = prio;
        this->siginfo.set_signo(signo);
        eloop.register_signal(this, signo);
    }
    
    inline void deregister(event_loop_t &eloop) noexcept
    {
        eloop.deregister(this, this->siginfo.get_signo());
    }
    
    template <typename T>
    static signal_watcher<event_loop_t> *add_watch(event_loop_t &eloop, int signo, T watch_hndlr)
    {
        class lambda_sig_watcher : public signal_watcher_impl<event_loop_t, lambda_sig_watcher>
        {
            private:
            T watch_hndlr;

            public:
            lambda_sig_watcher(T watch_handlr_a) : watch_hndlr(watch_handlr_a)
            {
                //
            }

            rearm received(event_loop_t &eloop, int signo, siginfo_p siginfo)
            {
                return watch_hndlr(eloop, signo, siginfo);
            }

            void watch_removed() noexcept override
            {
                delete this;
            }
        };

        lambda_sig_watcher * lsw = new lambda_sig_watcher(watch_hndlr);
        lsw->add_watch(eloop, signo);
        return lsw;
    }

    // virtual rearm received(EventLoop &eloop, int signo, siginfo_p siginfo) = 0;
};

template <typename EventLoop, typename Derived>
class signal_watcher_impl : public signal_watcher<EventLoop>
{
    void dispatch(void *loop_ptr) noexcept override
    {
        EventLoop &loop = *static_cast<EventLoop *>(loop_ptr);
        loop_access::get_base_lock(loop).unlock();

        auto rearm_type = static_cast<Derived *>(this)->received(loop, this->siginfo.get_signo(), this->siginfo);

        loop_access::get_base_lock(loop).lock();

        if (rearm_type != rearm::REMOVED) {

            this->active = false;
            if (this->deleteme) {
                // We don't want a watch that is marked "deleteme" to re-arm itself.
                rearm_type = rearm::REMOVE;
            }

            loop_access::process_signal_rearm(loop, this, rearm_type);

            post_dispatch(loop, this, rearm_type);
        }
    }
};

// Posix file descriptor event watcher
template <typename EventLoop>
class fd_watcher : private dprivate::base_fd_watcher
{
    template <typename, typename> friend class fd_watcher_impl;

    using base_watcher = dprivate::base_watcher;
    using mutex_t = typename EventLoop::mutex_t;

    protected:
    
    // Set the types of event to watch. Only supported if loop_traits_t_t::has_bidi_fd_watch
    // is true; otherwise has unspecified behavior.
    // Only safe to call from within the callback handler (fdEvent). Might not take
    // effect until the current callback handler returns with REARM.
    void set_watch_flags(int newFlags)
    {
        this->watch_flags = newFlags;
    }
    
    public:
    
    using event_loop_t = EventLoop;

    // Register a file descriptor watcher with an event loop. Flags
    // can be any combination of dasynq::IN_EVENTS / dasynq::OUT_EVENTS.
    // Exactly one of IN_EVENTS/OUT_EVENTS must be specified if the event
    // loop does not support bi-directional fd watchers (i.e. if
    // ! loop_traits_t::has_bidi_fd_watch).
    //
    // Mechanisms supporting dual watchers allow for two watchers for a
    // single file descriptor (one watching read status and the other
    // write status). Others mechanisms support only a single watcher
    // per file descriptor. Adding a watcher beyond what is supported
    // causes undefined behavior.
    //
    // Can fail with std::bad_alloc or std::system_error.
    void add_watch(event_loop_t &eloop, int fd, int flags, bool enabled = true, int prio = DEFAULT_PRIORITY)
    {
        base_watcher::init();
        this->priority = prio;
        this->watch_fd = fd;
        this->watch_flags = flags;
        eloop.register_fd(this, fd, flags, enabled, true);
    }

    void add_watch_noemu(event_loop_t &eloop, int fd, int flags, bool enabled = true, int prio = DEFAULT_PRIORITY)
    {
        base_watcher::init();
        this->priority = prio;
        this->watch_fd = fd;
        this->watch_flags = flags;
        eloop.register_fd(this, fd, flags, enabled, false);
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
    void deregister(event_loop_t &eloop) noexcept
    {
        eloop.deregister(this, this->watch_fd);
    }
    
    void set_enabled(event_loop_t &eloop, bool enable) noexcept
    {
        std::lock_guard<mutex_t> guard(eloop.get_base_lock());
        if (this->emulatefd) {
            if (enable && ! this->emulate_enabled) {
                loop_access::requeue_watcher(eloop, this);
            }
            this->emulate_enabled = enable;
        }
        else {
            eloop.set_fd_enabled_nolock(this, this->watch_fd, this->watch_flags, enable);
        }

        if (! enable) {
            eloop.dequeue_watcher(this);
        }
    }
    
    // Add an Fd watch via a lambda. The watch is allocated dynamically and destroys
    // itself when removed from the event loop.
    template <typename T>
    static fd_watcher<EventLoop> *add_watch(event_loop_t &eloop, int fd, int flags, T watchHndlr)
    {
        class lambda_fd_watcher : public fd_watcher_impl<event_loop_t, lambda_fd_watcher>
        {
            private:
            T watchHndlr;

            public:
            lambda_fd_watcher(T watchHandlr_a) : watchHndlr(watchHandlr_a)
            {
                //
            }

            rearm fd_event(event_loop_t &eloop, int fd, int flags)
            {
                return watchHndlr(eloop, fd, flags);
            }

            void watch_removed() noexcept override
            {
                delete this;
            }
        };
        
        lambda_fd_watcher * lfd = new lambda_fd_watcher(watchHndlr);
        lfd->add_watch(eloop, fd, flags);
        return lfd;
    }
    
    // virtual rearm fd_event(EventLoop &eloop, int fd, int flags) = 0;
};

template <typename EventLoop, typename Derived>
class fd_watcher_impl : public fd_watcher<EventLoop>
{
    void dispatch(void *loop_ptr) noexcept override
    {
        EventLoop &loop = *static_cast<EventLoop *>(loop_ptr);

        // In case emulating, clear enabled here; REARM or explicit set_enabled will re-enable.
        this->emulate_enabled = false;

        loop_access::get_base_lock(loop).unlock();

        auto rearm_type = static_cast<Derived *>(this)->fd_event(loop, this->watch_fd, this->event_flags);

        loop_access::get_base_lock(loop).lock();

        if (rearm_type != rearm::REMOVED) {
            this->event_flags = 0;
            this->active = false;
            if (this->deleteme) {
                // We don't want a watch that is marked "deleteme" to re-arm itself.
                rearm_type = rearm::REMOVE;
            }

            rearm_type = loop_access::process_fd_rearm(loop, this, rearm_type);

            post_dispatch(loop, this, rearm_type);
        }
    }
};


// A Bi-directional file descriptor watcher with independent read- and write- channels.
// This watcher type has two event notification methods which can both potentially be
// active at the same time.
template <typename EventLoop>
class bidi_fd_watcher : private dprivate::base_bidi_fd_watcher
{
    template <typename, typename> friend class bidi_fd_watcher_impl;

    using base_watcher = dprivate::base_watcher;
    using mutex_t = typename EventLoop::mutex_t;
    
    void set_watch_enabled(EventLoop &eloop, bool in, bool b)
    {
        int events = in ? IN_EVENTS : OUT_EVENTS;
        auto orig_flags = this->watch_flags;
        
        if (b) {
            this->watch_flags |= events;
        }
        else {
            this->watch_flags &= ~events;
        }

        dprivate::base_watcher * watcher = in ? this : &this->out_watcher;

        if (! watcher->emulatefd) {
            if (EventLoop::loop_traits_t::has_separate_rw_fd_watches) {
                eloop.set_fd_enabled_nolock(this, this->watch_fd, events | ONE_SHOT, b);
            }
            else {
                eloop.set_fd_enabled_nolock(this, this->watch_fd,
                        (this->watch_flags & IO_EVENTS) | ONE_SHOT,
                        (this->watch_flags & IO_EVENTS) != 0);
            }
        }
        else {
            // emulation: if enabling a previously disabled watcher, must queue now:
            if (b && (orig_flags != this->watch_flags)) {
                this->watch_flags = orig_flags;
                loop_access::requeue_watcher(eloop, watcher);
            }
        }

        if (! b) {
            eloop.dequeue_watcher(watcher);
        }
    }
    
    public:

    using event_loop_t = EventLoop;

    void set_in_watch_enabled(event_loop_t &eloop, bool b) noexcept
    {
        eloop.get_base_lock().lock();
        set_watch_enabled(eloop, true, b);
        eloop.get_base_lock().unlock();
    }
    
    void set_out_watch_enabled(event_loop_t &eloop, bool b) noexcept
    {
        eloop.get_base_lock().lock();
        set_watch_enabled(eloop, false, b);
        eloop.get_base_lock().unlock();
    }
    
    // Set the watch flags, which enables/disables both the in-watch and the out-watch accordingly.
    //
    // Concurrency: this method can only be called if
    //  - it does not enable a watcher that might currently be active
    ///   - unless the event loop will not be polled while the watcher is active.
    // (i.e. it is ok to call setWatchFlags from within the readReady/writeReady handlers if no other
    //  thread will poll the event loop; it is always ok to *dis*able a watcher that might be active,
    //  though the re-arm action returned by the callback may undo the effect).
    void set_watches(event_loop_t &eloop, int new_flags) noexcept
    {
        std::lock_guard<mutex_t> guard(eloop.get_base_lock());
        bool use_emulation = this->emulatefd || this->out_watcher.emulatefd;
        if (use_emulation || EventLoop::loop_traits_t::has_separate_rw_fd_watches) {
            set_watch_enabled(eloop, true, (new_flags & IN_EVENTS) != 0);
            set_watch_enabled(eloop, false, (new_flags & OUT_EVENTS) != 0);
        }
        else {
            this->watch_flags = (this->watch_flags & ~IO_EVENTS) | new_flags;
            eloop.set_fd_enabled_nolock((dprivate::base_watcher *) this, this->watch_fd, this->watch_flags & IO_EVENTS, true);
        }
    }
    
    // Register a bi-direction file descriptor watcher with an event loop. Flags
    // can be any combination of dasynq::IN_EVENTS / dasynq::OUT_EVENTS.
    //
    // Can fail with std::bad_alloc or std::system_error.
    void add_watch(event_loop_t &eloop, int fd, int flags, int inprio = DEFAULT_PRIORITY, int outprio = DEFAULT_PRIORITY)
    {
        base_watcher::init();
        this->out_watcher.base_watcher::init();
        this->watch_fd = fd;
        this->watch_flags = flags | dprivate::multi_watch;
        this->read_removed = false;
        this->write_removed = false;
        this->priority = inprio;
        this->set_priority(this->out_watcher, outprio);
        eloop.register_fd(this, fd, flags, true);
    }

    void add_watch_noemu(event_loop_t &eloop, int fd, int flags, int inprio = DEFAULT_PRIORITY, int outprio = DEFAULT_PRIORITY)
    {
        base_watcher::init();
        this->out_watcher.base_watcher::init();
        this->watch_fd = fd;
        this->watch_flags = flags | dprivate::multi_watch;
        this->read_removed = false;
        this->write_removed = false;
        this->priority = inprio;
        this->set_priority(this->out_watcher, outprio);
        eloop.register_fd(this, fd, flags, false);
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
    void deregister(event_loop_t &eloop) noexcept
    {
        eloop.deregister(this, this->watch_fd);
    }
    
    template <typename T>
    static bidi_fd_watcher<event_loop_t> *add_watch(event_loop_t &eloop, int fd, int flags, T watch_hndlr)
    {
        class lambda_bidi_watcher : public bidi_fd_watcher_impl<event_loop_t, lambda_bidi_watcher>
        {
            private:
            T watch_hndlr;

            public:
            lambda_bidi_watcher(T watch_handlr_a) : watch_hndlr(watch_handlr_a)
            {
                //
            }

            rearm read_ready(event_loop_t &eloop, int fd)
            {
                return watch_hndlr(eloop, fd, IN_EVENTS);
            }

            rearm write_ready(event_loop_t &eloop, int fd)
            {
                return watch_hndlr(eloop, fd, OUT_EVENTS);
            }

            void watch_removed() noexcept override
            {
                delete this;
            }
        };

        lambda_bidi_watcher * lfd = new lambda_bidi_watcher(watch_hndlr);
        lfd->add_watch(eloop, fd, flags);
        return lfd;
    }

    // virtual rearm read_ready(EventLoop &eloop, int fd) noexcept = 0;
    // virtual rearm write_ready(EventLoop &eloop, int fd) noexcept = 0;
};

template <typename EventLoop, typename Derived>
class bidi_fd_watcher_impl : public bidi_fd_watcher<EventLoop>
{
    void dispatch(void *loop_ptr) noexcept override
    {
        EventLoop &loop = *static_cast<EventLoop *>(loop_ptr);
        this->emulate_enabled = false;
        loop_access::get_base_lock(loop).unlock();

        auto rearm_type = static_cast<Derived *>(this)->read_ready(loop, this->watch_fd);

        loop_access::get_base_lock(loop).lock();

        if (rearm_type != rearm::REMOVED) {
            this->event_flags &= ~IN_EVENTS;
            this->active = false;
            if (this->deleteme) {
                // We don't want a watch that is marked "deleteme" to re-arm itself.
                rearm_type = rearm::REMOVE;
            }

            rearm_type = loop_access::process_primary_rearm(loop, this, rearm_type);

            auto &outwatcher = bidi_fd_watcher<EventLoop>::out_watcher;
            post_dispatch(loop, this, &outwatcher, rearm_type);
        }
    }

    void dispatch_second(void *loop_ptr) noexcept override
    {
        auto &outwatcher = bidi_fd_watcher<EventLoop>::out_watcher;

        EventLoop &loop = *static_cast<EventLoop *>(loop_ptr);
        loop_access::get_base_lock(loop).unlock();

        auto rearm_type = static_cast<Derived *>(this)->write_ready(loop, this->watch_fd);

        loop_access::get_base_lock(loop).lock();

        if (rearm_type != rearm::REMOVED) {
            this->event_flags &= ~OUT_EVENTS;
            outwatcher.active = false;
            if (outwatcher.deleteme) {
                // We don't want a watch that is marked "deleteme" to re-arm itself.
                rearm_type = rearm::REMOVE;
            }

            rearm_type = loop_access::process_secondary_rearm(loop, this, &outwatcher, rearm_type);

            if (rearm_type == rearm::REQUEUE) {
                post_dispatch(loop, &outwatcher, rearm_type);
            }
            else {
                post_dispatch(loop, this, &outwatcher, rearm_type);
            }
        }
    }
};

// Child process event watcher
template <typename EventLoop>
class child_proc_watcher : private dprivate::base_child_watcher
{
    template <typename, typename> friend class child_proc_watcher_impl;

    using base_watcher = dprivate::base_watcher;
    using mutex_t = typename EventLoop::mutex_t;

    public:

    using event_loop_t = EventLoop;

    // send a signal to this process, if it is still running, in a race-free manner.
    // return is as for POSIX kill(); return is -1 with errno=ESRCH if process has
    // already terminated.
    int send_signal(event_loop_t &loop, int signo) noexcept
    {
        auto reaper_mutex = loop.get_reaper_mutex();
        std::lock_guard<decltype(reaper_mutex)> guard(reaper_mutex);

        if (this->child_termd) {
            errno = ESRCH;
            return -1;
        }

        return kill(this->watch_pid, signo);
    }

    // Reserve resources for a child watcher with the given event loop.
    // Reservation can fail with std::bad_alloc. Some backends do not support
    // reservation (it will always fail) - check loop_traits_t::supports_childwatch_reservation.
    void reserve_watch(event_loop_t &eloop)
    {
        eloop.reserve_child_watch(this);
    }
    
    void unreserve(event_loop_t &eloop)
    {
        eloop.unreserve(this);
    }
    
    // Register a watcher for the given child process with an event loop.
    // Registration can fail with std::bad_alloc.
    // Note that in multi-threaded programs, use of this function may be prone to a
    // race condition such that the child terminates before the watcher is registered.
    void add_watch(event_loop_t &eloop, pid_t child, int prio = DEFAULT_PRIORITY)
    {
        base_watcher::init();
        this->watch_pid = child;
        this->priority = prio;
        eloop.register_child(this, child);
    }
    
    // Register a watcher for the given child process with an event loop,
    // after having reserved resources previously (using reserveWith).
    // Registration cannot fail.
    // Note that in multi-threaded programs, use of this function may be prone to a
    // race condition such that the child terminates before the watcher is registered;
    // use the "fork" member function to avoid this.
    void add_reserved(event_loop_t &eloop, pid_t child, int prio = DEFAULT_PRIORITY) noexcept
    {
        base_watcher::init();
        this->watch_pid = child;
        this->priority = prio;
        eloop.register_reserved_child(this, child);
    }
    
    void deregister(event_loop_t &eloop, pid_t child) noexcept
    {
        eloop.deregister(this, child);
    }
    
    // Stop watching the currently watched child, but retain watch reservation.
    void stop_watch(event_loop_t &eloop) noexcept
    {
        eloop.stop_watch(this);
    }

    // Fork and watch the child with this watcher on the given event loop.
    // If resource limitations prevent the child process from being watched, it is
    // terminated immediately (or if the implementation allows, never started),
    // and a suitable std::system_error or std::bad_alloc exception is thrown.
    // Returns:
    // - the child pid in the parent
    // - 0 in the child
    pid_t fork(event_loop_t &eloop, bool from_reserved = false, int prio = DEFAULT_PRIORITY)
    {
        base_watcher::init();
        this->priority = prio;

        if (EventLoop::loop_traits_t::supports_childwatch_reservation) {
            // Reserve a watch, fork, then claim reservation
            if (! from_reserved) {
                reserve_watch(eloop);
            }
            
            auto &lock = eloop.get_base_lock();
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
            this->watch_pid = child;
            eloop.register_reserved_child_nolock(this, child);
            lock.unlock();
            return child;
        }
        else {
            int pipefds[2];
            if (pipe2(pipefds, O_CLOEXEC) == -1) {
                throw std::system_error(errno, std::system_category());
            }
            
            std::lock_guard<mutex_t> guard(eloop.get_base_lock());
            
            pid_t child = ::fork();
            if (child == -1) {
                throw std::system_error(errno, std::system_category());
            }
            
            if (child == 0) {
                // I am the child
                close(pipefds[1]);
                
                // Wait for message from parent before continuing:
                int rr;
                int r = read(pipefds[0], &rr, sizeof(rr));
                while (r == -1 && errno == EINTR) {
                    r = read(pipefds[0], &rr, sizeof(rr));
                }
                
                if (r <= 0) _exit(0);
                
                close(pipefds[0]);
                return 0;
            }
            
            close(pipefds[0]); // close read end
            
            // Register this watcher.
            try {
                this->watch_pid = child;
                eloop.register_child(this, child);
                
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
    
    // virtual rearm child_status(EventLoop &eloop, pid_t child, int status) = 0;
};

template <typename EventLoop, typename Derived>
class child_proc_watcher_impl : public child_proc_watcher<EventLoop>
{
    void dispatch(void *loop_ptr) noexcept override
    {
        EventLoop &loop = *static_cast<EventLoop *>(loop_ptr);
        loop_access::get_base_lock(loop).unlock();

        auto rearm_type = static_cast<Derived *>(this)->status_change(loop, this->watch_pid, this->child_status);

        loop_access::get_base_lock(loop).lock();

        if (rearm_type != rearm::REMOVED) {

            this->active = false;
            if (this->deleteme) {
                // We don't want a watch that is marked "deleteme" to re-arm itself.
                rearm_type = rearm::REMOVE;
            }

            loop_access::process_child_watch_rearm(loop, this, rearm_type);

            // rearm_type = loop.process??;
            post_dispatch(loop, this, rearm_type);
        }
    }
};

template <typename EventLoop>
class timer : private base_timer_watcher
{
    template <typename, typename> friend class timer_impl;
    using base_t = base_timer_watcher;
    using mutex_t = typename EventLoop::mutex_t;

    public:
    using event_loop_t = EventLoop;
    
    void add_timer(event_loop_t &eloop, clock_type clock = clock_type::MONOTONIC, int prio = DEFAULT_PRIORITY)
    {
        base_watcher::init();
        this->priority = prio;
        this->clock = clock;
        this->intervals = 0;
        eloop.register_timer(this, clock);
    }
    
    void arm_timer(event_loop_t &eloop, const timespec &timeout) noexcept
    {
        eloop.set_timer(this, timeout, base_t::clock);
    }
    
    void arm_timer(event_loop_t &eloop, const timespec &timeout, const timespec &interval) noexcept
    {
        eloop.set_timer(this, timeout, interval, base_t::clock);
    }

    // Arm timer, relative to now:
    void arm_timer_rel(event_loop_t &eloop, const timespec &timeout) noexcept
    {
        eloop.set_timer_rel(this, timeout, base_t::clock);
    }
    
    void arm_timer_rel(event_loop_t &eloop, const timespec &timeout,
            const timespec &interval) noexcept
    {
        eloop.set_timer_rel(this, timeout, interval, base_t::clock);
    }
    
    void stop_timer(event_loop_t &eloop) noexcept
    {
        eloop.stop_timer(this, base_t::clock);
    }

    void set_enabled(event_loop_t &eloop, clock_type clock, bool enabled) noexcept
    {
        std::lock_guard<mutex_t> guard(eloop.get_base_lock());
        eloop.set_timer_enabled_nolock(this, clock, enabled);
        if (! enabled) {
            eloop.dequeue_watcher(this);
        }
    }

    void deregister(event_loop_t &eloop) noexcept
    {
        eloop.deregister(this, this->clock);
    }

    template <typename T>
    static timer<EventLoop> *add_timer(EventLoop &eloop, clock_type clock, bool relative,
            const timespec &timeout, const timespec &interval, T watch_hndlr)
    {
        class lambda_timer : public timer_impl<event_loop_t, lambda_timer>
        {
            private:
            T watch_hndlr;

            public:
            lambda_timer(T watch_handlr_a) : watch_hndlr(watch_handlr_a)
            {
                //
            }

            rearm timer_expiry(event_loop_t &eloop, int intervals)
            {
                return watch_hndlr(eloop, intervals);
            }

            void watch_removed() noexcept override
            {
                delete this;
            }
        };

        lambda_timer * lt = new lambda_timer(watch_hndlr);
        lt->add_timer(eloop, clock);
        if (relative) {
            lt->arm_timer_rel(eloop, timeout, interval);
        }
        else {
            lt->arm_timer(eloop, timeout, interval);
        }
        return lt;
    }

    // Timer expired, and the given number of intervals have elapsed before
    // expiry event was queued. Normally intervals == 1 to indicate no
    // overrun.
    // virtual rearm timer_expiry(event_loop_t &eloop, int intervals) = 0;
};

template <typename EventLoop, typename Derived>
class timer_impl : public timer<EventLoop>
{
    void dispatch(void *loop_ptr) noexcept override
    {
        EventLoop &loop = *static_cast<EventLoop *>(loop_ptr);
        loop_access::get_base_lock(loop).unlock();

        auto intervals_report = this->intervals;
        this->intervals = 0;
        auto rearm_type = static_cast<Derived *>(this)->timer_expiry(loop, intervals_report);

        loop_access::get_base_lock(loop).lock();

        if (rearm_type != rearm::REMOVED) {

            this->active = false;
            if (this->deleteme) {
                // We don't want a watch that is marked "deleteme" to re-arm itself.
                rearm_type = rearm::REMOVE;
            }

            loop_access::process_timer_rearm(loop, this, rearm_type);

            post_dispatch(loop, this, rearm_type);
        }
    }
};

} // namespace dprivate

} // namespace dasynq

#endif /* DASYNQ_H_ */
