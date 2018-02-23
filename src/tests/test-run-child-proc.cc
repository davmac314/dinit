#include "service.h"

// Stub out run_child_proc function, for testing purposes.

void service_record::run_child_proc(const char * const *args, const char *working_dir,
        const char *logfile, bool on_console, int wpipefd, int csfd, int socket_fd,
        uid_t uid, gid_t gid) noexcept
{

}
