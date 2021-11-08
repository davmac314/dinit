#ifndef DASYNQ_DARYHEAP_H_
#define DASYNQ_DARYHEAP_H_

#include <type_traits>
#include <functional>
#include <utility>
#include <limits>

#include <cstddef>

#include "svec.h"

namespace dasynq {

/**
 * Priority queue implementation based on a heap with parameterised fan-out. All nodes are stored
 * in a vector, with the root at position 0, and each node has N child nodes, at positions
 * (p * N + 1) through (p * N + N) where p is the (parent) node position.
 *
 * With N=2, this is a binary heap. Higher values of N may give better performance due to better
 * cache locality, but also increase fan-out which will (if too high) also reduce performance.
 *
 * The destructor will not clean up (destruct) objects that have been added to the queue. If the
 * destructor of the element type (T) is non-trivial, all handles should be de-allocated before
 * destroying the queue.
 *
 * Implementation details:
 *
 * Adding a node returns a "handle", which maintains an index into the heap. When the position of
 * a node in the heap changes, its handle must be updated (the advantage is that changing priority
 * of or removing a node does not require a linear search for the node).
 *
 * Node data is actually stored as part of the handle, not in the queue.
 *
 * To add a node to the queue, it is inserted at the end and then "bubbled down" to its correct
 * location according to priority. To removing a node, the node is replaced with the last node in
 * the vector and then that is "bubbled up" to the correct position.
 *
 * Parameters:
 *
 * T : node data type
 * P : priority type (eg int)
 * Compare : functional object type to compare priorities
 * N : fan out factor (number of child nodes per node)
 */
template <typename T, typename P, typename Compare = std::less<P>, int N = 4>
class dary_heap
{
    public:
    struct handle_t;
    using handle_t_r = handle_t &;

    private:

    static_assert(std::is_nothrow_move_assignable<P>::value, "P must be no-except move assignable");

    // Actual heap node
    class heap_node
    {
        public:
        P prio;
        handle_t * hnd;

        heap_node(handle_t * hnd_p, const P &prio_p) noexcept(std::is_nothrow_copy_constructible<P>::value)
            : prio(prio_p), hnd(hnd_p)
        {
            // nothing to do
        }

        heap_node() { }
    };

    svector<heap_node> hvec;

    using hindex_t = typename decltype(hvec)::size_type;

    hindex_t num_nodes = 0;

    public:

    // Handle to an element on the heap in the node buffer; also contains the data associated
    // with the node. (Alternative implementation would be to store the heap data in a
    // separate container, and have the handle be an index into that container).
    struct handle_t
    {
        union hd_u_t {
            // The data member is kept in a union so it doesn't get constructed/destructed
            // automatically, and we can construct it lazily.
            public:
            hd_u_t() { }
            ~hd_u_t() { }
            T hd;
        } hd_u;

        hindex_t heap_index;

        handle_t(const handle_t &) = delete;
        void operator=(const handle_t &) = delete;

        handle_t() { }
    };

    // Initialise a handle (if it does not have a suitable constructor). Need not do anything
    // but may store a sentinel value to mark the handle as inactive. It should not be
    // necessary to call this, really.
    static void init_handle(handle_t &h) noexcept
    {
    }

    private:

    // Bubble a newly added node down to the correct position
    bool bubble_down(hindex_t pos) noexcept
    {
        handle_t * ohndl = hvec[pos].hnd;
        P op = hvec[pos].prio;
        return bubble_down(pos, ohndl, op);
    }

    bool bubble_down(hindex_t pos, handle_t * ohndl, const P &op) noexcept
    {
        Compare lt;
        while (pos > 0) {
            hindex_t parent = (pos - 1) / N;
            if (! lt(op, hvec[parent].prio)) {
                break;
            }

            hvec[pos] = std::move(hvec[parent]);
            hvec[pos].hnd->heap_index = pos;
            pos = parent;
        }

        hvec[pos].hnd = ohndl;
        hvec[pos].prio = std::move(op);
        ohndl->heap_index = pos;

        return pos == 0;
    }

    void bubble_up(hindex_t pos = 0) noexcept
    {
        P p = hvec[pos].prio;
        handle_t &h = *(hvec[pos].hnd);
        bubble_up(pos, h, p);
    }

    void bubble_up(hindex_t pos, handle_t &h, const P &p) noexcept
    {
        hindex_t rmax = hvec.size() - 1;
        if (rmax == 0) {
            return;
        }

        Compare lt;
        hindex_t max = (rmax - 1) / N;

        while (pos <= max) {
            // Find (select) the smallest child node
            hindex_t lchild = pos * N + 1;
            hindex_t selchild = lchild;
            hindex_t rchild = std::min(lchild + N, rmax);
            for (hindex_t i = lchild + 1; i < rchild; i++) {
                if (lt(hvec[i].prio, hvec[selchild].prio)) {
                    selchild = i;
                }
            }

            if (! lt(hvec[selchild].prio, p)) {
                break;
            }

            hvec[pos] = std::move(hvec[selchild]);
            hvec[pos].hnd->heap_index = pos;
            pos = selchild;
        }

        hvec[pos].hnd = &h;
        hvec[pos].prio = std::move(p);
        h.heap_index = pos;
    }

    void remove_h(hindex_t hidx) noexcept
    {
        hvec[hidx].hnd->heap_index = -1;
        if (hvec.size() != hidx + 1) {
            bubble_up(hidx, *(hvec.back().hnd), hvec.back().prio);
            hvec.pop_back();
        }
        else {
            hvec.pop_back();
        }
    }

    public:

    T & node_data(handle_t & index) noexcept
    {
        return index.hd_u.hd;
    }

    // Allocate a slot, but do not incorporate into the heap:
    //  u... : parameters for data constructor T::T(...)
    template <typename ...U> void allocate(handle_t & hnd, U&&... u)
    {
        new (& hnd.hd_u.hd) T(std::forward<U>(u)...);
        hnd.heap_index = -1;

        // largest object size is PTRDIFF_MAX, so we expect the largest vector is that / sizeof node:
        constexpr hindex_t max_allowed = (std::numeric_limits<std::ptrdiff_t>::max() - 1) / sizeof(heap_node);

        if (num_nodes == max_allowed) {
            throw std::bad_alloc();
        }

        num_nodes++;

        if (__builtin_expect(hvec.capacity() < num_nodes, 0)) {
            hindex_t half_point = max_allowed / 2;
            try {
                if (__builtin_expect(num_nodes < half_point, 1)) {
                    hvec.reserve(num_nodes * 2);
                }
                else {
                    hvec.reserve(max_allowed);
                }
            }
            catch (std::bad_alloc &e) {
                hvec.reserve(num_nodes);
            }
        }
    }

    // Deallocate a slot
    void deallocate(handle_t & index) noexcept
    {
        num_nodes--;
        index.hd_u.hd.~T();

        // shrink the capacity of hvec if num_nodes is sufficiently less than
        // its current capacity:
        if (num_nodes < hvec.capacity() / 4) {
            hvec.shrink_to(num_nodes * 2);
        }
    }

    bool insert(handle_t & hnd) noexcept
    {
        P pval = P();
        return insert(hnd, pval);
    }

    bool insert(handle_t & hnd, const P &pval) noexcept
    {
        hnd.heap_index = hvec.size();
        // emplace an empty node; data/prio will be stored via bubble_down.
        hvec.emplace_back();
        return bubble_down(hvec.size() - 1, &hnd, pval);
    }

    // Get the root node handle. (Returns a handle_t or reference to handle_t).
    handle_t & get_root() noexcept
    {
        return * hvec[0].hnd;
    }

    P &get_root_priority() noexcept
    {
        return hvec[0].prio;
    }

    void pull_root() noexcept
    {
        remove_h(0);
    }

    void remove(handle_t & hnd) noexcept
    {
        remove_h(hnd.heap_index);
    }

    bool empty() noexcept
    {
        return hvec.empty();
    }

    bool is_queued(handle_t & hnd) noexcept
    {
        return hnd.heap_index != (hindex_t) -1;
    }

    // Set a node priority. Returns true iff the node becomes the root node (and wasn't before).
    bool set_priority(handle_t & hnd, const P& p) noexcept
    {
        int heap_index = hnd.heap_index;

        Compare lt;
        if (lt(hvec[heap_index].prio, p)) {
            // Increase key
            hvec[heap_index].prio = p;
            bubble_up(heap_index);
            return false;
        }
        else {
            // Decrease key
            hvec[heap_index].prio = p;
            return bubble_down(heap_index);
        }
    }

    size_t size() noexcept
    {
        return hvec.size();
    }

    dary_heap() { }

    dary_heap(const dary_heap &) = delete;
};

} // namespace dasynq

#endif /* DASYNQ_DARYHEAP_H_ */
