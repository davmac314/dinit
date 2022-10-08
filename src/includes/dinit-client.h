#include <cstdint>
#include <cstring>

#include <sys/types.h>
#include <pwd.h>

#include "mconfig.h"
#include "cpbuffer.h"
#include "control-cmds.h"

// Client library for Dinit clients


using handle_t = uint32_t;
using cpbuffer_t = cpbuffer<1024>;

class cp_read_exception
{
    public:
    int errcode;
    cp_read_exception(int err) : errcode(err) { }
};

class cp_write_exception
{
    public:
    int errcode;
    cp_write_exception(int err) : errcode(err) { }
};

class cp_old_client_exception
{
    // no body
};

class cp_old_server_exception
{
    // no body
};

class general_error
{
    int err; // related errno (or 0)
    const char *action; // may be nullptr if err != 0
    std::string arg; // may be empty, must be empty if action == nullptr

public:
    general_error(int err) : err(err), action(nullptr), arg()
    {
    }

    general_error(int err, const char *action, std::string arg = {}) : err(err), action(action), arg(std::move(arg))
    {
    }

    int get_err() { return err; }
    const char *get_action() { return action; }
    std::string &get_arg() { return arg; }
};

// static_membuf: a buffer of a fixed size (N) with one additional value (of type T). Don't use this
// directly, construct via membuf.
template <int N> class static_membuf
{
    public:
    static constexpr int size() { return N; }

    private:
    char buf[N];

    public:
    template <typename T>
    static_membuf(const T &val)
    {
        static_assert(sizeof(T) == N, "must initialise with object of correct size");
        memcpy(buf, &val, N);
    }

    template <int M, typename T>
    static_membuf(char (&prevbuf)[M], const T &val)
    {
        static_assert(M + sizeof(T) == N, "size is not correct");
        memcpy(buf, prevbuf, M);
        memcpy(buf + M, &val, sizeof(val));
    }

    const char *data() const { return buf; }

    template <typename U> static_membuf<N+sizeof(U)> append(const U &u)
    {
        return static_membuf<N+sizeof(U)>{buf, u};
    }

    void output(char *out)
    {
        memcpy(out, buf, size());
    }
};

// "membuf" class provides a compile-time allocated buffer that we can add items to one-by-one. This is
// much safer than working with raw buffers and calculating offsets and sizes by hand (and with a decent
// compiler the end result is just as efficient).
//
// To use:
//     auto m = membuf().append(value1).append(value2).append(value3);
// Then:
//     m.size() - returns total size of the buffer (sizeof(value1)+...)
//     m.data() - returns a 'const char *' to the buffer contents
class membuf
{
    public:

    template <typename U> static_membuf<sizeof(U)> append(const U &u)
    {
        return static_membuf<sizeof(U)>(u);
    }
};

// Fill a circular buffer from a file descriptor, until it contains at least _rlength_ bytes.
// Throws cp_read_exception if the requested number of bytes cannot be read, with:
//     errcode = 0   if end of stream (remote end closed)
//     errcode = errno   if another error occurred
// Note that EINTR is ignored (i.e. the read will be re-tried).
inline void fill_buffer_to(cpbuffer_t &buf, int fd, int rlength)
{
    do {
        int r = buf.fill_to(fd, rlength);
        if (r == -1) {
            if (errno != EINTR) {
                throw cp_read_exception(errno);
            }
        }
        else if (r == 0) {
            throw cp_read_exception(0);
        }
        else {
            return;
        }
    }
    while (true);
}

// Fill a circular buffer from a file descriptor, until it contains at least some more data
// then it did. Throws cp_read_exception if no more bytes can be read, with:
//     errcode = 0   if end of stream (remote end closed)
//     errcode = errno   if another error occurred
// Note that EINTR is ignored (i.e. the read will be re-tried).
inline void fill_some(cpbuffer_t &buf, int fd)
{
    while(true) {
        int r = buf.fill(fd);
        if (r == 0) {
            throw cp_read_exception(0);
        }
        else if (r > 0) {
            return;
        }

        if (errno != EINTR) {
            throw cp_read_exception(errno);
        }

        // if EINTR, just try again
    }
}

// Wait for a reply packet, skipping over any information packets that are received in the meantime.
// Note the reply packet may be partially read. Caller is responsible for reading full packet.
inline void wait_for_reply(cpbuffer_t &rbuffer, int fd)
{
    fill_buffer_to(rbuffer, fd, 1);

    while (rbuffer[0] >= 100) {
        // Information packet; discard.
        fill_buffer_to(rbuffer, fd, 2);
        int pktlen = (unsigned char) rbuffer[1];

        rbuffer.consume(1);  // Consume one byte so we'll read one byte of the next packet
        fill_buffer_to(rbuffer, fd, pktlen);
        rbuffer.consume(pktlen - 1);
    }
}

// Wait for an info packet. If any other reply packet comes, throw a cp_read_exception.
inline void wait_for_info(cpbuffer_t &rbuffer, int fd)
{
    fill_buffer_to(rbuffer, fd, 2);

    if (rbuffer[0] < 100) {
        throw cp_read_exception(0);
    }

    int pktlen = (unsigned char) rbuffer[1];
    fill_buffer_to(rbuffer, fd, pktlen);
}

// Write *all* the requested buffer and re-try if necessary until
// the buffer is written or an unrecoverable error occurs.
// Note: count is int
inline int write_all(int fd, const void *buf, int count)
{
    const char *cbuf = static_cast<const char *>(buf);
    int w = 0;
    while (count > 0) {
        int r = write(fd, cbuf, count);
        if (r == -1) {
            if (errno == EINTR) continue;
            return (count > 0) ? count : r;
        }
        w += r;
        cbuf += r;
        count -= r;
    }
    return w;
}

// Write all the requested buffer, and throw an exception on failure.
// Note: count is int
inline void write_all_x(int fd, const void *buf, int count)
{
    if (write_all(fd, buf, count) == -1) {
        throw cp_write_exception(errno);
    }
}

// Write all the requested buffer (eg membuf) and throw an exception on failure.
template <typename Buf> inline void write_all_x(int fd, const Buf &b)
{
    write_all_x(fd, b.data(), b.size());
}

// Check the protocol version is compatible with the client.
//   minversion - minimum protocol version that client can speak
//   version - maximum protocol version that client can speak
//   rbuffer, fd -  communication buffer and socket
// returns: the actual protocol version
// throws an exception on protocol mismatch or error.
inline uint16_t check_protocol_version(int minversion, int version, cpbuffer_t &rbuffer, int fd)
{
    constexpr int bufsize = 1;
    char buf[bufsize] = { DINIT_CP_QUERYVERSION };
    write_all_x(fd, buf, bufsize);

    wait_for_reply(rbuffer, fd);
    if (rbuffer[0] != DINIT_RP_CPVERSION) {
        throw cp_read_exception{0};
    }

    // DINIT_RP_CVERSION, (2 byte) minimum compatible version, (2 byte) actual version
    constexpr int rbufsize = 1 + 2 * sizeof(uint16_t);
    fill_buffer_to(rbuffer, fd, rbufsize);
    uint16_t rminversion;
    uint16_t cpversion;

    rbuffer.extract(reinterpret_cast<char *>(&rminversion), 1, sizeof(uint16_t));
    rbuffer.extract(reinterpret_cast<char *>(&cpversion), 1 + sizeof(uint16_t), sizeof(uint16_t));
    rbuffer.consume(rbufsize);

    if (rminversion > version) {
        // We are too old
        throw cp_old_client_exception();
    }
    if (cpversion < minversion) {
        // Server is too old
        throw cp_old_server_exception();
    }

    return cpversion;
}

// Get the default socket path (i.e. the path to use if no path is explicitly specified).
// 'control_socket_str' *may* be used for storage for the returned path.
// 'user_dinit' should be true if getuid() == 0.
inline const char *get_default_socket_path(std::string &control_socket_str, bool user_dinit)
{
    const char *control_socket_path;
    const char *sockpath = getenv("DINIT_SOCKET_PATH");
    if (sockpath) {
        control_socket_str = sockpath;
        control_socket_path = control_socket_str.c_str();
    }
    else if (user_dinit) {
        const char * rundir = getenv("XDG_RUNTIME_DIR");
        const char * sockname = "dinitctl";
        if (rundir == nullptr) {
            sockname = ".dinitctl";
            rundir = getenv("HOME");
            if (rundir == nullptr) {
                struct passwd * pwuid_p = getpwuid(getuid());
                if (pwuid_p != nullptr) {
                    rundir = pwuid_p->pw_dir;
                }
            }
        }

        if (rundir != nullptr) {
            control_socket_str = rundir;
            control_socket_str.push_back('/');
            control_socket_str += sockname;
            control_socket_path = control_socket_str.c_str();
        }
        else {
            return nullptr;
        }
    }
    else {
        control_socket_path = SYSCONTROLSOCKET; // default to system
    }
    return control_socket_path;
}

// Connect to the dinit daemon, return the connected socket fd.
// Throws general_error on error.
inline int connect_to_daemon(const char *control_socket_path)
{
    int socknum = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socknum == -1) {
        throw general_error(errno, "opening socket");
    }

    struct sockaddr_un * name;
    uint sockaddr_size = offsetof(struct sockaddr_un, sun_path) + strlen(control_socket_path) + 1;
    name = (struct sockaddr_un *) malloc(sockaddr_size);
    if (name == nullptr) {
        throw general_error(ENOMEM);
    }

    name->sun_family = AF_UNIX;
    strcpy(name->sun_path, control_socket_path);

    int connr = connect(socknum, (struct sockaddr *) name, sockaddr_size);
    free(name);

    if (connr == -1) {
        throw general_error(errno, "connecting to socket", control_socket_path);
    }

    return socknum;
}
