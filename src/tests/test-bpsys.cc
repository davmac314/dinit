#include <vector>
#include <utility>
#include <algorithm>
#include <memory>
#include <map>
#include <string>
#include <iostream>

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

    read_result(const std::vector<char> &data_p) : errcode(0), data(data_p) {}
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

class fs_node
{
    public:
    virtual fs_node *resolve(const char *name) = 0;
    virtual fs_node *create_dir(const char *name) = 0;
    virtual fs_node *create_file(const char *name) = 0;
    virtual bool set_file_content(std::vector<char> &&content) = 0;
    virtual const std::vector<char> *get_file_content() = 0;
    virtual ~fs_node() {}
};

class file_fs_node : public fs_node
{
    std::vector<char> contents;

    public:
    fs_node *resolve(const char *name) override
    {
        errno = ENOTDIR;
        return nullptr;
    }

    fs_node *create_dir(const char *name) override
    {
        errno = ENOTDIR;
        return nullptr;
    }

    fs_node *create_file(const char *name) override
    {
        errno = ENOTDIR;
        return nullptr;
    }

    bool set_file_content(std::vector<char> &&content) override
    {
        contents = std::move(content);
        return true;
    }

    const std::vector<char> *get_file_content() override
    {
        return &contents;
    }
};

class dir_fs_node : public fs_node
{
    std::map<std::string, std::unique_ptr<fs_node>> entries;

    public:
    virtual fs_node *resolve(const char *name) override
    {
        auto i = entries.find(std::string(name));
        if (i == entries.end()) return nullptr;
        return i->second.get();
    }

    fs_node *create_dir(const char *name) override
    {
        auto i = entries.find(std::string(name));
        if (i != entries.end()) {
            errno = EEXIST;
            return nullptr;
        }

        dir_fs_node *new_hndlr = new dir_fs_node();
        entries[std::string(name)] = std::unique_ptr<fs_node>(new_hndlr);
        return new_hndlr;
    }

    fs_node *create_file(const char *name) override
    {
        auto i = entries.find(std::string(name));
        if (i != entries.end()) {
            errno = EEXIST;
            return nullptr;
        }

        file_fs_node *new_hndlr = new file_fs_node();
        entries[std::string(name)] = std::unique_ptr<fs_node>(new_hndlr);
        return new_hndlr;
    }

    bool set_file_content(std::vector<char> &&content) override
    {
        errno = EISDIR;
        return false;
    }

    const std::vector<char> *get_file_content() override
    {
        errno = EISDIR;
        return nullptr;
    }
};

// Handle operations on a file descriptor
class fd_handler
{
    public:
    // read call with fd
    virtual int read(void *buf, size_t size) = 0;
    // set blocking/non-blocking (affects end-of-input behaviour, EAGAIN vs EOF).
    virtual bool set_blocking(bool blocking) = 0;
    // supply data to be read from the fd
    virtual void supply_data(std::vector<char> &&data) = 0;
    // for resolving against (openat etc)
    virtual fs_node *get_fs_node() { return nullptr; }

    virtual ~fd_handler() {}
};

class file_fd_handler : public fd_handler
{
    public:
    read_cond rrs;

    file_fd_handler() {}

    file_fd_handler(fs_node *fsnode)
    {
        const auto *file_content_ptr = fsnode->get_file_content();
        if (file_content_ptr == nullptr) {
            abort();
        }

        rrs.push_back(read_result(*file_content_ptr));
    }

    int read(void *buf, size_t count) override
    {
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

    virtual void supply_data(std::vector<char> &&data) override
    {
        rrs.emplace_back(std::move(data));
    }

    virtual bool set_blocking(bool blocking_io) override
    {
        rrs.is_blocking = blocking_io;
        return true;
    }

    virtual ~file_fd_handler() override {}
};

class dir_fd_handler : public fd_handler
{
    fs_node *node;

    public:
    dir_fd_handler(fs_node *node_p) : node(node_p) { }

    int read(void *buf, size_t count) override
    {
        errno = EINVAL;
        return false;
    }

    bool set_blocking(bool blocking_io) override
    {
        errno = EINVAL;
        return false;
    }

    void supply_data(std::vector<char> &&data) override
    {
        abort();
    }

    fs_node *get_fs_node() override
    {
        return node;
    }
};


fs_node *current_dir_node = nullptr;
fs_node *root_dir_hndlr = nullptr;

// map of fd to fd handler
std::map<int,fd_handler *> fd_handlers;

// map of fd to the handler for writes to that fd
std::map<int, std::unique_ptr<bp_sys::write_handler>> write_hndlr_map;

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
void supply_read_data(int fd, std::vector<char> &&data)
{
    auto i = fd_handlers.find(fd);
    if (i == fd_handlers.end()) {
        auto *hndlr = new file_fd_handler();
        hndlr->rrs.emplace_back(std::move(data));
        fd_handlers[fd] = hndlr;
    }
    else {
        i->second->supply_data(std::move(data));
    }


}

void supply_read_data(int fd, std::vector<char> &data)
{
    std::vector<char> data_copy = data;
    supply_read_data(fd, std::move(data_copy));
}

void set_blocking(int fd)
{
    if (fd_handlers.count(fd) == 0) {
        abort();
    }
    fd_handlers[fd]->set_blocking(true);
}

// retrieve data written via write()
void extract_written_data(int fd, std::vector<char> &data)
{
    auto &whandler = write_hndlr_map[fd];
    if (whandler == nullptr) abort();
    default_write_handler *dwhndlr = static_cast<default_write_handler *>(whandler.get());
    data = std::move(dwhndlr->data);
}

// beginning at offset, look for the next segment in a path (skip any leading slashes)
static std::pair<size_t,size_t> next_path_seg(const std::string &path, size_t offset)
{
    while (offset < path.length() && path[offset] == '/') ++offset;
    size_t first = offset;
    while (offset < path.length() && path[offset] != '/') ++offset;
    size_t last = offset;
    return {first, last};
}

static fs_node *find_or_create_dir_file(const std::string &path, bool create_file = true)
{
    if (path.empty()) return nullptr;

    fs_node *base_hndlr;
    if (path[0] == '/') {
        // start at root
        if (root_dir_hndlr == nullptr) {
            root_dir_hndlr = new dir_fs_node();
        }
        base_hndlr = root_dir_hndlr;
    }
    else {
        // start at cwd
        if (current_dir_node == nullptr) {
            current_dir_node = new dir_fs_node();
        }
        base_hndlr = current_dir_node;
    }

    size_t offset = 0;
    do {
        auto next_seg = next_path_seg(path, offset);
        if (next_seg.first == next_seg.second) break;
        bool last_seg = (next_seg.second == path.length());
        offset = next_seg.second;

        std::string next_seg_str = path.substr(next_seg.first, next_seg.second - next_seg.first);
        auto *next_hndlr = base_hndlr->resolve(next_seg_str.c_str());
        if (next_hndlr == nullptr) {
            if (last_seg && create_file) {
                return base_hndlr->create_file(next_seg_str.c_str());
            }
            else {
                next_hndlr = base_hndlr->create_dir(next_seg_str.c_str());
            }
        }

        if (next_hndlr == nullptr) return nullptr;
        base_hndlr = next_hndlr;

    } while (true);

    return base_hndlr;
}

static fs_node *resolve_from(fs_node *base, const std::string &path)
{
    if (path.empty()) return base;

    size_t offset = 0;
    do {
        auto next_seg = next_path_seg(path, offset);
        if (next_seg.first == next_seg.second) break;
        offset = next_seg.second;

        std::string next_seg_str = path.substr(next_seg.first, next_seg.second - next_seg.first);
        auto *next_hndlr = base->resolve(next_seg_str.c_str());

        if (next_hndlr == nullptr) return nullptr;
        base = next_hndlr;

    } while (true);

    return base;
}

static fs_node *resolve_path(const std::string &path)
{
    if (path.empty()) return nullptr;

    fs_node *base_hndlr;
    if (path[0] == '/') {
        // start at root
        if (root_dir_hndlr == nullptr) {
            root_dir_hndlr = new dir_fs_node();
        }
        base_hndlr = root_dir_hndlr;
    }
    else {
        // start at cwd
        if (current_dir_node == nullptr) {
            current_dir_node = new dir_fs_node();
        }
        base_hndlr = current_dir_node;
    }

    return resolve_from(base_hndlr, path);
}

// Supply a file content
void supply_file_content(const std::string &path, std::vector<char> &&data)
{
    fs_node *node = find_or_create_dir_file(path);
    if (node == nullptr) {
        throw std::string("Can't supply file content for path");
    }

    node->set_file_content(std::move(data));
}

void supply_file_content(const std::string &path, const std::vector<char> &data)
{
    std::vector<char> data_copy = data;
    supply_file_content(path, std::move(data_copy));
}

void supply_file_content(const std::string &path, const std::string &data)
{
    std::vector<char> vec_data;
    size_t len = data.length();
    const char *data_cstr = data.c_str();

    vec_data.reserve(len);
    vec_data.insert(vec_data.end(), &data_cstr[0], &data_cstr[len]);
    supply_file_content(path, std::move(vec_data));
}

// Mock implementations of system calls:

int open(const char *pathname, int flags)
{
    fs_node *node = resolve_path(pathname);
    if (node == nullptr) {
        errno = ENOENT;
        return -1;
    }

    fd_handler *hndlr;
    if (node->get_file_content() != nullptr) {
        hndlr = new file_fd_handler(node);
    }
    else {
        hndlr = new dir_fd_handler(node);
    }

    int nfd = allocfd();
    fd_handlers[nfd] = hndlr;
    return nfd;
}

// TODO: Check for mode instead of partial implementation
int open(const char *pathname, int flags, mode_t mode)
{
    return open(pathname, flags);
}

int openat(int dirfd, const char *pathname, int flags)
{
    if (pathname[0] == '/') return open(pathname, flags);
    if (dirfd == AT_FDCWD) return open(pathname, flags);

    auto i = fd_handlers.find(dirfd);
    if (i == fd_handlers.end()) {
        abort();
        //errno = EBADF;
        //return -1;
    }

    fd_handler *fdh = i->second;
    fs_node *fsnode = fdh->get_fs_node();
    if (fsnode == nullptr) {
        errno = ENOTDIR;
        return -1;
    }

    fs_node *resolved = resolve_from(fsnode, pathname);
    if (resolved == nullptr) {
        errno = ENOENT;
        return -1;
    }

    fd_handler *hndlr = new file_fd_handler(resolved);

    int nfd = allocfd();
    fd_handlers[nfd] = hndlr;
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
    if (size_t(fd) >= usedfds.size() || !usedfds[fd]) {
        abort();
    }

    usedfds[fd] = false;
    write_hndlr_map.erase(fd);
    auto it = fd_handlers.find(fd);
    if (it != fd_handlers.end()) {
        delete it->second;
        fd_handlers.erase(it);
    }
    return 0;
}

int kill(pid_t pid, int sig)
{
    last_sig_sent = sig;
    return 0;
}

ssize_t read(int fd, void *buf, size_t count)
{
    auto hndlr_it = fd_handlers.find(fd);
    if (hndlr_it == fd_handlers.end()) {
        //errno = EBADF;
        //return -1;
        throw std::string("Use of bad file descriptor");
    }

    return hndlr_it->second->read(buf, count);
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
