#ifndef DINIT_IOSTREAM_H_INCLUDED
#define DINIT_IOSTREAM_H_INCLUDED 1

#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <cmath>

#include <cpbuffer.h>

/*
 * dinit-iostream is a specialized library providing input/output wrappers around system I/O interfaces.
 * The purpose is to replace standard library iostreams with something more usable, robust, and
 * lightweight. One of the major issues with stdlib iostreams is the difficulty of obtaining useful
 * error messages in a standardized way. The language spec allows for it, but implementations often
 * map all error conditions to the same unhelpful message. std::ios::failure extends std::system_error
 * but doesn't use errno as the error code. Also, they are heavyweight due to support for different
 * locales (which is unnecessary in dinit).
 *
 * This library provides two classes for output and input: ostream and istream.
 * See documentation for each class at the top of it.
 *
 * All functionality of this library is located in the "dio" namespace.
 *
 * Global members based on ostream:
 * --------------------------------
 *
 * ostream cout         // stdout output object on STDOUT_FILENO file descriptor
 *
 * ostream cerr         // stderr output object on STDERR_FILENO file descriptor
 *
 * print(...)           // Writing to stdout
 *
 * print_err(...)       // Writing to stderr
 *
 * "cout" and "cerr" are global objects to write to standard/error output.
 * They provide two global functions: print() and print_err(). They are guaranteed
 * to be exception-free and not to throw any exceptions anyway.
 *
 * See "ostream" class documentation for low-level functionality details.
 *
 * Usage:
 *
 *      print("Hello Standard output :)\n")      // -> stdout
 *      print("2 * 2 equals to: ", 2 * 2, endl); // -> stdout
 *      print_err("Hello Error output :)\n")     // -> stderr
 *
 *
 * Global members based on istream:
 * --------------------------------
 *
 * istream cin          // stdin input object on STDIN_FILENO file descriptor
 *
 * get_char()           // Read one character from cin
 *
 * get_line(dio::istream &stream, std::string &dest, char delim = '\n')
 *                      // Read file until delim is found and push to std::string
 *
 * get_line_nx(dio::istream &stream, std::string &dest, char delim = '\n')
 *                      // Exception-free variant of get_line()
 *
 * get_line_until_eof(dio::istream &stream, std::string &dest, char delim = '\n')
 *                      // Same as get_line() but doesn't throw exception on EOF
 *
 * "cin" is a global object to interact with standard input, which provides get_char().
 *
 * get_char() is guaranteed to be exception-free and not to throw any exceptions anyway.
 * Will return -1 on failure or EOF.
 *
 * get_line() (and all of its variants) are used to read one line from the passed istream and push
 * it to the passed std::string. Will return 0 on EOF and -1 on failure.
 *
 * Note: get_line() erases all contents of the passed std::string when, there is something available
 * in associated file!
 *
 * get_line() may throw these exceptions:
 * On eofbit (get_line_until_eof() doesn't throw this):
 *      dio::iostream_eof("End Of File has been reached.")
 * On buffer_failbit:
 *      dio::iostream_internal_err("Cannot use stream's buffer: buffer is nullptr!")
 * On string_failbit:
 *      dio::iostream_internal_err("Cannot put lines into given std::string.")
 * On io_failbit:
 *      dio::iostream_system_err(errno)
 *
 * Usage:
 *
 *      // Getting a line from stdin via get_line_nx()
 *      while(get_line_nx(cin, my_string)) {
 *          my_output.write(my_string, "\n");
 *      }
 *
 *      // Getting a character from stdin via get_char()
 *      int a_character = get_char();
 *      if (a_character == -1) return; // Check for EOF or error
 *      my_output.write((char)a_character, "\n");
 */

constexpr unsigned IOSTREAM_BUFSIZE = 16384; // Size of the buffers

namespace dio {

/*
 * Stream state bits:
 *
 * 1. "eofbit" indicates that the End Of File has been reached, and there is nothing left in
 *    the buffer (istream only).
 * 2. "buffer_failbit" indicates an attempt to use the buffer when the buffer pointer was nullptr
 *    (e.g. failure to allocate a unique_ptr of streambuf) (ostream/istream).
 * 3. "string_failbit" indicates a failed operation in std::string related calls (e.g. failure in
 *    pushing a line from the buffer to the given std::string in get_line()) (istream only).
 * 4. "io_failbit" indicates that a system I/O function failed, and the errno was set (ostream/istream).
 *
 * If all of the above bits are false, the stream status is "good".
 *
 * The current_state() function returns the stream's current status based on this table.
 */
enum io_states
{
    eofbit = 0x01,
    buffer_failbit = 0x02,
    string_failbit = 0x04,
    io_failbit = 0x08
};

// Some empty classes for buffer operations implementation.
// Used for accepting endl and flush in ostream::write() functions.
class endline { };
class flushbuf { };
// Passing endl to those functions will put a '\n' and flush the stream's buffer.
static constexpr endline endl;
// Passing flush to those functions will flush the stream's buffer.
static constexpr flushbuf flush;

// Specialise system_error & runtime_error for more meaningful exceptions on state bits/vars:

// on eofbit.
class iostream_eof : public std::runtime_error
{
    public:
    iostream_eof(const char *msg) : runtime_error(msg)
    {
    }
};

// on buffer_failbit, string_failbit.
class iostream_internal_err : public std::runtime_error
{
    public:
    iostream_internal_err(const char *msg) : runtime_error(msg)
    {
    }
};

// on io_failbit.
class iostream_system_err : public std::system_error
{
    public:
    iostream_system_err(const int error_code) : system_error(error_code, std::system_category())
    {
    }
};

using streambuf = cpbuffer<IOSTREAM_BUFSIZE>;

class io_base
{
    protected:
    int fd = -1;
    std::unique_ptr<streambuf> buf;

    public:
    // Get raw pointer of current buffer.
    // Note: The buffer may be null if allocation failed (buffer_failbit will be set in this case).
    streambuf *get_buf() noexcept;

    // Is current stream's file descriptor open?
    bool is_open() noexcept;

    // Set file descriptor.
    // Note: Setting a new fd will replace the current fd in the stream and the buffer must be flushed.
    // first before replacing it. Also The previous file descriptor remains open and may need to be closed.
    // by other means to avoid a file descriptor leak.
    void set_fd(const int newfd) noexcept;

    // Get current file descriptor.
    int get_fd() noexcept;
};

/*
 * ostream
 * =-=-=-=
 *
 * This class provides file output functionality. It holds a std::unique_ptr of streambuf (a fixed-size
 * circular buffer which that's size can be changed at compile-time) and controls its file descriptor
 * and handle it gracefully when needed.
 *
 * Constructors
 * ------------
 *
 * The ostream class has four constructors:
 *
 *      // Simple construction
 *      dio::ostream output_a;
 *      if (output_a.buffer_failure()) return -1; // Failure in buffer allocation
 *
 *      // Construction with moving a pre-allocated unique_ptr of the buffer
 *      try {
 *          std::unique_ptr<streambuf> my_buffer(new streambuf);
 *      }
 *      catch(const std::bad_alloc &) {
 *          print_err("Cannot allocate memory for the output buffer!\n");
 *          return -1;
 *      }
 *      dio::ostream output_b(std::move(my_buffer)); // Takes ownership of my_buffer
 *
 *      // Construction with a file descriptor number
 *      dio::ostream output_c(3);
 *      if (output_c.buffer_failure()) return -1; // Failure in buffer allocation
 *
 *      // Construction with a combination of a file descriptor and moving a
 *      // pre-allocated unique_ptr of the buffer
 *      dio::ostream output_d(3, std::move(buffer_2));
 *
 * Setting (or changing) the file descriptor after construction is available through the set_fd()
 * function (See note in top of set_fd() func). opening and closing files is possible through open()
 * function.
 *
 * Copy/move constructors/assignments and destructor
 * -------------------------------------------------
 *
 * ostream objects cannot be moved, copied or assigned.
 *
 * The ostream destructor will write all of the buffer content into its file descriptor through the
 * flush_nx() call and close the its current fd.
 *
 * Note: Failures in ostream destructor are impossible to catch.
 *
 * Public member inherited from io_base: is_open()
 * ------------------------------------------------------
 *
 * ostream through base io_base class provides is_open() function which return true when current
 * file path is open or false when it's not.
 *
 * Error handling and Stream states
 * --------------------------------
 *
 * There are two possible error states in an ostream: buffer failure and I/O failure. These are each
 * represented by a bit in the value returned by the current_state() function.
 *
 * 1. buffer_failbit is false by default and means the class constructor failed to allocate a unique_ptr
 *    of the buffer at construction time.
 * 2. io_failbit is false by default and means there is something wrong with system I/O functions.
 *
 * Note: if the io_failbit is set in the return value of current_state(), then io_failure() will
 * return the errno value corresponding to the failed operation. If it is clear, io_failure() will return 0.
 *
 * Exception handling
 * ------------------
 *
 * See the description for each function to know what exceptions can be thrown by functions.
 *
 * All exception-throwing functions have exception-free variants marked by the "_nx" suffix. Those
 * functions guarantee not to throw any exceptions.
 */
class ostream : public io_base
{
    // io_error has different functionality, it will be set to errno which returned by system
    // functions (such as read()).
    int io_error = 0;

    // Internal function to appending into the buffer.
    ssize_t put(const char *msg, size_t count) noexcept;

    // Throw exception based on current state from io_states.
    // states parameter is for throwing appropriate exceptions (Not throwing unrelated exceptions).
    void throw_exception_on(const int states);

    public:
    ostream() noexcept
    {
        buf = std::unique_ptr<streambuf>(new(std::nothrow) streambuf);
    }

    ostream(std::unique_ptr<streambuf> passed_buf) noexcept
    {
        buf = std::move(passed_buf);
    }

    ostream(const int tfd) noexcept
    {
        fd = tfd;
        buf = std::unique_ptr<streambuf>(new(std::nothrow) streambuf);
    }

    ostream(const int tfd, std::unique_ptr<streambuf> passed_buf) noexcept
    {
        fd = tfd;
        buf = std::move(passed_buf);
    }

    ostream(const ostream &) = delete;

    void operator=(const ostream &) = delete;

    ostream(const ostream &&) = delete;

    void operator=(const ostream &&) = delete;

    // Open file path which specified in path or as open(2) parameter with flags or modes.
    //
    // Note: O_WRONLY is hard-coded in internal call to POSIX open(2).
    //
    // Note: Opening an already opened stream is not allowed and caller must close the stream
    // before open it again.
    //
    // Returns: true on success, false on failure.
    //
    // Non _nx variant may throw:
    // dio::iostream_system_err on POSIX open(2) failure.
    bool open_nx(const char *path) noexcept;
    bool open(const char *path);

    bool open_nx(const char *path, const int flags) noexcept;
    bool open(const char *path, const int flags);

    bool open_nx(const char *path, const int flags, const mode_t mode) noexcept;
    bool open(const char *path, const int flags, const mode_t mode);

    // Flush the buffer and close the current file descriptor.
    //
    // Returns: true on success, false on failure cases.
    //
    // Non _nx variant may throw:
    // dio::iostream_internal_err on Buffer failure (buffer did not allocated at construction time).
    // dio::iostream_system_err on system I/O failure.
    bool close_nx() noexcept;
    bool close();

    // Returns: current bits/vars in stream based on io_states enum.
    int current_state() noexcept;

    // Returns: true when there is no set bits/vars otherwise false.
    bool good() noexcept;

    // Returns: true when buffer_failbit is set otherwise false.
    bool buffer_failure() noexcept;

    // Returns: 0 or errno based on io_error value.
    int io_failure() noexcept;

    // Resets failure bits/vars to false (or 0 about io_error).
    // Note: buffer_failbit will not be reset.
    void clear() noexcept;

    // Flush buffer of stream.
    //
    // Returns: true on full-flushed buffer, false on failure or not full-flushed buffer.
    //
    // Non _nx variant may throw:
    // dio::iostream_internal_err on Buffer failure (buffer did not allocated at construction time).
    // dio::iostream_system_err on system I/O failure.
    bool flush_nx() noexcept;
    bool flush();

    // write() functions, Used to write many types.
    //
    // All calls to write() will be buffered and not necessarily written to the file descriptor except
    // in these situations:
    // 1. The size of the passed message is larger than the buffer size.
    // 2. The size of the passed message is larger than the current buffer free space.
    // 3. "flush()" (or its exception-free variant: flush_nx()) is called.
    // 4. A "flush" object is passed to write() functions (equivalent to calling flush()).
    // 5. An "endl" object is passed to write() functions (equivalent to writing \n and
    //    calling flush()).
    // 6. The object is destructed (which calls flush_nx()).
    //
    // Multiple arguments can be passed in a single call as well (due to the templated overload), e.g:
    //
    //      output_a.write("This is an example message.\n");
    //      output_b.write("2 + 2 equals: ", 2 + 2, endl);
    //
    // Returns: true on success, false on failure cases.
    //
    // Note: write() does return false on partial write, Use write_buf() and check its return, if you
    // want to handle partial write gracefully.
    //
    // Non _nx variants may throw:
    // dio::iostream_internal_err on Buffer failure (buffer did not allocated at construction time).
    // dio::iostream_system_err on system I/O failure.
    bool write_nx(const char *msg) noexcept;
    bool write(const char *msg);

    bool write_nx(char msg) noexcept;
    bool write(char msg);

    bool write_nx(const std::string &msg) noexcept;
    bool write(const std::string &msg);

    bool write_nx(const int num) noexcept;
    bool write(const int num);

    bool write_nx(const unsigned num) noexcept;
    bool write(const unsigned num);

    bool write_nx(const long num) noexcept;
    bool write(const long num);

    bool write_nx(const unsigned long num) noexcept;
    bool write(const unsigned long num);

    bool write_nx(const endline &) noexcept;
    bool write(const endline &);

    bool write_nx(const flushbuf &) noexcept;
    bool write(const flushbuf &);

    template <typename A, typename B, typename ...C> inline bool write_nx(const A &a, const B &b, const C & ...c)
            noexcept
    {
        if (!good()) return false;
        if (!write_nx(a)) return false;
        if (!write_nx(b, c...)) return false;
        return true;
    }

    template <typename A, typename B, typename ...C> inline bool write(const A &a, const B &b, const C & ...c)
    {
        if (!good()) return false;
        if (!write(a)) return false;
        if (!write(b, c...)) return false;
        return true;
    }

    // write_buf() functions, Used to write char* or std::string.
    //
    // All calls to write_buf() will be buffered and not necessarily written to the file descriptor
    // except in situations that are mentioned about write() above.
    //
    // Note: On partial write, io_error will reflect the cause of partial write.
    //
    // Returns: number of written-or-buffered characters.
    //
    // Non _nx variants may throw:
    // dio::iostream_internal_err on Buffer failure (buffer did not allocated at construction time).
    // dio::iostream_system_err on system I/O failure.
    ssize_t write_buf_nx(const char *msg) noexcept;
    ssize_t write_buf(const char *msg);

    ssize_t write_buf_nx(const std::string &msg) noexcept;
    ssize_t write_buf(const std::string &msg);

    // This is an alias for good() function and could be used like this:
    //
    //      if (output_a) {
    //          return; // Everything looks good
    //      }
    //
    // Returns: true when everything looks good otherwise false.
    explicit operator bool() noexcept;

    ~ostream() noexcept;
};

// Standard output "cout" on STDOUT_FILENO file descriptor.
extern ostream cout;
// Standard error output "cerr" on STDERR_FILENO file descriptor.
extern ostream cerr;

template <typename... arguments> ssize_t print(const arguments & ...args) noexcept
{
    return cout.write_nx(args...);
}

template <typename... arguments> ssize_t print_err(const arguments & ...args) noexcept
{
    return cerr.write_nx(args...);
}

/*
 * istream
 * =-=-=-=
 *
 * This class provides file input functionality. It holds a std::unique_ptr streambuf (a fixed-size
 * circular buffer which that's size can be changed at compile-time) and controls its file descriptor
 * and handle it gracefully when needed.
 *
 * Constructors
 * ------------
 *
 * The istream class has four constructors:
 *
 *      // Simple construction
 *      dio::istream input_a;
 *      if (input_a.buffer_failure()) return -1; // Failure in buffer allocation
 *
 *      // Construction with moving a pre-allocated unique_ptr of the buffer
 *      try {
 *          std::unique_ptr<streambuf> my_buffer(new streambuf);
 *      }
 *      catch(const std::bad_alloc &) {
 *          print_err("Cannot allocate memory for input buffer!\n");
 *          return -1;
 *      }
 *      dio::istream input_b(std::move(my_buffer)); // Takes ownership of my_buffer
 *
 *      // Construction with a file descriptor number
 *      dio::istream input_c(3);
 *      if (input_c.buffer_failure()) return -1; // Failure in buffer allocation
 *
 *      // Construction with a combination of a file descriptor and moving a
 *      // pre-allocated unique_ptr of the buffer
 *      dio::istream input_d(3, std::move(buffer_2));
 *
 * Also, setting (or changing) file descriptor after construction is available through set_fd() function
 * (See note in top of set_fd()). opening and closing files is possible through open() function.
 *
 * Copy/move constructors/assignments and destructor
 * -------------------------------------------------
 *
 * istream objects cannot be moved, copied or assigned.
 *
 * The istream destructer will close its fd.
 *
 * Public member inherited from io_base: is_open()
 * ------------------------------------------------------
 *
 * istream through base io_base class provides is_open() function which return true when current
 * file path is open or false when it's not.
 *
 * Error handling and Stream states
 * --------------------------------
 *
 * There are four possible states in an istream: EOF, buffer failure, string related operations failure and
 * I/O failure. These are each represented by a bit in the value returned by the current_state() function.
 *
 * 1. eofbit is false by default and means there is nothing left in the file descriptor and the buffer
 *    of the stream.
 * 2. buffer_failbit is false by default and means the class constructor failed to allocate a unique_ptr
 *    of the buffer at construction time.
 * 3. string_failbit is false by default and means a std::string related operation was failed.
 * 4. io_failbit is false by default and means there is something wrong from system I/O calls.
 *
 * Note: if the io_failbit is set in the return value of current_state(), then io_failure() will
 * return the errno value corresponding to the failed operation. If it is clear, io_failure() will return 0.
 *
 * Exception handling
 * ------------------
 *
 * See the description for each function to know what exceptions can be thrown by functions.
 *
 * All of the exception-throwing functions have exception-free variants which are marked by the "_nx"
 * suffix. Those functions guarantee not to throw any exceptions.
 */
class istream : public io_base
{
    // Variables to capture stream current status:
    bool eof_state = false;
    bool string_failed = false;
    // io_error has different functionality, it will be set to errno which returned by system
    // functions (such as read()).
    int io_error = 0;

    // Internal function to load from file descriptor (a wrapper for buf->fill()).
    int load_into_buf(unsigned len) noexcept;

    // Throw exception based on current state from io_states.
    // states parameter is for throwing appropriate exceptions (Not throwing unrelated exceptions).
    void throw_exception_on(const int states);

    public:
    istream() noexcept
    {
        buf = std::unique_ptr<streambuf>(new(std::nothrow) streambuf);
    }

    istream(std::unique_ptr<streambuf> passed_buf) noexcept
    {
        buf = std::move(passed_buf);
    }

    istream(const int tfd) noexcept
    {
        fd = tfd;
        buf = std::unique_ptr<streambuf>(new(std::nothrow) streambuf);
    }

    istream(const int tfd, std::unique_ptr<streambuf> passed_buf) noexcept
    {
        fd = tfd;
        buf = std::move(passed_buf);
    }

    istream(const istream &) = delete;

    void operator=(const istream &) = delete;

    istream(const istream &&) = delete;

    void operator=(const istream &&) = delete;

    // Open file path which specified in path or as open(2) parameter with flags or modes.
    //
    // Note: O_RDONLY is hard-coded in internal call to POSIX open(2).
    //
    // Note: Opening an already opened stream is not allowed and caller must close the stream
    // before open it again.
    //
    // Returns: true on success, false on failure.
    //
    // Non _nx variant may throw:
    // dio::iostream_system_err on POSIX open(2) failure.
    bool open_nx(const char *path) noexcept;
    bool open(const char *path);

    bool open_nx(const char *path, const int flags) noexcept;
    bool open(const char *path, const int flags);

    bool open_nx(const char *path, const int flags, const mode_t mode) noexcept;
    bool open(const char *path, const int flags, const mode_t mode);

    // Close the current file descriptor.
    //
    // Returns: true on success, false on failure cases.
    //
    // Non _nx variant may throw:
    // dio::iostream_system_err on system I/O failure.
    bool close_nx() noexcept;
    bool close();

    // Returns: current bits in stream based on io_states enum.
    int current_state() noexcept;

    // Returns: true when there is no set bits/vars otherwise false.
    bool good() noexcept;

    // Returns: true or false based on eofbit is set or not.
    bool eof() noexcept;

    // Returns: true or false based on buffer_failbit is set or not.
    bool buffer_failure() noexcept;

    // Returns: true or false based on string_failbit is set or not.
    bool string_failure() noexcept;

    // Returns: 0 or errno based on io_error value.
    int io_failure() noexcept;

    // Resets failure bits/vars to false (or 0 about io_error).
    // Note: buffer_failbit will not be reset.
    void clear() noexcept;

    // Reads and returns one character from file descriptor.
    //
    // Returns: character as an integer or -1 on eofbit, buffer_failure, and io_failure.
    //
    // Non _nx variant may throw:
    // dio::iostream_eof on EOF.
    // dio::iostream_internal_err on Buffer failure (buffer did not allocated at construction time).
    // dio::iostream_system_err on system I/O failure.
    int getc_nx() noexcept;
    int getc();

    // Reads and stores one line from fd into given std::string.
    //
    // istream provides the get_line() function to read from its file descriptor until the delimiter
    // character is found (usually '\n') and write it to the given std::string. Also, the given
    // std::string will be erased when there is something in the buffer on each call.
    //
    // Returns: the amount of characters in the string or 0 on EOF or -1 on failure cases.
    //
    // Non _nx variant may throw:
    // dio::iostream_eof on EOF.
    // dio::iostream_internal_err on Buffer failure (buffer did not allocated at construction time).
    // dio::iostream_internal_err on putting into given std::string failure.
    // dio::iostream_system_err on system I/O failure.
    ssize_t get_line_nx(std::string &dest, char delim = '\n') noexcept;
    ssize_t get_line(std::string &dest, char delim = '\n');

    // Same as get_line() but doesn't throw exception on EOF.
    //
    // Returns: Same as get_line().
    //
    // May throw:
    // dio::iostream_internal_err on Buffer failure (buffer did not allocated at construction time).
    // dio::iostream_internal_err on putting into given std::string failure.
    // dio::iostream_system_err on system I/O failure.
    ssize_t get_line_until_eof(std::string &dest, char delim = '\n');

    // This is an alias for good() function and could be used like this:
    //
    //      if (input_b) {
    //          return; // Everything looks good
    //      }
    //
    // Returns: true when everything looks good otherwise false.
    explicit operator bool() noexcept;

    ~istream() noexcept;
};

// Standard input "cin" on STDIN_FILENO file descriptor.
extern istream cin;

// Reads and returns one character from file descriptor 1 (stdin).
inline int get_char() noexcept
{
    return cin.getc_nx();
}

// Reads and stores one line from given istream into given std::string.
// Non _nx variants May throw:
// dio::iostream_eof on EOF.
// dio::iostream_internal_err on Buffer failure (buffer did not allocated at construction time).
// dio::iostream_internal_err on pushing into given std::string failure.
// dio::iostream_system_err on system I/O failure.
inline ssize_t get_line_nx(istream &stream, std::string &dest, char delim = '\n') noexcept
{
    return stream.get_line_nx(dest, delim);
}

inline ssize_t get_line(istream &stream, std::string &dest, char delim = '\n')
{
    return stream.get_line(dest, delim);
}

// Same as get_line() but doesn't throw exception on EOF.
// May throw:
// dio::iostream_internal_err on Buffer failure (buffer did not allocated at construction time).
// dio::iostream_internal_err on putting into given std::string failure.
// dio::iostream_system_err on system I/O failure.
inline ssize_t get_line_until_eof(istream &stream, std::string &dest, char delim = '\n')
{
    return stream.get_line_until_eof(dest, delim);
}

} /* dio namespace */

#endif /* DINIT_IOSTREAM_H_INCLUDED */
