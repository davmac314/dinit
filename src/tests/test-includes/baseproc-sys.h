#ifndef BPSYS_INCLUDED
#define BPSYS_INCLUDED

#include <string>
#include <vector>

#include <sys/types.h>
#include <unistd.h>
#include <sys/uio.h>

// Mock system functions for testing.

namespace bp_sys {

class write_handler
{
public:
    virtual ssize_t write(int fd, const void *buf, size_t count) = 0;
    virtual ~write_handler() { }
};

class default_write_handler : public write_handler
{
public:
    std::vector<char> data;

    virtual ssize_t write(int fd, const void *buf, size_t count)
    {
        data.insert(data.end(), (char *)buf, (char *)buf + count);
        return count;
    }
};

// Test helper functions:

void init_bpsys();

// allocate a file descriptor
int allocfd();
int allocfd(write_handler *hndlr);

void supply_read_data(int fd, std::vector<char> &data);
void supply_read_data(int fd, std::vector<char> &&data);
void set_blocking(int fd);
void extract_written_data(int fd, std::vector<char> &data);
void supply_file_content(const std::string &path, const std::vector<char> &data);
void supply_file_content(const std::string &path, std::vector<char> &&data);

// Mock system calls:

// implementations elsewhere:
int open(const char *pathname, int flags);
int pipe2(int pipefd[2], int flags);
int close(int fd);
int kill(pid_t pid, int sig);

inline int fcntl(int fd, int cmd, ...)
{
    // This is used for setting the CLOEXEC flag, we can just return 0:
    return 0;
}

inline pid_t getpgid(pid_t pid)
{
    return pid;
}

inline int tcsetpgrp(int fd, pid_t pgid)
{
    return 0;
}

inline pid_t getpgrp()
{
    return getpid();
}

class exit_status
{
    friend pid_t waitpid(pid_t, exit_status *, int);

    bool did_exit_v;
    bool was_signalled_v;
    int status;

    public:
    exit_status() : did_exit_v(true), was_signalled_v(false), status(0) { }

    // status_p is either the exit status or termination signal:
    exit_status(bool did_exit_p, bool was_signalled_p, int status_p)
        : did_exit_v(did_exit_p), was_signalled_v(was_signalled_p), status(status_p)
    { }

    explicit exit_status(int status_p)
    {
        throw std::string("initalised exit_status with integer argument");
    }

    bool did_exit()
    {
        return did_exit_v;
    }

    bool did_exit_clean()
    {
        return did_exit_v && status == 0;
    }

    bool was_signalled()
    {
        return was_signalled_v;
    }

    int get_exit_status()
    {
        return status;
    }

    int get_term_sig()
    {
        return status;
    }

    int as_int()
    {
        return -1;
    }
};

inline pid_t waitpid(pid_t p, exit_status *statusp, int flags)
{
    // throw std::string("not implemented");
    return 0; // TODO complete mock
}

ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
ssize_t writev (int fd, const struct iovec *iovec, int count);

extern char **environ;
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int clearenv();

}

#endif
