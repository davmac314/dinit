#ifndef DASYNQ_BTREE_SET_H
#define DASYNQ_BTREE_SET_H

#include <functional>
#include <utility>

namespace dasynq {

// A sorted set based on a B-Tree data structure, supporting pre-allocation of nodes.

template <typename T, typename P, typename Compare = std::less<P>, int N = 8>
class btree_set
{
    struct heapnode;

    public:
    using handle_t = heapnode;
    using handle_t_r = heapnode &;

    private:

    struct septnode
    {
        P prio[N];
        handle_t * hn_p[N];  // pointer to handle
        septnode * children[N + 1];
        septnode * parent;

        septnode()
        {
            // Do nothing; initialisation will be run later
        }

        void init()
        {
            for (int i = 0; i < N; i++) {
                hn_p[i] = nullptr;
                children[i] = nullptr;
            }
            children[N] = nullptr;
            parent = nullptr;
        }

        int num_vals() noexcept
        {
            // We expect to be >50% full, so count backwards:
            for (int i = N - 1; i >= 0; i--) {
                if (hn_p[i] != nullptr) {
                    return i + 1;
                }
            }
            return 0;
        }

        bool is_leaf() noexcept
        {
            return children[0] == nullptr;
        }

        void shift_elems_left(int pos, int newpos, int num)
        {
            int diff = pos - newpos;
            int end = pos + num;

            for (int i = pos; i < end; i++) {
                prio[i - diff] = prio[i];
                hn_p[i - diff] = hn_p[i];
                children[i - diff] = children[i];
            }
            children[end - diff] = children[end];
        }

        void shift_elems_right(int pos, int newpos, int num)
        {
            int diff = newpos - pos;
            int end = pos + num;

            children[end + diff] = children[end];
            for (int i = (end - 1); i >= pos; i--) {
                prio[i + diff] = prio[i];
                hn_p[i + diff] = hn_p[i];
                children[i + diff] = children[i];
            }
        }
    };

    struct heapnode
    {
        union nodedata_u
        {
            T data;

            nodedata_u() {}
        };

        nodedata_u nodedata;
        septnode * parent = nullptr;

        heapnode()
        {

        }
    };

    septnode * root_sept = nullptr; // root of the B-Tree
    septnode * left_sept = nullptr; // leftmost child (cache)
    septnode * sn_reserve = nullptr;

    int num_alloced = 0;
    int num_septs = 0;
    int num_septs_needed = 0;
    int next_sept = 1;  // next num_allocd for which we need another septnode in reserve.

    // Note that sept nodes are always at least half full, except for the root sept node.
    // For up to N nodes, one sept node is needed;
    //        at N+1 nodes, three sept nodes are needed: a root and two leaves;
    //     for every N/2 nodes thereafter, an additional sept node may be required.
    // A simple approximation is, s = (n * 2 + N - 1) / N.
    // (Actually we get away with much less, if nodes have the same priority, since they are
    // then linked in list and effectively become a single node).

    void alloc_slot()
    {
        num_alloced++;

        if (__builtin_expect(num_alloced == next_sept, 0)) {
            if (++num_septs_needed > num_septs) {
                try {
                    septnode *new_res = new septnode();
                    new_res->parent = sn_reserve;
                    sn_reserve = new_res;
                    num_septs++;
                }
                catch (...) {
                    num_septs_needed--;
                    num_alloced--;
                    throw;
                }
            }
            next_sept += N/2;
        }
    }

    septnode * alloc_sept()
    {
        septnode * r = sn_reserve;
        sn_reserve = r->parent;
        r->init();
        return r;
    }

    void release_sept(septnode *s)
    {
        s->parent = sn_reserve;
        sn_reserve = s;
    }

    // Merge rsibling, and one value from the parent, into lsibling.
    // Index is the index of the parent value.
    void merge(septnode *lsibling, septnode *rsibling, int index) noexcept
    {
        int lchildren = lsibling->num_vals();
        lsibling->hn_p[lchildren] = lsibling->parent->hn_p[index];
        lsibling->prio[lchildren] = lsibling->parent->prio[index];
        lsibling->hn_p[lchildren]->parent = lsibling;
        lchildren++;

        // bool leaf = lsibling->is_leaf();

        int ri = 0;
        for (ri = 0; rsibling->hn_p[ri] != nullptr; ri++) {
            lsibling->hn_p[lchildren] = rsibling->hn_p[ri];
            lsibling->prio[lchildren] = rsibling->prio[ri];
            lsibling->children[lchildren] = rsibling->children[ri];
            if (lsibling->children[lchildren]) lsibling->children[lchildren]->parent = lsibling;
            lsibling->hn_p[lchildren]->parent = lsibling;
            lchildren++;
        }
        lsibling->children[lchildren] = rsibling->children[ri];
        if (lsibling->children[lchildren]) lsibling->children[lchildren]->parent = lsibling;
        release_sept(rsibling);

        // Now delete in the parent:
        for (int i = index; i < (N-1); i++) {
            lsibling->parent->hn_p[i] = lsibling->parent->hn_p[i + 1];
            lsibling->parent->prio[i] = lsibling->parent->prio[i + 1];
            lsibling->parent->children[i + 1] = lsibling->parent->children[i + 2];
        }
        lsibling->parent->hn_p[N-1] = nullptr;

        if (lsibling->parent->hn_p[0] == nullptr) {
            // parent is now empty; it must be root. Make us the new root.
            release_sept(lsibling->parent);
            root_sept = lsibling;
            lsibling->parent = nullptr;
        }
    }

    // borrow values from, or merge with, a sibling node so that the node
    // is suitably (~>=50%) populated.
    void repop_node(septnode *sept, int children) noexcept
    {
        start:
        septnode *parent = sept->parent;
        if (parent == nullptr) {
            // It's the root node, so don't worry about it, unless empty
            if (sept->hn_p[0] == nullptr) {
                root_sept = nullptr;
                left_sept = nullptr;
                release_sept(sept);
            }
            return;
        }

        // Find a suitable sibling to the left or right:
        if (parent->children[0] == sept) {
            // take right sibling
            septnode *rsibling = parent->children[1];
            if (rsibling->num_vals() + children + 1 <= N) {
                // We can merge
                merge(sept, rsibling, 0);
                if (sept->parent != nullptr) {
                    children = sept->parent->num_vals();
                    if (children < N/2) {
                        sept = sept->parent;
                        goto start;
                    }
                }
            }
            else {
                sept->hn_p[children] = parent->hn_p[0];
                sept->prio[children] = parent->prio[0];
                sept->hn_p[children]->parent = sept;
                sept->children[children + 1] = rsibling->children[0];
                if (sept->children[children + 1]) sept->children[children + 1]->parent = sept;

                parent->hn_p[0] = rsibling->hn_p[0];
                parent->prio[0] = rsibling->prio[0];
                parent->hn_p[0]->parent = parent;

                rsibling->shift_elems_left(1, 0, N-1);
                rsibling->hn_p[N-1] = nullptr;
                return;
            }
        }
        else {
            // find left sibling
            int i;
            for (i = 1; i < N; i++) {
                if (parent->children[i] == sept) {
                    break;
                }
            }

            septnode *lsibling = parent->children[i-1];
            int lchildren = lsibling->num_vals();
            if (lchildren + children + 1 <= N) {
                // merge
                merge(lsibling, sept, i - 1);
                if (lsibling->parent != nullptr) {
                    children = lsibling->parent->num_vals();
                    if (children < N/2) {
                        sept = lsibling->parent;
                        goto start;
                    }
                }
            }
            else {
                sept->shift_elems_right(0, 1, children);

                sept->hn_p[0] = parent->hn_p[i - 1];
                sept->prio[0] = parent->prio[i - 1];
                sept->hn_p[0]->parent = sept;
                sept->children[0] = lsibling->children[lchildren];
                if (sept->children[0]) sept->children[0]->parent = sept;

                parent->hn_p[i - 1] = lsibling->hn_p[lchildren - 1];
                parent->prio[i - 1] = lsibling->prio[lchildren - 1];
                parent->hn_p[i - 1]->parent = parent;
                lsibling->hn_p[lchildren - 1] = nullptr;

                return;
            }
        }
    }

    public:

    T & node_data(handle_t & hn) noexcept
    {
        return hn.nodedata.data;
    }

    static void init_handle(handle_t &hn) noexcept
    {
        // nothing to do
    }

    // Allocate a slot, but do not incorporate into the heap:
    template <typename ...U> void allocate(handle_t &hn, U&&... u)
    {
        alloc_slot();
        new (& hn.nodedata.data) T(std::forward<U>(u)...);
    }

    void deallocate(handle_t & hn) noexcept
    {
        hn.nodedata.data.T::~T();
        num_alloced--;

        // Potentially release reserved sept node
        if (__builtin_expect(num_alloced < next_sept - N/2, 0)) {
            next_sept -= N/2;
            num_septs_needed--;
            if (num_septs_needed < num_septs - 1) {
                // Note the "-1" margin is to alleviate bouncing allocation/deallocation
                septnode * r = sn_reserve;
                sn_reserve = r->parent;
                delete r;
                num_septs--;
            }
        }
    }

    // Insert an allocated slot into the heap.
    // Return true if it is the leftmost value.
    bool insert(handle_t & hndl, P pval = P()) noexcept
    {
        if (root_sept == nullptr) {
            root_sept = alloc_sept();
            left_sept = root_sept;
        }

        septnode * srch_sept = root_sept;

        bool leftmost = true;

        while (! srch_sept->is_leaf()) {
            int min = 0;
            int max = N - 1;
            while (min <= max) {
                int i = (min + max) / 2;

                if (srch_sept->hn_p[i] == nullptr || pval < srch_sept->prio[i]) {
                    max = i - 1;
                }
                else if (srch_sept->prio[i] == pval) {
                    // Already present?
                    return false;
                }
                else {
                    min = i + 1;
                }
            }

            if (min != 0) {
                leftmost = false;
            }

            // go up to the right:
            srch_sept = srch_sept->children[max + 1];
        }

        // We got to a leaf: does it have space?
        // First check if we can add to a linked list
        int children = srch_sept->num_vals();

        {
            int min = 0;
            int max = children - 1;
            while (min <= max) {
                int i = (min + max) / 2;

                if (srch_sept->hn_p[i] == nullptr || pval < srch_sept->prio[i]) {
                    max = i - 1;
                }
                else if (srch_sept->prio[i] == pval) {
                    // Already present?
                    return false;
                }
                else {
                    min = i + 1;
                }
            }
        }

        septnode * left_down = nullptr; // left node going down
        septnode * right_down = nullptr; // right node going down
        leftmost = leftmost && pval < srch_sept->prio[0];

        handle_t * hndl_p = &hndl;

        while (children == N) {
            // split and push value towards root
            septnode * new_sibling = alloc_sept();
            new_sibling->parent = srch_sept->parent;

            // create new sibling to the right:
            for (int i = N/2; i < N; i++) {
                new_sibling->prio[i - N/2] = srch_sept->prio[i];  // new[0] = old[4]
                new_sibling->hn_p[i - N/2] = srch_sept->hn_p[i];
                new_sibling->children[i - N/2 + 1] = srch_sept->children[i + 1];
                if (new_sibling->children[i - N/2 + 1]) new_sibling->children[i - N/2 + 1]->parent = new_sibling;
                new_sibling->hn_p[i - N/2]->parent = new_sibling;
                srch_sept->hn_p[i] = nullptr;
            }
            // Note that new_sibling->children[0] has not yet been set.

            if (pval < srch_sept->prio[N/2 - 1])  {
                auto o_prio = srch_sept->prio[N/2 - 1];
                auto o_hidx = srch_sept->hn_p[N/2 - 1];

                new_sibling->children[0] = srch_sept->children[N/2];
                if (new_sibling->children[0]) new_sibling->children[0]->parent = new_sibling;

                int i = N/2 - 1;
                for ( ; i > 0 && pval < srch_sept->prio[i - 1]; i--) {
                    srch_sept->prio[i] = srch_sept->prio[i - 1];
                    srch_sept->children[i+1] = srch_sept->children[i];
                    srch_sept->hn_p[i] = srch_sept->hn_p[i - 1];
                }
                srch_sept->prio[i] = pval;
                srch_sept->hn_p[i] = hndl_p;
                hndl_p->parent = srch_sept;
                srch_sept->children[i] = left_down;
                srch_sept->children[i+1] = right_down;
                hndl_p = o_hidx;
                pval = o_prio;
            }
            else if (pval < new_sibling->prio[0]) {
                // new value is right in the middle
                srch_sept->children[N/2] = left_down;
                new_sibling->children[0] = right_down;
                if (left_down) left_down->parent = srch_sept;
                if (right_down) right_down->parent = new_sibling;
            }
            else {
                auto o_prio = new_sibling->prio[0];
                auto o_hidx = new_sibling->hn_p[0];
                int i = 0;
                for ( ; i < (N/2 - 1) && new_sibling->prio[i + 1] < pval; i++) {
                    new_sibling->prio[i] = new_sibling->prio[i + 1];
                    new_sibling->children[i] = new_sibling->children[i + 1];
                    new_sibling->hn_p[i] = new_sibling->hn_p[i + 1];
                }
                new_sibling->prio[i] = pval;
                new_sibling->hn_p[i] = hndl_p;
                hndl_p->parent = new_sibling;
                new_sibling->children[i] = left_down;
                new_sibling->children[i+1] = right_down;
                if (left_down) left_down->parent = new_sibling;
                if (right_down) right_down->parent = new_sibling;
                hndl_p = o_hidx;
                pval = o_prio;
            }

            left_down = srch_sept;
            right_down = new_sibling;

            srch_sept = srch_sept->parent;
            if (srch_sept == nullptr) {
                // Need new root node:
                srch_sept = alloc_sept();
                root_sept = srch_sept;
                left_down->parent = root_sept;
                right_down->parent = root_sept;
                children = 0;
            }
            else {
                children = srch_sept->num_vals();
            }
        }

        // Insert into non-full node:
        int inspos;
        for (inspos = children; inspos > 0; inspos--) {
            if (srch_sept->prio[inspos - 1] < pval) {
                break;
            }

            srch_sept->prio[inspos] = srch_sept->prio[inspos-1];
            srch_sept->hn_p[inspos] = srch_sept->hn_p[inspos-1];
            srch_sept->children[inspos+1] = srch_sept->children[inspos];
        }

        srch_sept->prio[inspos] = pval;
        srch_sept->hn_p[inspos] = hndl_p;
        srch_sept->children[inspos] = left_down;
        srch_sept->children[inspos+1] = right_down;
        hndl_p->parent = srch_sept;
        return leftmost;
    }

    // Remove a slot from the heap (but don't deallocate it)
    void remove(handle_t & hndl) noexcept
    {
        // we have to remove from the Btree itself
        // Pull nodes from a child, all the way down
        // the tree. Then re-balance back up the tree,
        // merging nodes if necessary.
        septnode * sept = hndl.parent;

        int i;
        for (i = 0; i < N; i++) {
            if (sept->hn_p[i] == &hndl) {
                // Ok, go right, then as far as we can to the left:
                septnode * lsrch = sept->children[i+1];
                septnode * prev = sept;
                while (lsrch != nullptr) {
                    prev = lsrch;
                    lsrch = lsrch->children[0];
                }

                if (prev != sept) {
                    sept->hn_p[i] = prev->hn_p[0];
                    sept->prio[i] = prev->prio[0];
                    sept->hn_p[i]->parent = sept;
                    prev->hn_p[0] = &hndl;
                    sept = prev;
                    i = 0;
                }

                // Now we have:
                // - sept is a leaf in the BTree
                // - i is the index of the child to remove from it

                for ( ; i < (N-1); i++) {
                    sept->hn_p[i] = sept->hn_p[i+1];
                    sept->prio[i] = sept->prio[i+1];
                    if (sept->hn_p[i] == nullptr) {
                        break;
                    }
                }

                sept->hn_p[N-1] = nullptr;

                // Now if the node is underpopulated, we need to merge with or
                // borrow from a sibling
                if (i < N/2) {
                    repop_node(sept, i);
                }

                return;
            }
        }
    }

    handle_t *find(const P &pval)
    {
        septnode * cursept = root_sept;
        while (cursept != nullptr) {
            int i;
            for (i = 0; i < N && cursept->hn_p[i] != nullptr; i++) {
                if (cursept->prio[i] == pval) {
                    return cursept->hn_p[i];
                }
                if (cursept->prio[i] > pval) {
                    break;
                }
            }
            cursept = cursept->children[i];
        }

        return nullptr;
    }

    bool is_queued(handle_t & hndl) noexcept
    {
        return hndl.parent != nullptr;
    }

    bool empty() noexcept
    {
        return root_sept == nullptr;
    }

    ~btree_set()
    {
        while (left_sept != nullptr) {
            remove(*(left_sept->hn_p[0]));
        }

        while (sn_reserve != nullptr) {
            auto *next = sn_reserve->parent;
            delete sn_reserve;
            sn_reserve = next;
        }
    }
};

}

#endif
