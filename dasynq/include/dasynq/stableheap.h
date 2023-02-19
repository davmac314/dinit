#ifndef DASYNQ_STABLEHEAP_H_
#define DASYNQ_STABLEHEAP_H_

// Convert an "unstable" priority queue (which doesn't use FIFO ordering for same-priority elements)
// into a "stable" queue (which does deliver same-priority elements in FIFO order). This is done by
// adding a generation counter to each element added to the queue, and using it as a second-order
// priority key (after the original key).
//
// The generation counter is a 64-bit integer and can not realistically overflow.

#include <functional>
#include <utility>

#include <cstdint>

namespace dasynq {

template <typename P>
class stable_prio
{
    public:
    P p;
    uint64_t order;
    
    template <typename ...U>
    stable_prio(uint64_t o, U... u) noexcept(noexcept(P(u...))): p(u...), order(o)
    {
    }
    
    // zero-argument constructor should not really be needed, but some
    // heap implementations aren't yet perfect.
    stable_prio() noexcept
    {
    }
};

template <typename P, typename C>
class compare_stable_prio
{
    public:
    bool operator()(const stable_prio<P> &a, const stable_prio<P> &b)
            noexcept(noexcept(C()) && noexcept(std::declval<C>()(std::declval<P>(),std::declval<P>())))
    {
        C lt;
        if (lt(a.p, b.p)) {
            return true;
        }
        if (lt(b.p, a.p)) {
            return false;
        }
        
        return a.order < b.order;
    }
};


template <template <typename H1, typename H2, typename H3> class H, typename T, typename P, typename C = std::less<P>>
class stable_heap : private H<T,stable_prio<P>,compare_stable_prio<P,C>>
{
    using Base = H<T,stable_prio<P>,compare_stable_prio<P,C>>;
    
    // using H<T,P,compare_stable_prio<P,C>>:H;  // inherit constructors
    using Base::Base;
    
    uint64_t sequence = 0;
    
    public:
    
    using handle_t = typename Base::handle_t;
    using handle_t_r = typename Base::handle_t_r;
    
    bool insert(handle_t & index, P pval = P())
    {
        auto sp = stable_prio<P>(sequence++, pval);
        return Base::insert(index, sp);
    }

    template <typename ...U> void allocate(handle_t & hnd, U&& ...u)
    {
        Base::allocate(hnd, std::forward<U>(u)...);
    }

    static void init_handle(handle_t &hndl) noexcept(noexcept(std::declval<Base>().init_handle(hndl)))
    {
        Base::init_handle(hndl);
    }

    T &node_data(handle_t &hndl) noexcept(noexcept(std::declval<Base>().node_data(hndl)))
    {
        return Base::node_data(hndl);
    }

    bool is_queued(handle_t & hnd) noexcept(noexcept(std::declval<Base>().is_queued(hnd)))
    {
        return Base::is_queued(hnd);
    }

    decltype(std::declval<Base>().get_root()) get_root() noexcept(noexcept(std::declval<Base>().get_root()))
    {
        return Base::get_root();
    }
    
    void pull_root() noexcept(noexcept(std::declval<Base>().pull_root()))
    {
        Base::pull_root();
    }
    
    void deallocate(handle_t_r index) noexcept(noexcept(std::declval<Base>().deallocate(index)))
    {
        Base::deallocate(index);
    }
    
    void remove(handle_t_r hnd) noexcept(noexcept(std::declval<Base>().remove(hnd)))
    {
        Base::remove(hnd);
    }
    
    bool empty() noexcept(noexcept(std::declval<Base>().empty()))
    {
        return Base::empty();
    }

    unsigned size() noexcept(noexcept(std::declval<Base>().size()))
    {
        return Base::size();
    }
};

} // namespace dasynq

#endif /* DASYNQ_STABLEHEAP_H_ */
