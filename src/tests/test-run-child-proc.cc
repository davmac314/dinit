#include <vector>

#include "proc-service.h"

// Stub out run_child_proc function, for testing purposes.

void base_process_service::run_child_proc(const char * const *args, const char *working_dir,
        const char *logfile, bool on_console, int wpipefd, int csfd, int socket_fd,
        int notify_fd, int forced_notify_fd, const char * notify_var, uid_t uid, gid_t gid,
        const std::vector<service_rlimits> &rlimits) noexcept
{

}
