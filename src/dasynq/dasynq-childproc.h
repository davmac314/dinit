#include <signal.h>
#include "dasynq-btree_set.h"

namespace dasynq {

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

using pid_watch_handle_t = pid_map::pid_handle_t;

template <class Base> class ChildProcEvents : public Base
{
    private:
    pid_map child_waiters;
    
    protected:
    using SigInfo = typename Base::SigInfo;
    
    template <typename T>
    bool receive_signal(T & loop_mech, SigInfo &siginfo, void *userdata)
    {
        if (siginfo.get_signo() == SIGCHLD) {
            int status;
            pid_t child;
            while ((child = waitpid(-1, &status, WNOHANG)) > 0) {
                pid_map::entry ent = child_waiters.remove(child);
                if (ent.first) {
                    Base::receiveChildStat(child, status, ent.second);
                }
            }
            return false; // leave signal watch enabled
        }
        else {
            return Base::receive_signal(loop_mech, siginfo, userdata);
        }
    }
    
    public:
    void reserveChildWatch(pid_watch_handle_t &handle)
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        child_waiters.reserve(handle);
    }
    
    void unreserveChildWatch(pid_watch_handle_t &handle) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        unreserveChildWatch_nolock(handle);
    }
    
    void unreserveChildWatch_nolock(pid_watch_handle_t &handle) noexcept
    {
        child_waiters.unreserve(handle);
    }

    void addChildWatch(pid_watch_handle_t &handle, pid_t child, void *val)
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        child_waiters.add(handle, child, val);
    }
    
    void addReservedChildWatch(pid_watch_handle_t &handle, pid_t child, void *val) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        child_waiters.add_from_reserve(handle, child, val);
    }

    void addReservedChildWatch_nolock(pid_watch_handle_t &handle, pid_t child, void *val) noexcept
    {
        child_waiters.add_from_reserve(handle, child, val);
    }
    
    // Stop watching a child, but retain watch reservation
    void stop_child_watch(pid_watch_handle_t &handle) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        child_waiters.remove(handle);
    }

    void removeChildWatch(pid_watch_handle_t &handle) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        removeChildWatch_nolock(handle);
    }

    void removeChildWatch_nolock(pid_watch_handle_t &handle) noexcept
    {
        child_waiters.remove(handle);
        child_waiters.unreserve(handle);
    }
    
    template <typename T> void init(T *loop_mech)
    {
        struct sigaction chld_action;
        chld_action.sa_handler = sigchld_handler;
        sigemptyset(&chld_action.sa_mask);
        chld_action.sa_flags = 0;
        sigaction(SIGCHLD, &chld_action, nullptr);
        loop_mech->addSignalWatch(SIGCHLD, nullptr);
        Base::init(loop_mech);
    }
};


} // end namespace
