#ifndef DASYNQ_SVEC_H_INCLUDED
#define DASYNQ_SVEC_H_INCLUDED

#include <limits>
#include <utility>
#include <new>

// Vector with possibility to shrink capacity arbitrarily.
//
// The standard vector (std::vector) only allows shrinking a vector's capacity to its current size. In cases
// where we need to keep some reserved capacity beyond the current size, we need an alternative solution: hence,
// this class, svector.

namespace dasynq {

template <typename T>
class svector
{
    private:
    union vec_node {
        T elem;

        vec_node() { }
    };

    vec_node * array;
    size_t size_v;
    size_t capacity_v;

    void check_capacity()
    {
        if (size_v == capacity_v) {
            // double capacity now:
            if (capacity_v == 0) capacity_v = 1;
            vec_node * new_array = new vec_node[capacity_v * 2];
            for (size_t i = 0; i < size_v; i++) {
                new (&new_array[i].elem) T(std::move(array[i].elem));
                array[i].elem.T::~T();
            }
            delete[] array;
            array = new_array;
            capacity_v *= 2;
        }
    }

    public:
    using size_type = size_t;

    svector() : array(nullptr), size_v(0), capacity_v(0)
    {

    }

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
            array[i].elem.T::~T();
        }
        delete[] array;
    }

    void push_back(const T &t)
    {
        check_capacity();
        new (&array[size_v].elem) T(t);
        size_v++;
    }

    void push_back(T &&t)
    {
        check_capacity();
        new (&array[size_v].elem) T(t);
        size_v++;
    }

    template <typename ...U>
    void emplace_back(U... args)
    {
        check_capacity();
        new (&array[size_v].elem) T(args...);
        size_v++;
    }

    void pop_back()
    {
        size_v--;
    }

    T &operator[](size_t index)
    {
        return array[index].elem;
    }

    const T &operator[](size_t index) const
    {
        return array[index].elem;
    }

    size_t size() const
    {
        return size_v;
    }

    size_t capacity() const
    {
        return capacity_v;
    }

    bool empty() const
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
        if (capacity_v < amount) {
            vec_node * new_array = new vec_node[amount];
            for (size_t i = 0; i < size_v; i++) {
                new (&new_array[i].elem) T(std::move(array[i].elem));
                array[i].elem.T::~T();
            }
            delete[] array;
            array = new_array;
            capacity_v = amount;
        }
    }

    void shrink_to(size_t amount)
    {
        if (capacity_v > amount) {
            vec_node * new_array = new(std::nothrow) vec_node[amount];
            if (new_array == nullptr) {
                return;
            }
            for (size_t i = 0; i < size_v; i++) {
                new (&new_array[i].elem) T(std::move(array[i].elem));
                array[i].elem.T::~T();
            }
            delete[] array;
            array = new_array;
            capacity_v = amount;
        }
    }

    T &back()
    {
        return array[size_v - 1].elem;
    }

    T* begin()
    {
        return reinterpret_cast<T *>(array);
    }

    const T *begin() const
    {
        return reinterpret_cast<const T *>(array);
    }

    T* end()
    {
        return reinterpret_cast<T *>(array + size_v);
    }

    const T *end() const
    {
        return reinterpret_cast<const T *>(array + size_v);
    }
};


} // namespace

#endif
