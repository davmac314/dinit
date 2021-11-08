#ifndef DASYNQ_CHILDPROC_H_
#define DASYNQ_CHILDPROC_H_

#include <sys/types.h>
#include <sys/wait.h>

#include <csignal>

#include "btree_set.h"

namespace dasynq {

namespace dprivate {

// Map of pid_t to void *, with possibility of reserving entries so that mappings can
// be later added with no danger of allocator exhaustion (bad_alloc).
class pid_map
{
    using bmap_t = btree_set<void *, pid_t>;
    bmap_t b_map;
    
    public:
    using pid_handle_t = bmap_t::handle_t;
    
    // Map entry: present (bool), data (void *)
    using entry = std::pair<bool, void *>;

    entry get(pid_t key) noexcept
    {
        auto it = b_map.find(key);
        if (it == nullptr) {
            return entry(false, nullptr);
        }
        return entry(true, b_map.node_data(*it));
    }
    
    entry remove(pid_t key) noexcept
    {
        auto it = b_map.find(key);
        if (it == nullptr) {
            return entry(false, nullptr);
        }
        b_map.remove(*it);
        return entry(true, b_map.node_data(*it));
    }
    
    void remove(pid_handle_t &hndl)
    {
        if (b_map.is_queued(hndl)) {
            b_map.remove(hndl);
        }
    }

    // Throws bad_alloc on reservation failure
    void reserve(pid_handle_t &hndl)
    {
        b_map.allocate(hndl);
    }
    
    void unreserve(pid_handle_t &hndl) noexcept
    {
        b_map.deallocate(hndl);
    }
    
    void add(pid_handle_t &hndl, pid_t key, void *val) // throws std::bad_alloc
    {
        reserve(hndl);
        b_map.node_data(hndl) = val;
        b_map.insert(hndl, key);
    }
    
    void add_from_reserve(pid_handle_t &hndl, pid_t key, void *val) noexcept
    {
        b_map.node_data(hndl) = val;
        b_map.insert(hndl, key);
    }
};

inline void sigchld_handler(int signum)
{
    // If SIGCHLD has no handler (is ignored), SIGCHLD signals will
    // not be queued for terminated child processes. (On Linux, the
    // default disposition for SIGCHLD is to be ignored but *not* have
    // this behavior, which seems inconsistent. Setting a handler doesn't
    // hurt in any case).
}

} // namespace dprivate

using pid_watch_handle_t = dprivate::pid_map::pid_handle_t;

template <class Base> class child_proc_events : public Base
{
    public:
    using reaper_mutex_t = typename Base::mutex_t;

    class traits_t : public Base::traits_t
    {
        public:
        constexpr static bool supports_childwatch_reservation = true;
    };

    private:
    dprivate::pid_map child_waiters;
    reaper_mutex_t reaper_lock; // used to prevent reaping while trying to signal a process
    
    protected:
    using sigdata_t = typename traits_t::sigdata_t;
    
    template <typename T>
    bool receive_signal(T & loop_mech, sigdata_t &siginfo, void *userdata)
    {
        if (siginfo.get_signo() == SIGCHLD) {
            int status;
            pid_t child;
            reaper_lock.lock();
            while ((child = waitpid(-1, &status, WNOHANG)) > 0) {
                auto ent = child_waiters.remove(child);
                if (ent.first) {
                    Base::receive_child_stat(child, status, ent.second);
                }
            }
            reaper_lock.unlock();
            return false; // leave signal watch enabled
        }
        else {
            return Base::receive_signal(loop_mech, siginfo, userdata);
        }
    }
    
    public:
    void reserve_child_watch_nolock(pid_watch_handle_t &handle)
    {
        child_waiters.reserve(handle);
    }
    
    void unreserve_child_watch(pid_watch_handle_t &handle) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        unreserve_child_watch_nolock(handle);
    }
    
    void unreserve_child_watch_nolock(pid_watch_handle_t &handle) noexcept
    {
        child_waiters.unreserve(handle);
    }

    void add_child_watch_nolock(pid_watch_handle_t &handle, pid_t child, void *val)
    {
        child_waiters.add(handle, child, val);
    }
    
    void add_reserved_child_watch(pid_watch_handle_t &handle, pid_t child, void *val) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        child_waiters.add_from_reserve(handle, child, val);
    }

    void add_reserved_child_watch_nolock(pid_watch_handle_t &handle, pid_t child, void *val) noexcept
    {
        child_waiters.add_from_reserve(handle, child, val);
    }
    
    // Stop watching a child, but retain watch reservation
    void stop_child_watch(pid_watch_handle_t &handle) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        child_waiters.remove(handle);
    }

    void remove_child_watch(pid_watch_handle_t &handle) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        remove_child_watch_nolock(handle);
    }

    void remove_child_watch_nolock(pid_watch_handle_t &handle) noexcept
    {
        child_waiters.remove(handle);
        child_waiters.unreserve(handle);
    }
    
    // Get the reaper lock, which can be used to ensure that a process is not reaped while attempting to
    // signal it.
    reaper_mutex_t &get_reaper_lock() noexcept
    {
        return reaper_lock;
    }

    template <typename T> void init(T *loop_mech)
    {
        // Mask SIGCHLD:
        sigset_t sigmask;
        this->sigmaskf(SIG_UNBLOCK, nullptr, &sigmask);
        sigaddset(&sigmask, SIGCHLD);
        this->sigmaskf(SIG_SETMASK, &sigmask, nullptr);

        // On some systems a SIGCHLD handler must be established, or SIGCHLD will not be
        // generated:
        struct sigaction chld_action;
        chld_action.sa_handler = dprivate::sigchld_handler;
        sigemptyset(&chld_action.sa_mask);
        chld_action.sa_flags = 0;
        sigaction(SIGCHLD, &chld_action, nullptr);

        // Specify a dummy user data value - sigchld_handler
        loop_mech->add_signal_watch(SIGCHLD, (void *) dprivate::sigchld_handler);
        Base::init(loop_mech);
    }
};

} // namespace dasynq

#endif /* DASYNQ_CHILDPROC_H_ */
