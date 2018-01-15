#include <vector>
#include <utility>
#include <algorithm>

#include "baseproc-sys.h"

std::vector<bool> usedfds = {true, true, true};

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

int pipe2(int fds[2], int flags)
{
    fds[0] = allocfd();
    fds[1] = allocfd();
    return 0;
}

int close(int fd)
{
    if (fd >= usedfds.size()) abort();

    usedfds[fd] = false;
    return 0;
}

}
