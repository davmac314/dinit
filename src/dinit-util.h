#ifndef DINIT_UTIL_H_INCLUDED
#define DINIT_UTIL_H_INCLUDED 1

#include <cerrno>

// Signal-safe read. Read and re-try if interrupted by signal (EINTR).
// *May* affect errno even on a successful read (when the return is less than n).
inline int ss_read(int fd, void * buf, size_t n)
{
    char * cbuf = static_cast<char *>(buf);
    int r = 0;
    while (r < n) {
        int res = read(fd, cbuf + r, n - r);
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
