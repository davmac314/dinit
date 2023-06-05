#include <vector>
#include <utility>
#include <algorithm>
#include <memory>
#include <map>

#include <cstdlib>
#include <cstring>
#include <cerrno>

#include "baseproc-sys.h"

namespace {

// which simulated file descriptors are currently "open"
std::vector<bool> usedfds = {true, true, true};

struct read_result
{
    read_result(int errcode_p) : errcode(errcode_p) {}

    read_result(std::vector<char> &data_p) : errcode(0), data(data_p) {}
    read_result(std::vector<char> &&data_p) : errcode(0), data(std::move(data_p)) {}

    int errcode; // errno return
    std::vector<char> data;  // data (if errcode == 0)
};

class read_cond : public std::vector<read_result>
{
    public:
    using vector<read_result>::vector;

    // if blocking, return EAGAIN rather than end-of-file:
    bool is_blocking = false;
};

// map of fd to read results to supply for reads of that fd
std::map<int,read_cond> read_data;

// map of fd to the handler for writes to that fd
std::map<int, std::unique_ptr<bp_sys::write_handler>> write_hndlr_map;

// map of path to file content
std::map<std::string, std::vector<char>> file_content_map;

// environment variables, in "NAME=VALUE" form
std::vector<char *> env_vars;

} // anon namespace

namespace bp_sys {

char **environ = nullptr;

int last_sig_sent = -1; // last signal number sent, accessible for tests.
pid_t last_forked_pid = 1;  // last forked process id (incremented each 'fork')

// Test helper methods:

void init_bpsys()
{
    write_hndlr_map[0] = std::unique_ptr<bp_sys::write_handler> { new default_write_handler() };
    write_hndlr_map[1] = std::unique_ptr<bp_sys::write_handler> { new default_write_handler() };
    write_hndlr_map[2] = std::unique_ptr<bp_sys::write_handler> { new default_write_handler() };
}

// Allocate a file descriptor
int allocfd()
{
    return allocfd(new default_write_handler());
}

int allocfd(write_handler *whndlr)
{
    auto f = std::find(usedfds.begin(), usedfds.end(), false);
    if (f == usedfds.end()) {
        int r = usedfds.size();
        usedfds.push_back(true);
        write_hndlr_map[r] = std::unique_ptr<bp_sys::write_handler>(whndlr);
        return r;
    }

    *f = true;
    auto r = f - usedfds.begin();
    write_hndlr_map[r] = std::unique_ptr<bp_sys::write_handler>(whndlr);
    return r;
}

// Supply data to be returned by read()
void supply_read_data(int fd, std::vector<char> &data)
{
    read_data[fd].emplace_back(data);
}

void supply_read_data(int fd, std::vector<char> &&data)
{
    read_data[fd].emplace_back(std::move(data));
}

void set_blocking(int fd)
{
    read_data[fd].is_blocking = true;
}

// retrieve data written via write()
void extract_written_data(int fd, std::vector<char> &data)
{
    auto &whandler = write_hndlr_map[fd];
    if (whandler == nullptr) abort();
    default_write_handler *dwhndlr = static_cast<default_write_handler *>(whandler.get());
    data = std::move(dwhndlr->data);
}

// Supply a file content
void supply_file_content(const std::string &path, std::vector<char> &data)
{
    file_content_map[path] = data;
}

void supply_file_content(const std::string &path, std::vector<char> &&data)
{
    file_content_map[path] = std::move(data);
}

// Mock implementations of system calls:

int open(const char *pathname, int flags)
{
    auto i = file_content_map.find(pathname);
    if (i == file_content_map.end()) {
        errno = ENOENT;
        return -1;
    }

    int nfd = allocfd();
    supply_read_data(nfd, i->second);
    return nfd;
}

int pipe2(int fds[2], int flags)
{
    fds[0] = allocfd();
    fds[1] = allocfd();
    return 0;
}

int close(int fd)
{
    if (size_t(fd) >= usedfds.size()) abort();
    if (! usedfds[fd]) abort();

    usedfds[fd] = false;
    write_hndlr_map.erase(fd);
    return 0;
}

int kill(pid_t pid, int sig)
{
    last_sig_sent = sig;
    return 0;
}

ssize_t read(int fd, void *buf, size_t count)
{
    read_cond & rrs = read_data[fd];
    if (rrs.empty()) {
        if (rrs.is_blocking) {
            errno = EAGAIN;
            return -1;
        }
        return 0;
    }

    read_result &rr = rrs.front();
    if (rr.errcode != 0) {
        errno = rr.errcode;
        // Remove the result record:
        auto i = rrs.begin();
        i++;
        rrs.erase(rrs.begin(), i);
        return -1;
    }

    auto dsize = rr.data.size();
    if (dsize <= count) {
        // Consume entire result:
        std::copy_n(rr.data.begin(), dsize, (char *)buf);
        // Remove the result record:
        rrs.erase(rrs.begin());
        return dsize;
    }

    // Consume partial result:
    std::copy_n(rr.data.begin(), count, (char *)buf);
    rr.data.erase(rr.data.begin(), rr.data.begin() + count);
    return count;
}

ssize_t write(int fd, const void *buf, size_t count)
{
    return write_hndlr_map[fd]->write(fd, buf, count);
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t r = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t wr = write(fd, iov[i].iov_base, iov[i].iov_len);
        if (wr < 0) {
            if (r > 0) {
                return r;
            }
            return wr;
        }
        r += wr;
        if (size_t(wr) < iov[i].iov_len) {
            return r;
        }
    }
    return r;
}

char *getenv(const char *name)
{
    size_t name_len = strlen(name);
    for (auto *var : env_vars) {
        if (strncmp(name, var, name_len) == 0) {
            if (var[name_len] == '=') {
                return &(var[name_len + 1]);
            }
        }
    }
    return nullptr;
}

int setenv(const char *name, const char *value, int overwrite)
{
    size_t name_len = strlen(name);
    if (env_vars.empty()) {
        env_vars.push_back(nullptr);
    }
    else for (unsigned i = 0; i < (env_vars.size() - 1); ++i) {
        if (strncmp(name, env_vars[i], name_len) == 0) {
            if (env_vars[i][name_len] == '=') {
                // name matches, replace the value
                if (overwrite) {
                    delete[](env_vars[i]);
                    env_vars[i] = new char[name_len + 1 + strlen(value) + 1];
                    strcpy(env_vars[i], name);
                    env_vars[i][name_len] = '=';
                    strcpy(env_vars[i] + name_len + 1, value);
                }
                return 0;
            }
        }
    }

    // not found, add
    char *new_var = new char[name_len + 1 + strlen(value) + 1];
    strcpy(new_var, name);
    new_var[name_len] = '=';
    strcpy(new_var + name_len + 1, value);
    env_vars[env_vars.size() - 1] = new_var;
    env_vars.push_back(nullptr);
    environ = env_vars.data();
    return 0;
}

int clearenv()
{
    for (char *env_var : env_vars) {
        delete[](env_var);
    }
    env_vars.clear();
    environ = nullptr;
    return 0;
}

}
