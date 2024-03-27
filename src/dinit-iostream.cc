#include <limits>
#include <cstdio>
#include <cstring>

#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

#include <dinit-iostream.h>
#include <baseproc-sys.h>

// See dinit-iostream.h for inforamtion about the usage

namespace {

// A simple trick for calculating how much digits can be in a type with compile-time integer
// logarithm like function.
template <typename T> constexpr unsigned TYPE_MAX_NUMDIGITS(T num = std::numeric_limits<T>::max(),
        unsigned pow = 0)
{
    return (num == 0) ? pow : TYPE_MAX_NUMDIGITS(num / 10, pow + 1);
}

// We use <unsigned T> type for signed numeric types, because unsigned limit is better
// for calculating how much digits can be in a type.
// For instance when "long" is 64-bits, printing LONG_MIN will missing last "8" digit in
// "-9223372036854775808" if <unsigned>.max() get used instead of <unsigned long>.
constexpr unsigned UINT_NUMDIGITS = TYPE_MAX_NUMDIGITS<unsigned>() + 1;
constexpr unsigned INT_NUMDIGITS = UINT_NUMDIGITS;
constexpr unsigned ULONG_NUMDIGITS = TYPE_MAX_NUMDIGITS<unsigned long>() + 1;
constexpr unsigned LONG_NUMDIGITS = ULONG_NUMDIGITS;

} /* anonymous namespace */

namespace dio {

// class io_base
streambuf *io_base::get_buf() noexcept
{
    return buf.get();
}

bool io_base::is_open() noexcept
{
    return (fd >= 0);
}

void io_base::set_fd(const int newfd) noexcept
{
    fd = newfd;
}

int io_base::get_fd() noexcept
{
    return fd;
}

// class ostream
ssize_t ostream::put(const char *msg, size_t count) noexcept
{
    if (!good()) {
        return -1;
    }

    // If the current buffer space is not enough to store the message, we fill the buffer as much
    // as possible and subtract the number of characters used to fill the buffer from the "count"
    // and add it to this variable.
    size_t pre_buffered_chars = 0;

    if (count == 0) {
        return 0; // Null/Empty message
    }
    else if (count > buf->get_size()) {
        ssize_t r = flush_nx();
        if (r < 0) {
            // io_error was set by flush_nx() call
            return -1;
        }
        ssize_t res = bp_sys::write(fd, msg, count);
        if (res < 0) {
            io_error = errno;
            return -1;
        }
        return res;
    }
    else while (count > static_cast<size_t>(buf->get_free())) {
        // If we haven't enough storage for caputring the message Firstly we try to fill buffer as
        // much as possible and then write the buffer.
        int prev_freespace = buf->get_free();
        buf->append(msg, buf->get_free());
        msg += prev_freespace;
        pre_buffered_chars += prev_freespace;
        count -= prev_freespace;
        ssize_t r = flush_nx();
        if (r < 0) {
            // io_error was set by flush_nx()
            return (pre_buffered_chars > 0) ? pre_buffered_chars : -1;
        }
    }

    buf->append(msg, count);

    return pre_buffered_chars + count;
}

void ostream::throw_exception_on(const unsigned states)
{
    if ((states & io_states::buffer_failbit) && buffer_failure()) {
        throw iostream_internal_err("Cannot use stream's buffer: buffer is nullptr!");
    }
    if ((states & io_states::io_failbit) && io_failure()) {
        throw iostream_system_err(io_failure());
    }
}

bool ostream::open_nx(const char *path) noexcept
{
    fd = bp_sys::open(path, O_WRONLY);

    if (fd < 0) {
        io_error = errno;
        return false;
    }
    return true;
}

bool ostream::open(const char *path)
{
    bool r = open_nx(path);
    if (io_failure()) {
        throw iostream_system_err(io_failure());
    }
    return r;
}

bool ostream::open_nx(const char *path, const int flags) noexcept
{
    fd = bp_sys::open(path, O_WRONLY | flags);

    if (fd < 0) {
        io_error = errno;
        return false;
    }
    return true;
}

bool ostream::open(const char *path, const int flags)
{
    bool r = open_nx(path, flags);
    if (io_failure()) {
        throw iostream_system_err(io_failure());
    }
    return r;
}

bool ostream::open_nx(const char *path, const int flags, const mode_t mode) noexcept
{
    fd = bp_sys::open(path, O_WRONLY | flags, mode);

    if (fd < 0) {
        io_error = errno;
        return false;
    }
    return true;
}

bool ostream::open(const char *path, const int flags, const mode_t mode)
{
    bool r = open_nx(path, flags, mode);
    if (io_failure()) {
        throw iostream_system_err(io_failure());
    }
    return r;
}

bool ostream::close_nx() noexcept
{
    if (flush_nx() < 0) {
        // io_error was set by flush_nx() call
        return false;
    }
    if (bp_sys::close(fd) < 0) {
        io_error = errno;
        return false; // Failed
    }

    fd = -1;
    return true;
}

bool ostream::close()
{
    bool r = close_nx();
    if (!r) {
        throw_exception_on(io_states::buffer_failbit | io_states::io_failbit);
    }
    return r;
}

unsigned ostream::current_state() noexcept
{
    unsigned bits = 0;
    if (buffer_failed) bits |= io_states::buffer_failbit;
    if (io_error) bits |= io_states::io_failbit;
    return bits;
}

bool ostream::good() noexcept
{
    return (current_state() == 0);
}

bool ostream::buffer_failure() noexcept
{
    return buffer_failed;
}

int ostream::io_failure() noexcept
{
    return io_error;
}

void ostream::clear() noexcept
{
    // Reset all failures
    buffer_failed = false;
    io_error = 0;
}

ssize_t ostream::flush_nx() noexcept
{
    if (!good()) {
        return -1;
    }
    ssize_t r = 0;
    while (buf->get_length() > 0) {
        char *ptr = buf->get_ptr(0);
        unsigned len = buf->get_contiguous_length(ptr);
        r = bp_sys::write(fd, ptr, len);

        if (r < 0) {
            io_error = errno;
            return -1;
        }
        buf->consume(r);
    }

    return r;
}

ssize_t ostream::flush()
{
    ssize_t r = flush_nx();
    if (r < 0) {
        throw_exception_on(io_states::buffer_failbit | io_states::io_failbit);
    }
    return r;
}

bool ostream::write_nx(const char *msg) noexcept
{
    return (put(msg, strlen(msg)) >= 0) ? true : false;
}

bool ostream::write(const char *msg)
{
    bool r = write_nx(msg);
    if (!r) {
        throw_exception_on(io_states::buffer_failbit | io_states::io_failbit);
    }
    return r;
}

bool ostream::write_nx(char msg) noexcept
{
    return (put(&msg, 1) >= 0);
}

bool ostream::write(char msg)
{
    bool r = write_nx(msg);
    if (!r) {
        throw_exception_on(io_states::buffer_failbit | io_states::io_failbit);
    }
    return r;
}

bool ostream::write_nx(const std::string &msg) noexcept
{
    return (put(msg.c_str(), msg.size()) >= 0);
}

bool ostream::write(const std::string &msg)
{
    bool r = write_nx(msg);
    if (!r) {
        throw_exception_on(io_states::buffer_failbit | io_states::io_failbit);
    }
    return r;
}

bool ostream::write_nx(const int num) noexcept
{
    char tmp[INT_NUMDIGITS];
    int len = snprintf(tmp, INT_NUMDIGITS, "%d", num);
    return (put(tmp, len) >= 0);
}

bool ostream::write(const int num)
{
    bool r = write_nx(num);
    if (!r) {
        throw_exception_on(io_states::buffer_failbit | io_states::io_failbit);
    }
    return r;
}

bool ostream::write_nx(const unsigned num) noexcept
{
    char tmp[UINT_NUMDIGITS];
    int len = snprintf(tmp, UINT_NUMDIGITS, "%d", num);
    return (put(tmp, len) >= 0);
}

bool ostream::write(const unsigned num)
{
    bool r = write_nx(num);
    if (!r) {
        throw_exception_on(io_states::buffer_failbit | io_states::io_failbit);
    }
    return r;
}

bool ostream::write_nx(const long num) noexcept
{
    char tmp[LONG_NUMDIGITS];
    int len = snprintf(tmp, LONG_NUMDIGITS, "%ld", num);
    return (put(tmp, len) >= 0);
}

bool ostream::write(const long num)
{
    bool r = write_nx(num);
    if (!r) {
        throw_exception_on(io_states::buffer_failbit | io_states::io_failbit);
    }
    return r;
}

bool ostream::write_nx(const unsigned long num) noexcept
{
    char tmp[ULONG_NUMDIGITS];
    int len = snprintf(tmp, ULONG_NUMDIGITS, "%ld", num);
    return (put(tmp, len) >= 0);
}

bool ostream::write(const unsigned long num)
{
    bool r = write_nx(num);
    if (!r) {
        throw_exception_on(io_states::buffer_failbit | io_states::io_failbit);
    }
    return r;
}

bool ostream::write_nx(const endline &) noexcept
{
    int r = put("\n", 1);
    flush_nx();
    return (r >= 0);
}

bool ostream::write(const endline &)
{
    bool r = write_nx(endl);
    if (!r) {
        throw_exception_on(io_states::buffer_failbit | io_states::io_failbit);
    }
    return r;
}

bool ostream::write_nx(const flushbuf &) noexcept
{
    return (flush_nx() >= 0);
}

bool ostream::write(const flushbuf &)
{
    return (flush() >= 0);
}

ssize_t ostream::write_buf_nx(const char *msg) noexcept
{
    return put(msg, strlen(msg));
}

ssize_t ostream::write_buf(const char *msg)
{
    ssize_t r = write_buf_nx(msg);
    if (r < 0) {
        throw_exception_on(io_states::buffer_failbit | io_states::io_failbit);
    }
    return r;
}

ssize_t ostream::write_buf_nx(const std::string &msg) noexcept
{
    return put(msg.c_str(), msg.size());
}

ssize_t ostream::write_buf(const std::string &msg)
{
    ssize_t r = write_buf_nx(msg);
    if (r < 0) {
        throw_exception_on(io_states::buffer_failbit | io_states::io_failbit);
    }
    return r;
}

ostream::operator bool() noexcept
{
    return good();
}

ostream::~ostream() noexcept
{
    // We don't use close()/close_nx() functions because we cannot do anything about flush_nx() failure
    // in this stage.
    if (!is_open()) return;
    flush_nx();
    bp_sys::close(get_fd());
}

// Standard output "cout" on STDOUT_FILENO file descriptor
ostream cout(STDOUT_FILENO);
// Standard error output "cerr" on STDERR_FILENO file descriptor
ostream cerr(STDERR_FILENO);

// class istream
int istream::load_into_buf(unsigned len) noexcept
{
    int r = buf->fill(fd, len);
    if (r < 0) {
        io_error = errno;
        return -1;
    }
    return r;
}

void istream::throw_exception_on(const unsigned states)
{
    if ((states & io_states::eofbit) && eof()) {
        throw iostream_eof("End Of File has been reached.");
    }
    if ((states & io_states::buffer_failbit) && buffer_failure()) {
        throw iostream_internal_err("Cannot use stream's buffer: buffer is nullptr!");
    }
    if ((states & io_states::string_failbit) && string_failure()) {
        throw iostream_internal_err("Cannot put lines into given std::string.");
    }
    if ((states & io_states::io_failbit) && io_failure()) {
        throw iostream_system_err(io_failure());
    }
}

bool istream::open_nx(const char *path) noexcept
{
    fd = bp_sys::open(path, O_RDONLY);

    if (fd < 0) {
        io_error = errno;
        return false;
    }
    return true;
}

bool istream::open(const char *path)
{
    bool r = open_nx(path);
    if (io_failure()) {
        throw iostream_system_err(io_failure());
    }
    return r;
}

bool istream::open_nx(const char *path, const int flags) noexcept
{
    fd = bp_sys::open(path, O_RDONLY | flags);

    if (fd < 0) {
        io_error = errno;
        return false;
    }
    return true;
}

bool istream::open(const char *path, const int flags)
{
    bool r = open_nx(path, flags);
    if (io_failure()) {
        throw iostream_system_err(io_failure());
    }
    return r;
}

bool istream::open_nx(const char *path, const int flags, const mode_t mode) noexcept
{
    fd = bp_sys::open(path, O_RDONLY | flags, mode);

    if (fd < 0) {
        io_error = errno;
        return false;
    }
    return true;
}

bool istream::open(const char *path, const int flags, const mode_t mode)
{
    bool r = open_nx(path, flags, mode);
    if (io_failure()) {
        throw iostream_system_err(io_failure());
    }
    return r;
}

bool istream::close_nx() noexcept
{
    if (bp_sys::close(fd) < 0) {
        io_error = errno;
        return false; // Failed
    }

    fd = -1;
    return true;
}

bool istream::close()
{
    bool r = close_nx();
    if (!r) {
        throw_exception_on(io_states::io_failbit);
    }
    return r;
}

unsigned istream::current_state() noexcept
{
    unsigned bits = 0;
    if (eof_state) bits |= io_states::eofbit;
    if (buffer_failed) bits |= io_states::buffer_failbit;
    if (string_failed) bits |= io_states::string_failbit;
    if (io_error) bits |= io_states::io_failbit;
    return bits;
}

bool istream::good() noexcept
{
    return (current_state() == 0);
}

bool istream::eof() noexcept
{
    return eof_state;
}

bool istream::buffer_failure() noexcept
{
    return buffer_failed;
}

bool istream::string_failure() noexcept
{
    return string_failed;
}

int istream::io_failure() noexcept
{
    return io_error;
}

void istream::clear() noexcept
{
    eof_state = false;
    buffer_failed = false;
    string_failed = false;
    io_error = 0;
}

int istream::getc_nx() noexcept
{
    if (eof() || buffer_failure() || io_failure()) {
        // We don't care about string_failure() in getc() function.
        return -1;
    }
    if (buf->get_length() == 0) {
        int r = load_into_buf(1);
        if (r == 0) {
            eof_state = true;
            return -1;
        }
        else if (r < 0) {
            io_error = errno;
            return -1;
        }
    }
    char result = *buf->get_ptr(0);
    buf->consume(1);
    return result;
}

int istream::getc()
{
    int r = getc_nx();
    if (r < 0) {
        throw_exception_on(io_states::eofbit | io_states::buffer_failbit | io_states::io_failbit);
    }
    return r;
}

ssize_t istream::get_line_nx(std::string &dest, char delim) noexcept
{
    if (!good()) {
        return -1;
    }
    if (buf->get_length() == 0) {
        int r = load_into_buf(buf->get_size());
        if (r == 0) {
            eof_state = true;
            return 0;
        }
        if (r < 0) {
            io_error = errno;
            return -1;
        }
    }
    dest.clear();

    char *ptr = buf->get_ptr(0);
    unsigned len = buf->get_contiguous_length(ptr);
    char *eptr = static_cast<char *>(std::memchr(ptr, delim, len));

    if (eptr == ptr) {
        buf->consume(1);
        return 1; // An empty line
    }
    while (eptr == nullptr) {
        // If it hasn't any delim; We try to read as much as possible to reach the EOF or find a
        // "delim" character.
        try {
            // dest.append() may throw out_of_range, length_error or bad_alloc
            dest.append(ptr, len);
        }
        catch (const std::exception &e) {
            string_failed = true;
            return -1;
        }
        buf->consume(len);

        if (buf->get_length() == 0) {
            int r = load_into_buf(buf->get_size());
            if (r == 0) {
                return dest.size();
            }
            if (r < 0) {
                io_error = errno;
                return -1;
            }
        }

        ptr = buf->get_ptr(0);
        len = buf->get_contiguous_length(ptr);
        eptr = static_cast<char *>(std::memchr(ptr, delim, len));
    }
    unsigned index = eptr - ptr;
    try {
        // dest.append() may throw out_of_range, length_error or bad_alloc
        dest.append(ptr, index);
    }
    catch (const std::exception &e) {
        string_failed = true;
        return -1;
    }
    buf->consume(index + 1); // Consume the delimiter character as well
    return dest.size();
}

ssize_t istream::get_line(std::string &dest, char delim)
{
    ssize_t r = get_line_nx(dest, delim);
    if (r <= 0) {
        throw_exception_on(io_states::eofbit | io_states::buffer_failbit | io_states::string_failbit
                | io_states::io_failbit);
    }
    return r;
}

ssize_t istream::get_line_until_eof(std::string &dest, char delim)
{
    ssize_t r = get_line_nx(dest, delim);
    if (r < 0) {
        throw_exception_on(io_states::buffer_failbit | io_states::string_failbit | io_states::io_failbit);
    }
    return r;
}

istream::operator bool() noexcept
{
    return good();
}

istream::~istream() noexcept
{
    if (!is_open()) return;
    close_nx();
}

// Standard input "cin" on STDIN_FILENO file descriptor
istream cin(STDIN_FILENO);

} /* dio namespace */
