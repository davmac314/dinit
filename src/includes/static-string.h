// This is a minimal compile-time string handling library (currently just handling concatenation)
// which owes much to Andrzej Krzemieński, and his blog post:
//
//   https://akrzemi1.wordpress.com/2017/06/28/compile-time-string-concatenation/
//
// However, the approach is not exactly the same and is perhaps a tiny bit simpler.
//
// Provided are two templates: static_string<N> and array_string<N>. The first is a direct wrapper
// around character string literals and instances can be created using the "literal" function:
//
//   constexpr auto str = literal("this will result in a static_string");
//
// The array_string<N> type is similar but can be produced as the result of concatenation of other
// compile-time strings:
//
//   constexpr auto str = literal("one") + literal("two");   // array_string containing "onetwo"
//   constexpr auto str3 = literal("one") + "two";           // same
//
// To use the string at run time, call the c_str() function to obtain a pointer to the contained
// string, or simply assign or cast to a 'const char *':
//
//   const char * cstr = str3; // now points to "onetwo"
//
// Note that to be most effective, it is necessary to construct literals at file/namespace scope.
// If they are constructed as locals, the compiler may reconstruct the object each time the
// function is called (i.e. it will allocate stack and copy the constant string result into it).

namespace cts {

// A static string, designed as a wrapper around string literals:
template <int N>
class static_string
{
    const char (&lit)[N+1];

    public:
    constexpr static_string(const char (&lit_p)[N+1]) : lit(lit_p) {}
    constexpr char operator[](int i) const { return lit[i]; }
    constexpr const char * c_str() const { return lit; }

    operator const char*() const
    {
        return c_str();
    }

    constexpr static int length = N;
};

template <int N>
constexpr static_string<N-1> literal(const char (&lit)[N])
{
    return static_string<N-1>(lit);
}


// A sequence of integers, designed to look like:
//     sequence<0,1,2,3,...>:
template <int ...N>
class sequence { };

// A utility to append one element to a sequence:
template <class S>
class extend_sequence;

template <int ...N>
class extend_sequence<sequence<N...>>
{
    public:
    using t = sequence<N..., sizeof...(N)>;
};

// A utility to construct a sequence from 0 .. N:
template <int N>
class make_sequence;

template <>
class make_sequence<0>
{
    public:
    using t = sequence<>;
};

template <int N>
class make_sequence
{
    static_assert(N >= 0, "N must be >= 0");

    public:
    using t = typename extend_sequence<typename make_sequence<N - 1>::t>::t;
};

// forward declaration:
// extract a single character by index from the join of two strings
template <typename S1, typename S2>
constexpr char joined_index(const S1 &s1, const S2 &s2, int i);

// get the length of a compile-time static string
template <typename T>
struct static_length_t
{
    constexpr static int len = T::length;
};

template <int N>
struct static_length_t<char [N]>
{
    constexpr static int len = N - 1;
};

template <typename T>
constexpr int static_length()
{
    return static_length_t<T>::len;
}


// A compile-time string
template <int N>
class array_string
{
    const char arr[N+1];

    template <typename S1, typename S2, int... S> constexpr
    array_string(const S1 &s1, const S2 &s2, const sequence<S...> seq)
        : arr{joined_index(s1, s2, S)...}
    {}

    template <int... S> constexpr
    array_string(const static_string<N> &src, const sequence<S...> seq)
        : arr{src.c_str()[S]...}
    {}

    public:
    // construct array_string from static_string
    constexpr array_string(const static_string<N> &src)
        : array_string(src, typename make_sequence<N>::t {})
    {}

    template <typename S1, typename S2>
    constexpr array_string(const S1 &a, const S2 &b)
        : array_string(a, b, typename make_sequence<static_length<S1>() + static_length<S2>()>::t {})
    {}

    static constexpr int length = N;

    constexpr char operator[](int i) const
    {
        return arr[i];
    }

    const char *c_str() const { return arr; }

    operator const char*() const
    {
        return c_str();
    }
};

template <typename S1, typename S2>
constexpr char joined_index(const S1 &s1, const S2 &s2, int i)
{
    return (i < S1::length) ? s1[i] : s2[i - S1::length];
}

// Allow concatenating array_string and static_string with any compile-time constant string
// (including character string literal):

template <int N, typename S2> constexpr
array_string<N+static_length<S2>()> operator+(const array_string<N> &s1, const S2 &s2)
{
    return array_string<N+static_length<S2>()>(s1, s2);
}

template <int N, typename S2> constexpr
array_string<N+static_length<S2>()> operator+(const static_string<N> &s1, const S2 &s2)
{
    return array_string<N+static_length<S2>()>(s1, s2);
}


}  // end "cts" namespace
