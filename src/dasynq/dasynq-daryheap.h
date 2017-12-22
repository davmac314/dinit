#ifndef DASYNC_DARYHEAP_H_INCLUDED
#define DASYNC_DARYHEAP_H_INCLUDED

#include "dasynq-svec.h"
#include <type_traits>
#include <functional>
#include <limits>

namespace dasynq {

/**
 * Priority queue implementation based on a binary heap.
 *
 * Heap entry "handles" maintain an index into the heap. When the position of a node in the heap
 * changes, its handle must be updated.
 *
 * T : node data type
 * P : priority type (eg int)
 * Compare : functional object type to compare priorities
 */
template <typename T, typename P, typename Compare = std::less<P>, int N = 4>
class dary_heap
{
    public:
    struct handle_t;
    using handle_t_r = handle_t &;

    private:

    // Actual heap node
    class heap_node
    {
        public:
        P data;
        handle_t * hnd_p;

        heap_node(handle_t * hnd, const P &odata) : data(odata), hnd_p(hnd)
        {
            // nothing to do
        }

        heap_node() { }
    };

    svector<heap_node> hvec;

    using hindex_t = typename decltype(hvec)::size_type;

    int root_node = -1;
    hindex_t num_nodes = 0;

    public:

    // Handle to an element on the heap in the node buffer; also contains the data associated
    // with the node. (Alternative implementation would be to store the heap data in a
    // separate container, and have the handle be an index into that container).
    struct handle_t
    {
        T hd;
        hindex_t heap_index;

        handle_t(const handle_t &) = delete;
        handle_t() { }
    };

    // Initialise a handle (if it does not have a suitable constructor). Need not do anything
    // but may store a sentinel value to mark the handle as inactive. It should not be
    // necessary to call this, really.
    static void init_handle(handle_t &h) noexcept
    {
    }

    private:

    // Bubble a newly added timer down to the correct position
    bool bubble_down(hindex_t pos) noexcept
    {
        handle_t * ohndl = hvec[pos].hnd_p;
        P op = hvec[pos].data;
        return bubble_down(pos, ohndl, op);
    }

    bool bubble_down(hindex_t pos, handle_t * ohndl, const P &op) noexcept
    {
        // int pos = v.size() - 1;
        Compare lt;
        while (pos > 0) {
            hindex_t parent = (pos - 1) / N;
            if (! lt(op, hvec[parent].data)) {
                break;
            }

            hvec[pos] = hvec[parent];
            hvec[pos].hnd_p->heap_index = pos;
            pos = parent;
        }

        hvec[pos].hnd_p = ohndl;
        hvec[pos].data = op;
        ohndl->heap_index = pos;

        return pos == 0;
    }

    void bubble_up(hindex_t pos = 0) noexcept
    {
        P p = hvec[pos].data;
        handle_t &h = *(hvec[pos].hnd_p);
        bubble_up(pos, h, p);
    }

    void bubble_up(hindex_t pos, handle_t &h, const P &p) noexcept
    {
        Compare lt;
        hindex_t rmax = hvec.size() - 1;
        hindex_t max = (rmax - 1) / N;

        while (pos <= max) {
            hindex_t lchild = pos * N + 1;
            hindex_t selchild = lchild;
            hindex_t rchild = std::min(lchild + N, rmax);
            for (hindex_t i = lchild + 1; i < rchild; i++) {
                if (lt(hvec[i].data, hvec[selchild].data)) {
                    selchild = i;
                }
            }

            if (! lt(hvec[selchild].data, p)) {
                break;
            }

            hvec[pos] = hvec[selchild];
            hvec[pos].hnd_p->heap_index = pos;
            pos = selchild;
        }

        hvec[pos].hnd_p = &h;
        hvec[pos].data = p;
        h.heap_index = pos;
    }

    void remove_h(hindex_t hidx) noexcept
    {
        hvec[hidx].hnd_p->heap_index = -1;
        if (hvec.size() != hidx + 1) {
            bubble_up(hidx, *(hvec.back().hnd_p), hvec.back().data);
            hvec.pop_back();
        }
        else {
            hvec.pop_back();
        }
    }

    public:

    T & node_data(handle_t & index) noexcept
    {
        return index.hd;
    }

    // Allocate a slot, but do not incorporate into the heap:
    //  u... : parameters for data constructor T::T(...)
    template <typename ...U> void allocate(handle_t & hnd, U... u)
    {
        new (& hnd.hd) T(u...);
        hnd.heap_index = -1;
        constexpr hindex_t max_allowed = std::numeric_limits<hindex_t>::is_signed ?
                std::numeric_limits<hindex_t>::max() : ((hindex_t) - 2);

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
        //hvec.emplace_back(&hnd, pval);
        hvec.emplace_back();
        return bubble_down(hvec.size() - 1, &hnd, pval);
    }

    // Get the root node handle. (Returns a handle_t or reference to handle_t).
    handle_t & get_root()
    {
        return * hvec[0].hnd_p;
    }

    P &get_root_priority()
    {
        return hvec[0].data;
    }

    void pull_root()
    {
        remove_h(0);
    }

    void remove(handle_t & hnd)
    {
        remove_h(hnd.heap_index);
    }

    bool empty()
    {
        return hvec.empty();
    }

    bool is_queued(handle_t & hnd)
    {
        return hnd.heap_index != (hindex_t) -1;
    }

    // Set a node priority. Returns true iff the node becomes the root node (and wasn't before).
    bool set_priority(handle_t & hnd, const P& p)
    {
        int heap_index = hnd.heap_index;

        Compare lt;
        if (lt(hvec[heap_index].data, p)) {
            // Increase key
            hvec[heap_index].data = p;
            bubble_up(heap_index);
            return false;
        }
        else {
            // Decrease key
            hvec[heap_index].data = p;
            return bubble_down(heap_index);
        }
    }

    dary_heap() { }

    dary_heap(const dary_heap &) = delete;
};

}

#endif
