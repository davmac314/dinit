#ifndef DASYNQ_UTIL_H_
#define DASYNQ_UTIL_H_

#include <unistd.h>

#include "config.h"

namespace dasynq {

// Define pipe2, if it's not present in the sytem library. pipe2 is like pipe with an additional flags
// argument which can set file/descriptor flags atomically. The emulated version that we generate cannot
// do this atomically, of course.

#if DASYNQ_HAVE_PIPE2 == 0
inline int pipe2(int filedes[2], int flags)
{
    if (pipe(filedes) == -1) {
        return -1;
    }

    if (flags & O_CLOEXEC) {
        fcntl(filedes[0], F_SETFD, FD_CLOEXEC);
        fcntl(filedes[1], F_SETFD, FD_CLOEXEC);
    }

    if (flags & O_NONBLOCK) {
        fcntl(filedes[0], F_SETFL, O_NONBLOCK);
        fcntl(filedes[1], F_SETFL, O_NONBLOCK);
    }

    return 0;
}
#else

using ::pipe2;

#endif

} // namespace dasynq

#endif /* DASYNQ_UTIL_H_ */
