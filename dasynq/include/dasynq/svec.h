#ifndef DASYNQ_SVEC_H_
#define DASYNQ_SVEC_H_

#include <type_traits>
#include <limits>
#include <utility>
#include <new>

// Vector with possibility to shrink capacity arbitrarily.
//
// The standard vector (std::vector) only allows shrinking a vector's capacity to its current size. In cases
// where we need to keep some reserved capacity beyond the current size, we need an alternative solution: hence,
// this class, svector.

namespace dasynq {

namespace svec_helper {

using size_type = decltype(sizeof(0));

// Helper function - copy/move vector contents from old to new array
// (choice of copy vs move depends on whether we can move without throwing)
template <typename T,
        bool __use_move = (std::is_nothrow_move_constructible<T>::value || !std::is_copy_constructible<T>::value),
        bool __copy_is_noexcept = std::is_nothrow_copy_constructible<T>::value>
struct move_helper
{
    static void move(T *from, T *to, size_type count) noexcept
    {
        // nothrow copy; we can safely copy-and-destruct as we go
        for (size_type i = 0; i < count; i++) {
            new (&to[i]) T(from[i]);
            from[i].T::~T();
        }
    }
};

template <typename T>
struct move_helper<T, false, false>
{
    static void move(T *from, T *to, size_type count)
    {
        // copy may throw. Wait until the whole vector is copied before destructing.
        size_type i;
        try {
            for (i = 0; i < count; i++) {
                new (&to[i]) T(from[i]);
            }

            for (i = 0; i < count; i++) {
                from[i].T::~T();
            }
        }
        catch (...) {
            // we have to destruct any constructed items
            while (i > 0) {
                to[i].T::~T();
            }
            throw;
        }
    }
};

template <typename T, bool __dontcare>
struct move_helper<T, true, __dontcare>
{
    // we always use noexcept, even if move may throw we cannot safely recover, so best
    // to terminate.
    static void move(T *from, T *to, size_type count) noexcept
    {
        for (size_type i = 0; i < count; i++) {
            new (&to[i]) T(std::move(from[i]));
            from[i].T::~T();
        }
    }
};

} // namespace svec_helper

template <typename T>
class svector
{
public:
    using size_type = decltype(sizeof(0));
    using pointer = T*;
    using difference_type = decltype(std::declval<pointer>() - std::declval<pointer>());

private:
    T * array;
    size_type size_v;
    size_type capacity_v;

    bool change_capacity(size_type c)
            noexcept(std::is_nothrow_move_constructible<T>::value || std::is_nothrow_copy_constructible<T>::value)
    {
        T *new_storage = (T *)(new (std::nothrow) char[c * sizeof(T)]);
        if (new_storage == nullptr) return false;

        // To transfer, we prefer move unless it is throwing and copy exists
        svec_helper::move_helper<T>::move(array, new_storage, size_v);

        delete[] (char *)array;
        array = new_storage;
        capacity_v = c;

        return true;
    }

    bool ensure_capacity(size_type c, bool exact = false)
            noexcept(std::is_nothrow_move_constructible<T>::value || std::is_nothrow_copy_constructible<T>::value)
    {
        if (capacity_v >= c) return true;

        if (c > max_size()) return false;

        size_type new_capacity = c;
        if (!exact) {
            if (max_size() / 2 <= capacity_v) {
                new_capacity = max_size();
            }
            else {
                if (capacity_v == 0) {
                    // 4 elements seems a reasonable minimum capacity to begin with
                    new_capacity = 4;
                }
                else {
                    new_capacity = capacity_v * 2;
                }
            }
        }

        return change_capacity(new_capacity);
    }

public:

    svector() : array(nullptr), size_v(0), capacity_v(0)
    {

    }

    template <typename U = T, typename = typename std::enable_if<std::is_copy_constructible<U>::value>::type>
    svector(const svector<T> &other)
    {
        capacity_v = other.size_v;
        size_v = other.size_v;
        array = new T[capacity_v];
        for (size_t i = 0; i < size_v; i++) {
            new (&array[i].elem) T(other[i].elem);
        }
    }

    ~svector()
    {
        for (size_t i = 0; i < size_v; i++) {
            array[i].T::~T();
        }
        delete[] array;
    }

    template <typename U = T, typename = typename std::enable_if<std::is_copy_constructible<U>::value>::type>
    void push_back(const T &t)
    {
        if (!ensure_capacity(size_v + 1)) {
            throw std::bad_alloc();
        }
        new (&array[size_v].elem) T(t);
        size_v++;
    }

    template <typename U = T, typename = typename std::enable_if<std::is_move_constructible<U>::value>::type>
    void push_back(T &&t)
    {
        if (!ensure_capacity(size_v + 1)) {
            throw std::bad_alloc();
        }
        new (&array[size_v].elem) T(t);
        size_v++;
    }

    template <typename ...U>
    void emplace_back(U... args)
    {
        if (!ensure_capacity(size_v + 1)) {
            throw std::bad_alloc();
        }
        new (&array[size_v]) T(args...);
        size_v++;
    }

    void pop_back() noexcept
    {
        size_v--;
    }

    T &operator[](size_t index) noexcept
    {
        return array[index];
    }

    const T &operator[](size_t index) const noexcept
    {
        return array[index];
    }

    size_t size() const noexcept
    {
        return size_v;
    }

    size_t capacity() const noexcept
    {
        return capacity_v;
    }

    bool empty() const noexcept
    {
        return size_v == 0;
    }

    static size_t max_size() noexcept
    {
        return std::numeric_limits<size_type>::max() / sizeof(T);

        // if we were to support allocators:
        //size_t max = std::allocator_traits<std::allocator<char>>::max_size(std::allocator<T>());
        //return max / sizeof(T);
        //  (but not / sizeof(T) for C++17 apparently)
    }

    void reserve(size_t amount)
    {
        if (!ensure_capacity(amount, true)) {
            throw std::bad_alloc();
        }
    }

    void shrink_to(size_t amount)
    {
        if (capacity_v > amount) {
            change_capacity(amount);
        }
    }

    T &back() noexcept
    {
        return array[size_v - 1];
    }

    T* begin() noexcept
    {
        return array;
    }

    const T *begin() const noexcept
    {
        return array;
    }

    T* end() noexcept
    {
        return array + size_v;
    }

    const T *end() const noexcept
    {
        return array + size_v;
    }
};

} // namespace dasynq

#endif /* DASYNQ_SVEC_H_ */
