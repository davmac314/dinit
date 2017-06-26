#ifndef DINIT_LL_INCLUDED
#define DINIT_LL_INCLUDED 1

// Simple single- and doubly-linked list implementation, where the contained element includes the
// list node. This allows a single item to be a member of several different kinds of list, without
// requiring dynamic allocation of nodes for the different lists.
//
// To accomplish this without abstraction penalty, the function to retrieve the list node from the
// element is specified as the second template parameter.

// Doubly-linked list node:
template <typename T>
struct lld_node
{
    T * next = nullptr;
    T * prev = nullptr;
};

// Singly-linked list node:
template <typename T>
struct lls_node
{
    T * next = nullptr;
};

// Doubly-linked list implementation. The list is circular, so 'first->prev' yields the tail of
// the list, though we still need to special-case the empty list (where first == nullptr).
// next/prev pointers in a node are set to nullptr when the node is not linked into a list
// (and are never equal to nullptr when the node is linked into a list).
template <typename T, lld_node<T> &(*E)(T *)>
class dlist
{
    T * first;
    // E extractor;

    public:
    dlist() noexcept : first(nullptr) { }

    bool is_queued(T *e) noexcept
    {
        auto &node = E(e);
        return node.next != nullptr;
    }

    void append(T *e) noexcept
    {
        auto &node = E(e);
        if (first == nullptr) {
            first = e;
            node.next = e;
            node.prev = e;
        }
        else {
            node.next = first;
            node.prev = E(first).prev;
            E(E(first).prev).next = e;
            E(first).prev = e;
        }
    }

    T * tail() noexcept
    {
        if (first == nullptr) {
            return nullptr;
        }
        else {
            return E(first).prev;
        }
    }

    bool is_empty() noexcept
    {
        return first == nullptr;
    }

    T * pop_front() noexcept
    {
        auto r = first;
        auto &first_node = E(first);
        if (first_node.next == first) {
            // Only one node in the queue:
            first_node.next = nullptr;
            first_node.prev = nullptr;
            first = nullptr;
        }
        else {
            // Unlink first node:
            auto &node = E(first_node.next);
            node.prev = first_node.prev;
            E(node.prev).next = first_node.next;
            first = first_node.next;
            // Return original first node:
            first_node.next = nullptr;
            first_node.prev = nullptr;
        }
        return r;
    }

    void unlink(T *record) noexcept
    {
        auto &node = E(record);
        if (first == record) {
            first = node.next;
            if (first == record) {
                // unlinking the only node in the list:
                first = nullptr;
            }
        }
        E(node.next).prev = node.prev;
        E(node.prev).next = node.next;
        node.next = nullptr;
        node.prev = nullptr;
    }
};

// Singly-linked list implementation.
template <typename T, lls_node<T> &(*E)(T *)>
class slist
{
    T * first;

    public:
    slist() noexcept : first(nullptr) { }

    bool is_queued(T *e) noexcept
    {
        auto &node = E(e);
        return node.next != nullptr || first == e;
    }

    void insert(T *e) noexcept
    {
        auto &node = E(e);
        node.next = first;
        first = e;
    }

    bool is_empty() noexcept
    {
        return first == nullptr;
    }

    T * pop_front() noexcept
    {
        T * r = first;
        auto &node = E(r);
        first = node.next;
        node.next = nullptr;
        return r;
    }
};


#endif
