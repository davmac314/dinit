// Dasynq: early declarations and base watchers.
//
// Here we define watcher functionality that is not dependent on the event loop type. In particular,
// base classes for the various watcher types. These are not part of the public API.
//
// In general access to the members of the basewatcher should be protected by a mutex. The
// event_dispatch lock is used for this purpose.

#include <type_traits>

namespace dasynq {

namespace dprivate {
    // POSIX says that sigprocmask has unspecified behaviour if used in a multi-threaded process. We can use
    // pthread_sigmask instead, but that may require linking with the threads library. This function is
    // specialised to call one or the other depending on the mutex type:
    template <typename T_Mutex> void sigmaskf(int how, const sigset_t *set, sigset_t *oset)
    {
        pthread_sigmask(how, set, oset);
    }

    template <> inline void sigmaskf<null_mutex>(int how, const sigset_t *set, sigset_t *oset)
    {
        sigprocmask(how, set, oset);
    }
}

// A template to generate suitable default loop traits for a given type of mutex:
template <typename T_Mutex> class default_traits
{
    public:
    using mutex_t  = T_Mutex;
    template <typename Base> using backend_t = dasynq::loop_t<Base>;
    using backend_traits_t = dasynq::loop_traits_t;

    // Alter the current thread signal mask using the correct function
    // (sigprocmask or pthread_sigmask):
    static void sigmaskf(int how, const sigset_t *set, sigset_t *oset)
    {
        dprivate::sigmaskf<T_Mutex>(how, set, oset);
    }
};

// Forward declarations:
template <typename T_Mutex, typename Traits = default_traits<T_Mutex>>
class event_loop;

inline namespace {
    constexpr int DEFAULT_PRIORITY = 50;
}

namespace dprivate {
    // (non-public API)

    class base_watcher;

    class empty_node
    {
        DASYNQ_EMPTY_BODY
    };

    // heap_def decides the queue implementation that we use. It must be stable:
    template <typename A, typename B, typename C> using dary_heap_def = dary_heap<A,B,C>;
    template <typename A, typename B> using heap_def = stable_heap<dary_heap_def,A,B>;

    namespace {
        // use empty handles (not containing basewatcher *) if the handles returned from the
        // queue are handle references, because we can derive a pointer to the containing basewatcher
        // via the address of the handle in that case:
        constexpr bool use_empty_node = std::is_same<
                typename heap_def<empty_node, int>::handle_t_r,
                typename heap_def<empty_node, int>::handle_t &>::value;

        using node_type = std::conditional<use_empty_node, empty_node, base_watcher *>::type;
    }

    using prio_queue = heap_def<node_type, int>;

    using prio_queue_emptynode = heap_def<empty_node, int>;
    using prio_queue_bwnode = heap_def<base_watcher *, int>;

    template <typename T_Loop> class fd_watcher;
    template <typename T_Loop> class bidi_fd_watcher;
    template <typename T_Loop> class signal_watcher;
    template <typename T_Loop> class child_proc_watcher;
    template <typename T_Loop> class timer;

    template <typename, typename> class fd_watcher_impl;
    template <typename, typename> class bidi_fd_watcher_impl;
    template <typename, typename> class signal_watcher_impl;
    template <typename, typename> class child_proc_watcher_impl;
    template <typename, typename> class timer_impl;

    enum class watch_type_t
    {
        SIGNAL,
        FD,
        CHILD,
        SECONDARYFD,
        TIMER
    };

    template <typename Traits, typename LoopTraits> class event_dispatch;

    // For FD watchers:
    // Use this watch flag to indicate that in and out events should be reported separately,
    // that is, watcher should not be disabled until all watched event types are queued.
    constexpr static int multi_watch = 4;

    // Represents a queued event notification. Various event watchers derive from this type.
    class base_watcher
    {
        public:
        watch_type_t watchType;
        int active : 1;    // currently executing handler?
        int deleteme : 1;  // delete when handler finished?
        int emulatefd : 1; // emulate file watch (by re-queueing)
        int emulate_enabled : 1;   // whether an emulated watch is enabled
        int child_termd : 1;  // child process has terminated

        prio_queue::handle_t heap_handle;
        int priority;

        static void set_priority(base_watcher &p, int prio)
        {
            p.priority = prio;
        }

        // Perform initialisation necessary before registration with an event loop
        void init()
        {
            active = false;
            deleteme = false;
            emulatefd = false;
            emulate_enabled = false;
            child_termd = false;
            prio_queue::init_handle(heap_handle);
            priority = DEFAULT_PRIORITY;
        }

        base_watcher(watch_type_t wt) noexcept : watchType(wt) { }
        base_watcher(const base_watcher &) = delete;
        base_watcher &operator=(const base_watcher &) = delete;

        // The dispatch function is called to process a watcher's callback. It is the "real" callback
        // function; it usually delegates to a user-provided callback.
        virtual void dispatch(void *loop_ptr) noexcept { };

        // Bi-directional file descriptor watches have a secondary dispatch function for the secondary
        // watcher (i.e. the output watcher):
        virtual void dispatch_second(void *loop_ptr) noexcept { }

        virtual ~base_watcher() noexcept { }

        // Called when the watcher has been removed.
        // It is guaranteed by the caller that:
        // - the dispatch method is not currently running
        // - the dispatch method will not be called.
        virtual void watch_removed() noexcept
        {
            // Later: the "delete" behaviour could be dependent on a flag, perhaps?
            // delete this;
        }
    };

    // Retrieve watcher from queue handle:
    inline base_watcher * get_watcher(prio_queue_emptynode &q, prio_queue_emptynode::handle_t &n)
    {
        uintptr_t bptr = (uintptr_t)&n;
        _Pragma ("GCC diagnostic push")
        _Pragma ("GCC diagnostic ignored \"-Winvalid-offsetof\"")
        bptr -= offsetof(base_watcher, heap_handle);
        _Pragma ("GCC diagnostic pop")
        return (base_watcher *)bptr;
    }

    inline dprivate::base_watcher * get_watcher(prio_queue_bwnode &q, prio_queue_bwnode::handle_t &n)
    {
        return q.node_data(n);
    }

    // Allocate queue handle:
    inline void allocate_handle(prio_queue_emptynode &q, prio_queue_emptynode::handle_t &n, base_watcher *bw)
    {
        q.allocate(n);
    }

    inline void allocate_handle(prio_queue_bwnode &q, prio_queue_bwnode::handle_t &n, base_watcher *bw)
    {
        q.allocate(n, bw);
    }

    // Base signal event - not part of public API
    template <typename T_Sigdata>
    class base_signal_watcher : public base_watcher
    {
        template <typename, typename> friend class event_dispatch;
        template <typename, typename> friend class dasynq::event_loop;

        protected:
        T_Sigdata siginfo;
        base_signal_watcher() : base_watcher(watch_type_t::SIGNAL) { }

        public:
        using siginfo_t = T_Sigdata;
        typedef siginfo_t &siginfo_p;
    };

    class base_fd_watcher : public base_watcher
    {
        template <typename, typename> friend class event_dispatch;
        template <typename, typename> friend class dasynq::event_loop;

        protected:
        int watch_fd;

        // These flags are protected by the loop's internal lock:
        int watch_flags;  // events being watched
        int event_flags;  // events pending (queued)

        // watch_flags: for a regular fd_watcher, this specifies the events that the watcher
        //              is watching (or was watching if disabled). For a bidi_fd_watcher, specifies
        //              the events that the watcher is currently watching (i.e. specifies which
        //              halves of the Bidi watcher are enabled).

        base_fd_watcher() noexcept : base_watcher(watch_type_t::FD) { }
    };

    class base_bidi_fd_watcher : public base_fd_watcher
    {
        template <typename, typename> friend class event_dispatch;
        template <typename, typename> friend class dasynq::event_loop;

        base_bidi_fd_watcher(const base_bidi_fd_watcher &) = delete;

        protected:
        base_bidi_fd_watcher() noexcept { }

        // The main instance is the "input" watcher only; we keep a secondary watcher with a secondary set
        // of flags for the "output" watcher. Note that some of the flags in the secondary watcher aren't
        // used; it exists mainly so that it can be queued independently of the primary watcher.
        base_watcher out_watcher {watch_type_t::SECONDARYFD};

        int read_removed : 1; // read watch removed?
        int write_removed : 1; // write watch removed?
    };

    class base_child_watcher : public base_watcher
    {
        template <typename, typename> friend class event_dispatch;
        template <typename, typename> friend class dasynq::event_loop;

        protected:
        pid_watch_handle_t watch_handle;
        pid_t watch_pid;
        int child_status;

        base_child_watcher() : base_watcher(watch_type_t::CHILD) { }
    };


    class base_timer_watcher : public base_watcher
    {
        template <typename, typename> friend class event_dispatch;
        template <typename, typename> friend class dasynq::event_loop;

        protected:
        timer_handle_t timer_handle;
        int intervals;
        clock_type clock;

        base_timer_watcher() : base_watcher(watch_type_t::TIMER)
        {
            init_timer_handle(timer_handle);
        }
    };
} // dprivate
} // dasynq
