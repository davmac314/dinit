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
 * All names are declared in the "dio" namespace.
 */

#ifndef DINIT_IOSTREAM_H_INCLUDED
#define DINIT_IOSTREAM_H_INCLUDED 1

#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <cmath>

#include <cpbuffer.h>

constexpr unsigned IOSTREAM_BUFSIZE = 16384; // Size of the buffers

namespace dio {

// Stream state bits:
//
// 1. 'eofbit' indicates that the End Of File has been reached, and there is nothing left in
//    the buffer (istream only).
// 2. 'buffer_failbit' indicates an attempt to use the buffer when the buffer pointer was nullptr
//    (e.g. failure to allocate a unique_ptr of streambuf) (ostream & istream).
// 3. 'string_failbit' indicates a failed operation in std::string related calls (e.g. failure in
//    pushing a line from the buffer to the given std::string in get_line()) (istream only).
// 4. 'io_failbit' indicates that a system I/O function failed, and the error number (errno) was
//    recorded (istream & ostream).
//
// If all of the above bits are false, the stream status is "good".
//
// The istream/ostream::current_state() function returns the stream's current status based on this
// table.
enum io_states
{
    eofbit = 0x01,
    buffer_failbit = 0x02,
    string_failbit = 0x04,
    io_failbit = 0x08
};

// Some (empty) classes for signalling special buffer operations. These are used to implement
// support for 'endl' and 'flush' in ostream::write(...) functions.
class endline { };
class flushbuf { };

// Passing 'endl' to write(...) functions will write a newline and flush the stream's buffer.
static constexpr endline endl;
// Passing 'flush' to write(...) functions will flush the stream's buffer.
static constexpr flushbuf flush;

// Specialise system_error & runtime_error for more meaningful exceptions on error states:

// Exception thrown on end-of-file (eofbit).
class iostream_eof : public std::runtime_error
{
    public:
    iostream_eof(const char *msg) : runtime_error(msg)
    {
    }
};

// Exception thrown on allocation failure (buffer_failbit, string_failbit).
class iostream_internal_err : public std::runtime_error
{
    public:
    iostream_internal_err(const char *msg) : runtime_error(msg)
    {
    }
};

// Exception thrown on I/O error (io_failbit).
class iostream_system_err : public std::system_error
{
    public:
    iostream_system_err(const int error_code) : system_error(error_code, std::system_category())
    {
    }
};

using streambuf = cpbuffer<IOSTREAM_BUFSIZE>;

// A type representing the result of an attempt to read a character (getc[_nx]() functions).
struct getc_result
{
    bool success;
    char character;
};

// A class to provide basic variables and methods which are shared between ostream and istream.
class io_base
{
    protected:
    int fd = -1;
    std::unique_ptr<streambuf> buf;

    public:
    // Get raw pointer to the current buffer.
    // Note: The buffer may be null if allocation failed (buffer_failbit will be set).
    streambuf *get_buf() noexcept;

    // Is the current stream's file descriptor open?
    bool is_open() noexcept;

    // Set the file descriptor. Setting a new file descriptor will replace any current descriptor
    // in the stream. The buffer must be flushed before replacing the file descriptor. The
    // previous file descriptor remains open and it is the responsibility of the caller to ensure
    // that it is closed to avoid a file descriptor leak.
    void set_fd(const int newfd) noexcept;

    // Get the current file descriptor.
    int get_fd() noexcept;
};

// ostream
//
// This class provides file output functionality. It holds a std::unique_ptr of streambuf (a fixed-size
// circular buffer which that's size can be changed at compile-time) and controls its file descriptor
// and handle it gracefully when needed.
//
// Constructors
// ------------
//
// The ostream class has four constructors:
//
//      // Simple construction
//      dio::ostream output_a;
//      if (output_a.buffer_failure()) return -1; // Failure in buffer allocation
//
//      // Construction with moving a pre-allocated unique_ptr to the buffer
//      try {
//          std::unique_ptr<streambuf> my_buffer(new streambuf);
//      }
//      catch(const std::bad_alloc &) {
//          print_err("Cannot allocate memory for the output buffer!\n");
//          return -1;
//      }
//      dio::ostream output_b(std::move(my_buffer)); // Takes ownership of my_buffer
//
//      // Construction with a file descriptor
//      dio::ostream output_c(3);
//      if (output_c.buffer_failure()) return -1; // Failure in buffer allocation
//
//      // Construction with a combination of a file descriptor and moving a
//      // pre-allocated unique_ptr to the buffer
//      dio::ostream output_d(3, std::move(buffer_2));
//
// Setting (or changing) the file descriptor after construction is available through the set_fd()
// function (See note in top of set_fd() func). opening and closing files is possible through open()
// function.
//
// Copy/move constructors/assignments and destructor
// -------------------------------------------------
//
// ostream objects cannot be moved, copied or assigned.
//
// The ostream destructor will write all of the buffer content into its file descriptor through the
// flush_nx() call and close the its current fd.
//
// Note: Failures in ostream destructor are impossible to catch.
//
// Public member inherited from io_base: is_open()
// ------------------------------------------------------
//
// ostream through base io_base class provides is_open() function which return true when the
// ostream currently has an open file descriptor or false when it's not.
//
// Error handling and Stream states
// --------------------------------
//
// There are two possible error states in an ostream: buffer failure and I/O failure. These are each
// represented by a bit in the value returned by the current_state() function.
//
// 1. buffer_failbit is false by default and means the class constructor failed to allocate a unique_ptr
//    of the buffer at construction time.
// 2. io_failbit is false by default and means there is something wrong with system I/O functions.
//
// Note: if the io_failbit is set in the return value of current_state(), then io_failure() will
// return the errno value corresponding to the failed operation. If it is clear, io_failure() will return 0.
//
// Exception handling
// ------------------
//
// See the description for each function to know what exceptions can be thrown by functions.
//
// All exception-throwing functions have exception-free variants marked by the "_nx" suffix. Those
// functions guarantee not to throw any exceptions.
class ostream : public io_base
{
    int io_error = 0;

    // Internal function to appending into the buffer. Returns written bytes from message or -1 on
    // error (error number can be retrieved via io_failure()).
    ssize_t put(const char *msg, size_t count) noexcept;

    // Throw an exception if current state flags indicate an error condition. 'states' is a
    // bitmask formed by combining values in the io_states enum, indicating which state flags are
    // to be tested.
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

    // Open file by path, optionally with flags and mode as given to open(2). If flags are not
    // specified, the file will be opened write-only. If flags are specified, O_WRONLY is added
    // implicitly.
    // Note: opening an already opened stream is not supported; the stream must be closed before
    // opening it again.
    // Returns:
    //   true on success, false on failure.
    // Throws:
    //   Non _nx variant may throw:
    //     dio::iostream_system_err on I/O error
    bool open_nx(const char *path) noexcept;
    bool open(const char *path);

    bool open_nx(const char *path, const int flags) noexcept;
    bool open(const char *path, const int flags);

    bool open_nx(const char *path, const int flags, const mode_t mode) noexcept;
    bool open(const char *path, const int flags, const mode_t mode);

    // Flush the buffer and close the current file descriptor.
    // Returns:
    //   true on success, false on failure cases.
    // Throws:
    //   Non _nx variant may throw:
    //     dio::iostream_internal_err on buffer failure (buffer could not be allocated at
    //       construction).
    //     dio::iostream_system_err on system I/O failure.
    bool close_nx() noexcept;
    bool close();

    // Check the current state of the stream.
    // Returns:
    //  The current state bits in stream (a combination of values from 'io_states').
    int current_state() noexcept;

    // Check whether the stream is currently "good".
    // Returns:
    //   true when there are no error states on the stream; false otherwise.
    bool good() noexcept;

    // Check whether the allocation failure bit ('buffer_failbit') is set in the stream state.
    // Returns:
    //   The value of 'buffer_failbit' in the the current state.
    bool buffer_failure() noexcept;

    // Check whether an I/O error has been recorded in the stream state ('io_failbit').
    // Returns:
    //   0 if no error recorded; otherwise, the 'errno' value that was recorded.
    int io_failure() noexcept;

    // Resets current state, other than 'buffer_failbit', to clear (error-free).
    void clear() noexcept;

    // Flush the stream's buffer.
    // Returns:
    //   true if the buffer was successfully fully-flushed buffer, false otherwise.
    // Throws:
    //   Non _nx variant may throw:
    //     dio::iostream_internal_err on buffer failure (buffer could not be allocated at
    //       construction).
    //     dio::iostream_system_err on system I/O failure.
    bool flush_nx() noexcept;
    bool flush();

    // write(...) functions: write (text) to the stream. Accepts string and numeric arguments;
    // numbers are written as decimal digits.
    //
    // All writes go through the buffer and not necessarily written to the file descriptor except
    // in these situations:
    // 1. The buffer becomes full.
    // 2. "flush()" (or its exception-free variant: flush_nx()) is called.
    // 3. A "flush" object is passed to write() functions (equivalent to calling flush()).
    // 4. An "endl" object is passed to write() functions (equivalent to writing \n and
    //    calling flush()).
    // 5. The object is destructed (which calls flush_nx()).
    //
    // Multiple arguments can be passed in a single call as well (due to the templated overload), e.g:
    //
    //      output_a.write("This is an example message.\n");
    //      output_b.write("2 + 2 equals: ", 2 + 2, endl);
    //
    // Returns:
    //   true if the data was completely written to the stream (including by being buffered);
    //     false if an error occurred (including if the data could be partly written/buffered).
    // Throws:
    //   Non _nx variants may throw:
    //     dio::iostream_internal_err on buffer failure (buffer could not be allocated at
    //       construction).
    //     dio::iostream_system_err on system I/O failure.
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
        bool r = write_nx(a, b, c...);
        if (!r) {
            // This will definitely throw an exception
            throw_exception_on(io_states::buffer_failbit | io_states::io_failbit);
        }
        return r;
    }

    // write_buf() functions: write bytes to the stream.
    //
    // All writes go through the buffer and not necessarily written to the file descriptor except
    // in the situations described for write(...) above.
    //
    // Returns:
    //   Number of successfully written-or-buffered characters.
    // Throws:
    //   Non _nx variants may throw:
    //     dio::iostream_internal_err on buffer failure (buffer could not be allocated at
    //       construction).
    //     dio::iostream_system_err on system I/O failure.
    ssize_t write_buf_nx(const char *msg) noexcept;
    ssize_t write_buf(const char *msg);

    ssize_t write_buf_nx(const std::string &msg) noexcept;
    ssize_t write_buf(const std::string &msg);

    // Convenience conversion to bool, equivalent to good().
    // Returns:
    //   true when there are no error states on the stream; false otherwise.
    explicit operator bool() noexcept;

    ~ostream() noexcept;
};

// istream
//
// This class provides a buffered input stream for file descriptors. An istream owns a file descriptor
// and an associated buffer.
//
// Constructors
// ------------
//
// The istream class has four constructors:
//
//      // Simple construction
//      dio::istream input_a;
//      if (input_a.buffer_failure()) return -1; // Failure in buffer allocation
//
//      // Construction with moving a pre-allocated unique_ptr to the buffer
//      try {
//          std::unique_ptr<streambuf> my_buffer(new streambuf);
//      }
//      catch(const std::bad_alloc &) {
//          print_err("Cannot allocate memory for input buffer!\n");
//          return -1;
//      }
//      dio::istream input_b(std::move(my_buffer)); // Takes ownership of my_buffer
//
//      // Construction with a file descriptor
//      dio::istream input_c(3);
//      if (input_c.buffer_failure()) return -1; // Failure in buffer allocation
//
//      // Construction with a combination of a file descriptor and moving a
//      // pre-allocated unique_ptr to the buffer
//      dio::istream input_d(3, std::move(buffer_2));
//
// Also, setting (or changing) file descriptor after construction is available through set_fd() function
// (See note in top of set_fd()). Opening and closing files is possible through open() function.
//
// Copy/move constructors/assignments and destructor
// -------------------------------------------------
//
// istream objects cannot be moved, copied or assigned.
//
// The istream destructer will close its fd.
//
// Public member inherited from io_base: is_open()
// ------------------------------------------------------
//
// istream through base io_base class provides is_open() function which return true when the
// istream currently has an open file descriptor or false when it's not.
//
// Error handling and Stream states
// --------------------------------
//
// There are four possible states in an istream: EOF, buffer failure, string related operations failure and
// I/O failure. These are each represented by a bit in the value returned by the current_state() function.
//
// 1. eofbit is false by default and means there is nothing left in the file descriptor and the buffer
//    of the stream.
// 2. buffer_failbit is false by default and means the class constructor failed to allocate a unique_ptr
//    of the buffer at construction time.
// 3. string_failbit is false by default and means a std::string related operation was failed.
// 4. io_failbit is false by default and means there is something wrong from system I/O calls.
//
// Note: if the io_failbit is set in the return value of current_state(), then io_failure() will
// return the errno value corresponding to the failed operation. If it is clear, io_failure() will return 0.
//
// Exception handling
// ------------------
//
// See the description for each function to know what exceptions can be thrown by functions.
//
// All of the exception-throwing functions have exception-free variants which are marked by the "_nx"
// suffix. Those functions guarantee not to throw any exceptions.
//
class istream : public io_base
{
    // Variables to capture stream current status:
    bool eof_state = false;
    bool string_failed = false;
    int io_error = 0;

    // Helper function to fill the buffer by reading from the file descriptor (a wrapper for
    // buf->fill()).
    // Returns:
    //   The number of bytes read into the buffer, or -1 on error (with io_error set).
    int load_into_buf(unsigned len) noexcept;

    // Throw an exception if current state flags indicate an error condition. 'states' is a
    // bitmask formed by combining values in the io_states enum, indicating which state flags are
    // to be tested.
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

    // Open file path, optionally with flags and modes as given to open(2). If flags are not
    // specified, the file will be opened read-only. If flags are specified, O_RDONLY is added
    // implicitly.
    // Note: opening an already opened stream is not supported; the stream must be closed before
    // opening it again.
    // Returns:
    //   true on success, false on failure.
    // Throws:
    //   Non _nx variant may throw:
    //     dio::iostream_system_err on I/O error
    bool open_nx(const char *path) noexcept;
    bool open(const char *path);

    bool open_nx(const char *path, const int flags) noexcept;
    bool open(const char *path, const int flags);

    bool open_nx(const char *path, const int flags, const mode_t mode) noexcept;
    bool open(const char *path, const int flags, const mode_t mode);

    // Flush the buffer and close the current file descriptor.
    // Returns:
    //   true on success, false on failure cases.
    // Throws:
    //   Non _nx variant may throw:
    //     dio::iostream_system_err on system I/O failure.
    bool close_nx() noexcept;
    bool close();

    // Check the current state of the stream.
    // Returns:
    //  The current state bits in stream (a combination of values from 'io_states').
    int current_state() noexcept;

    // Check whether the stream is currently "good".
    // Returns:
    //   true when there are no error states on the stream; false otherwise.
    bool good() noexcept;

    // Check whether end-of-file ('eofbit') is set in the stream state.
    // Returns:
    //   The value of 'eofbit' in the the current state.
    bool eof() noexcept;

    // Check whether the allocation failure bit ('buffer_failbit') is set in the stream state.
    // Returns:
    //   The value of 'buffer_failbit' in the the current state.
    bool buffer_failure() noexcept;

    // Check whether the string store failure bit ('string_failbit') is set in the stream state.
    // Returns:
    //   The value of 'string_failbit' in the the current state.
    bool string_failure() noexcept;

    // Check whether an I/O error has been recorded in the stream state ('io_failbit').
    // Returns:
    //   0 if no error recorded; otherwise, the 'errno' value that was recorded.
    int io_failure() noexcept;

    // Resets current state, other than buffer_failbit, to clear (error-free).
    void clear() noexcept;

    // Reads and returns one character from the stream.
    // Returns:
    //   The character read (if successful) and success status.
    // Throws:
    //   Non _nx variant may throw:
    //     dio::iostream_eof on end-of-file.
    //     dio::iostream_internal_err on buffer failure (buffer could not be allocated at construction).
    //     dio::iostream_internal_err on (earlier) string failure.
    //     dio::iostream_system_err on system I/O failure.
    getc_result getc_nx() noexcept;
    getc_result getc();

    // Reads and stores one line from the stream into the given std::string, until a delimiter
    // character (usually '\n') is found, and store what was read in the given string. The
    // previous contents of the string are destroyed. Reaching end-of-file will flag a failure
    // condition.
    // Returns:
    //   true for success, false otherwise.
    // Throws:
    //   Non _nx variant may throw:
    //     dio::iostream_eof on end-of-file.
    //     dio::iostream_internal_err on buffer failure (buffer could not be allocated at construction).
    //     dio::iostream_internal_err on putting into given std::string failure.
    //     dio::iostream_system_err on system I/O failure.
    bool get_line_nx(std::string &dest, char delim = '\n') noexcept;
    bool get_line(std::string &dest, char delim = '\n');

    // Reads and stores one line from the stream into the given std::string, until a delimiter
    // character (usually '\n') is found or end-of-file is reached, and store what was read in the
    // given string. The previous contents of the string are destroyed.
    // Returns:
    //   true for success, false otherwise.
    // Throws:
    //   dio::iostream_internal_err on buffer failure (buffer could not be allocated at construction).
    //   dio::iostream_internal_err on putting into given std::string failure.
    //   dio::iostream_system_err on system I/O failure.
    bool get_line_until_eof(std::string &dest, char delim = '\n');

    // Convenience conversion to bool, equivalent to good().
    // Returns:
    //   true when there are no error states on the stream; false otherwise.
    explicit operator bool() noexcept;

    ~istream() noexcept;
};

} // dio namespace

#endif // DINIT_IOSTREAM_H_INCLUDED
