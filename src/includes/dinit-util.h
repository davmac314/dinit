#ifndef DINIT_UTIL_H_INCLUDED
#define DINIT_UTIL_H_INCLUDED 1

#include <string>

#include <cstddef>
#include <cerrno>

#include <sys/types.h>
#include <unistd.h>

#include "baseproc-sys.h"

// Complete read - read the specified size until end-of-file or error; continue read if
// interrupted by signal.
inline ssize_t complete_read(int fd, void * buf, size_t n)
{
    char * cbuf = static_cast<char *>(buf);
    ssize_t r = 0;
    while ((size_t)r < n) {
        ssize_t res = bp_sys::read(fd, cbuf + r, n - r);
        if (res == 0) {
            return r;
        }
        if (res < 0) {
            if (errno == EINTR) {
                continue;
            }

            // If any other error, and we have successfully read some, return it:
            if (r == 0) {
                return -1;
            }
            else {
                return r;
            }
        }
        r += res;
    }
    return n;
}

// Combine two paths to produce a path. If the second path is absolute, it is returned unmodified;
// otherwise, it is appended to the first path (with a slash separator added if needed).
inline std::string combine_paths(const std::string &p1, const char * p2)
{
    if (*p2 == 0) return p1;
    if (p1.empty()) return std::string(p2);

    if (p2[0] == '/') return p2;

    if (*(p1.rbegin()) == '/') return p1 + p2;
    return p1 + '/' + p2;
}

// Find the parent path of a given path, which should refer to a named file or directory (not . or ..).
// If the path contains no directory, returns the empty string.
inline std::string parent_path(const std::string &p)
{
    auto spos = p.rfind('/');
    if (spos == std::string::npos) {
        return std::string {};
    }

    return p.substr(0, spos + 1);
}

// Find the base name of a path (the name after the final '/').
inline const char * base_name(const char *path)
{
    const char * basen = path;
    const char * s = path;
    while (*s != 0) {
        if (*s == '/') basen = s + 1;
        s++;
    }
    return basen;
}

// Check if one string starts with another
inline bool starts_with(std::string s, const char *prefix)
{
    const char * sp = s.c_str();
    while (*sp != 0 && *prefix != 0) {
        if (*sp != *prefix) return false;
        sp++; prefix++;
    }
    return *prefix == 0;
}

#endif
