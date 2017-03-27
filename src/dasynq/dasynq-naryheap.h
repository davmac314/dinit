#ifndef DASYNC_NARYHEAP_H_INCLUDED
#define DASYNC_NARYHEAP_H_INCLUDED

#include "dasynq-svec.h"
#include <type_traits>
#include <functional>
#include <limits>

namespace dasynq {

/**
 * Priority queue implementation based on an "Nary" heap (for power-of-2 N's).
 *
 * This is implemented somewhat as a heap-of-(binary)heaps. Eg with an N of 8, each mini heap has 7 elements;
 * a mini-heap looks like:
 *
 *              0
 *           1     2
 *          3 4   5 6
 *
 * There are N child mini-heaps underneath each mini-heap.
 *
 * Heap entry "handles" maintain an index into the heap. When the position of a node in the heap
 * changes, its handle must be updated.
 *
 * T : node data type
 * P : priority type (eg int)
 * Compare : functional object type to compare priorities
 */
template <typename T, typename P, typename Compare = std::less<P>, int N = 16>
class NaryHeap
{
    public:
    struct handle_t;
    using handle_t_r = handle_t &;

    private:

    // Actual heap node
    class HeapNode
    {
        public:
        P data;
        handle_t * hnd_p;

        HeapNode(handle_t * hnd, const P &odata) : data(odata), hnd_p(hnd)
        {
            // nothing to do
        }

        HeapNode() { }
    };

    svector<HeapNode> hvec;

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
    };

    // Initialise a handle (if it does not have a suitable constructor). Need not do anything
    // but may store a sentinel value to mark the handle as inactive. It should not be
    // necessary to call this, really.
    static void init_handle(handle_t &h) noexcept
    {
    }

    private:

    // In hindsight, I probably could have used std::priority_queue rather than re-implementing a
    // queue here. However, priority_queue doesn't expose the container (except as a protected
    // member) and so would make it more complicated to reserve storage. It would also have been
    // necessary to override assignment between elements to correctly update the reference in the
    // handle.

    bool bubble_down() noexcept
    {
        return bubble_down(hvec.size() - 1);
    }

    // Bubble a newly added timer down to the correct position
    bool bubble_down(hindex_t pos) noexcept
    {
        P op = hvec[pos].data;
        handle_t * ohndl = hvec[pos].hnd_p;
        return bubble_down(pos, ohndl, op);
    }

    bool bubble_down(hindex_t pos, handle_t * ohndl, const P &op) noexcept
    {
        // int pos = v.size() - 1;
        Compare lt;

        while (pos > 0) {
            // bubble down within micro heap:
            auto mh_index = pos % (N - 1);
            auto mh_base = pos - mh_index;
            while (mh_index > 0) {
                hindex_t parent = (mh_index - 1) / 2;
                if (! lt(op, hvec[parent + mh_base].data)) {
                    pos = mh_index + mh_base;
                    hvec[pos].hnd_p = ohndl;
                    ohndl->heap_index = pos;
                    return false;
                }

                hvec[mh_index + mh_base] = hvec[parent + mh_base];
                hvec[mh_index + mh_base].hnd_p->heap_index = mh_index + mh_base;
                mh_index = parent;
            }

            pos = mh_base;
            if (pos == 0) break;

            auto containing_unit = pos / (N - 1);
            auto parent_unit = (containing_unit - 1) / N;
            auto rem = (containing_unit - 1) % N;
            auto parent_idx = (rem / 2) + (N / 2) - 1;

            hindex_t parent = parent_unit * (N - 1) + parent_idx;
            if (! lt(op, hvec[parent].data)) {
                break;
            }

            hvec[pos] = hvec[parent];
            hvec[pos].hnd_p->heap_index = pos;
            pos = parent;
        }

        hvec[pos].hnd_p = ohndl;
        ohndl->heap_index = pos;

        return pos == 0;
    }

    void bubble_up(hindex_t pos = 0)
    {
        P p = hvec[pos].data;
        handle_t &hndl = *hvec[pos].hnd_p;
        bubble_up(pos, hvec.size() - 1, hndl, p);
    }

    void bubble_up(hindex_t pos, hindex_t rmax, handle_t &h, const P &p) noexcept
    {
        Compare lt;

        while (true) {
            auto containing_unit = pos / (N - 1);
            auto mh_index = pos % (N - 1);
            auto mh_base = pos - mh_index;

            while (mh_index < (N-2) / 2) {
                hindex_t selchild;
                hindex_t lchild = mh_index * 2 + 1 + mh_base;
                hindex_t rchild = lchild + 1;

                if (rchild >= rmax) {
                    if (lchild > rmax) {
                        hvec[pos].hnd_p = &h;
                        hvec[pos].data = p;
                        h.heap_index = pos;
                        return;
                    }
                    selchild = lchild;
                }
                else {
                    // select the sooner of lchild and rchild
                    selchild = lt(hvec[lchild].data, hvec[rchild].data) ? lchild : rchild;
                }

                if (! lt(hvec[selchild].data, p)) {
                    hvec[pos].hnd_p = &h;
                    hvec[pos].data = p;
                    h.heap_index = pos;
                    return;
                }

                hvec[pos] = hvec[selchild];
                hvec[pos].hnd_p->heap_index = pos;
                pos = selchild;
                mh_index = pos - mh_base;
            }

            auto left_unit = containing_unit * N + (mh_index - (N/2 - 1)) * 2 + 1;
            // auto right_unit = left_unit + 1;

            hindex_t selchild;
            hindex_t lchild = left_unit * (N - 1);
            hindex_t rchild = lchild + (N - 1);
            if (rchild >= rmax) {
                if (lchild > rmax) {
                    break;
                }
                selchild = lchild;
            }
            else {
                // select the sooner of lchild and rchild
                selchild = lt(hvec[lchild].data, hvec[rchild].data) ? lchild : rchild;
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

    void remove_h(hindex_t hidx)
    {
        // bvec[hvec[hidx].data_index].heap_index = -1;
        hvec[hidx].hnd_p->heap_index = -1;
        if (hvec.size() != hidx + 1) {
            // replace the first element with the last:
            // bvec[hvec.back().data_index].heap_index = hidx;

            //hvec.back().hnd_p->heap_index = hidx;
            //hvec[hidx] = hvec.back();
            //hvec.pop_back();

            // Now bubble up:
            //bubble_up(hidx);

            bubble_up(hidx, hvec.size() - 2, *(hvec.back().hnd_p), hvec.back().data);
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

    bool insert(handle_t & hnd, const P & pval) noexcept
    {
        hvec.emplace_back(&hnd, pval);
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

    bool is_queued(handle_t hnd)
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
};

}

#endif
