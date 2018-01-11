#ifndef DINIT_UTIL_H_INCLUDED
#define DINIT_UTIL_H_INCLUDED 1

#include <cstddef>
#include <cerrno>

#include <sys/types.h>
#include <unistd.h>

// Signal-safe read. Read and re-try if interrupted by signal (EINTR).
// *May* affect errno even on a successful read (when the return is less than n).
inline ssize_t ss_read(int fd, void * buf, size_t n)
{
    char * cbuf = static_cast<char *>(buf);
    ssize_t r = 0;
    while ((size_t)r < n) {
        ssize_t res = read(fd, cbuf + r, n - r);
        if (res == 0) {
            return r;
        }
        if (res < 0) {
            if (res == EINTR) {
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

#endif
