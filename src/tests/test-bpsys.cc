#include <vector>
#include <utility>
#include <algorithm>

#include <cstdlib>

#include "baseproc-sys.h"

static std::vector<bool> usedfds = {true, true, true};

// Allocate a file descriptor
static int allocfd()
{
    auto f = std::find(usedfds.begin(), usedfds.end(), false);
    if (f == usedfds.end()) {
        int r = usedfds.size();
        usedfds.push_back(true);
        return r;
    }

    *f = true;
    return f - usedfds.begin();
}

namespace bp_sys {

int last_sig_sent = -1; // last signal number sent, accessible for tests.
pid_t last_forked_pid = 1;  // last forked process id (incremented each 'fork')

int pipe2(int fds[2], int flags)
{
    fds[0] = allocfd();
    fds[1] = allocfd();
    return 0;
}

int close(int fd)
{
    if (size_t(fd) >= usedfds.size()) abort();

    usedfds[fd] = false;
    return 0;
}

int kill(pid_t pid, int sig)
{
    last_sig_sent = sig;
    return 0;
}

}
