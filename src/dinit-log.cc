#include <iostream>
#include <algorithm>

#include <unistd.h>
#include <fcntl.h>

#include "dasync.h"

#include "service.h"
#include "dinit-log.h"
#include "cpbuffer.h"

extern EventLoop_t eventLoop;

static bool log_current_line[2];  // Whether the current line is being logged (for console, main log)
LogLevel log_level[2] = { LogLevel::WARN, LogLevel::WARN };

static ServiceSet *service_set = nullptr;  // Reference to service set

namespace {
class BufferedLogStream : public PosixFdWatcher<NullMutex>
{
    private:

    // Outgoing:
    bool partway = false;     // if we are partway throught output of a log message
    bool discarded = false;   // if we have discarded a message
    bool release = true;      // if we should inhibit output and release console

    // A "special message" is not stored in the circular buffer; instead
    // it is delivered from an external buffer not managed by BufferedLogger.
    bool special = false;      // currently outputting special message?
    char *special_buf; // buffer containing special message
    int msg_index;     // index into special message

    CPBuffer<4096> log_buffer;
    
    public:
    
    // Incoming:
    int current_index = 0;    // current/next incoming message index

    int fd = -1;

    void init(int fd)
    {
        this->fd = fd;
        release = false;
    }
    
    Rearm gotEvent(EventLoop_t *loop, int fd, int flags) noexcept override;

    // Check whether the console can be released.
    void flushForRelease();
    void release_console();
    bool is_release_set() { return release; }
    
    // Commit a log message
    void commit_msg()
    {
        bool was_first = current_index == 0;
        current_index = log_buffer.get_length();
        if (was_first && ! release) {
            setEnabled(&eventLoop, true);
        }
    }
    
    void rollback_msg()
    {
        log_buffer.trim_to(current_index);
    }
    
    int get_free()
    {
        return log_buffer.get_free();
    }
    
    void append(const char *s, size_t len)
    {
        log_buffer.append(s, len);
    }
};
}

// Two log streams:
// (One for main log, one for console)
static BufferedLogStream log_stream[2];

constexpr static int DLOG_MAIN = 0; // main log facility
constexpr static int DLOG_CONS = 1; // console

void BufferedLogStream::release_console()
{
    if (release) {
        int flags = fcntl(1, F_GETFL, 0);
        fcntl(1, F_SETFL, flags & ~O_NONBLOCK);
        service_set->pullConsoleQueue();
    }
}

void BufferedLogStream::flushForRelease()
{
    release = true;
    
    // Try to flush any messages that are currently buffered. (Console is non-blocking
    // so it will fail gracefully).
    if (gotEvent(&eventLoop, fd, out_events) == Rearm::DISARM) {
        // Console has already been released at this point.
        setEnabled(&eventLoop, false);
    }
    // gotEvent didn't want to disarm, so must be partway through a message; will
    // release when it's finished.
}

Rearm BufferedLogStream::gotEvent(EventLoop_t *loop, int fd, int flags) noexcept
{
    auto &log_stream = *this;

    if ((! partway) && log_stream.special) {
        char * start = log_stream.special_buf + log_stream.msg_index;
        char * end = std::find(log_stream.special_buf + log_stream.msg_index, (char *)nullptr, '\n');
        int r = write(fd, start, end - start + 1);
        if (r >= 0) {
            if (start + r > end) {
                // All written: go on to next message in queue
                log_stream.special = false;
                log_stream.partway = false;
                log_stream.msg_index = 0;
                
                if (release) {
                    release_console();
                    return Rearm::DISARM;
                }
            }
            else {
                log_stream.msg_index += r;
                return Rearm::REARM;
            }
        }
        else {
            // spurious readiness, or EAGAIN/EWOULDBLOCK/EINTR
            // There's not much we can do for other errors anyway.
        }
        return Rearm::REARM;
    }
    else {
        // Writing from the regular circular buffer
        
        // TODO issue special message if we have discarded a log message
        
        if (log_stream.current_index == 0) {
            release_console();
            return Rearm::DISARM;
        }
        
        char *ptr = log_stream.log_buffer.get_ptr(0);
        int len = log_stream.log_buffer.get_contiguous_length(ptr);
        char *creptr = ptr + len;  // contiguous region end
        char *eptr = std::find(ptr, creptr, '\n');
        
        bool will_complete = false;  // will complete this message?
        if (eptr != creptr) {
            eptr++;  // include '\n'
            will_complete = true;
        }

        len = eptr - ptr;
        
        int r = write(fd, ptr, len);

        if (r >= 0) {
            bool complete = (r == len) && will_complete;
            log_stream.log_buffer.consume(len);
            log_stream.partway = ! complete;
            if (complete) {
                log_stream.current_index -= len;
                if (log_stream.current_index == 0 || release) {
                    // No more messages buffered / stop logging to console:
                    release_console();
                    return Rearm::DISARM;
                }
            }
        }
    }
    
    // We've written something by the time we get here. We could fall through to below, but
    // let's give other events a chance to be processed by returning now.
    return Rearm::REARM;
}

void init_log(ServiceSet *sset) noexcept
{
    service_set = sset;
    log_stream[DLOG_CONS].registerWith(&eventLoop, STDOUT_FILENO, out_events); // TODO register in disabled state
    enable_console_log(true);
}

// Set up the main log to output to the given file descriptor
void setup_main_log(int fd)
{
    log_stream[DLOG_MAIN].init(STDERR_FILENO);
    log_stream[DLOG_MAIN].registerWith(&eventLoop, STDERR_FILENO, out_events);
}

bool is_log_flushed() noexcept
{
    return log_stream[DLOG_CONS].current_index == 0;
}

// Enable or disable console logging. If disabled, console logging will be disabled on the
// completion of output of the current message (if any), at which point the first service record
// queued in the service set will acquire the console.
void enable_console_log(bool enable) noexcept
{
    bool log_to_console = ! log_stream[DLOG_CONS].is_release_set();
    if (enable && ! log_to_console) {
        // Console is fd 1 - stdout
        // Set non-blocking IO:
        int flags = fcntl(1, F_GETFL, 0);
        fcntl(1, F_SETFL, flags | O_NONBLOCK);
        // Activate watcher:
        log_stream[DLOG_CONS].init(STDOUT_FILENO);
        log_stream[DLOG_CONS].setEnabled(&eventLoop, true);
    }
    else if (! enable && log_to_console) {
        log_stream[DLOG_CONS].flushForRelease();
    }
}


// Variadic method to calculate the sum of string lengths:
static int sum_length(const char *arg) noexcept
{
    return std::strlen(arg);
}

template <typename U, typename ... T> static int sum_length(U first, T ... args) noexcept
{
    return sum_length(first) + sum_length(args...);
}

// Variadic method to append strings to a buffer:
static void append(BufferedLogStream &buf, const char *s)
{
    buf.append(s, std::strlen(s));
}

template <typename U, typename ... T> static void append(BufferedLogStream &buf, U u, T ... t)
{
    append(buf, u);
    append(buf, t...);
}

// Variadic method to log a sequence of strings as a single message:
template <typename ... T> static void push_to_log(T ... args) noexcept
{
    int amount = sum_length(args...);
    for (int i = 0; i < 2; i++) {
        if (! log_current_line[i]) continue;
        if (log_stream[i].get_free() >= amount) {
            append(log_stream[i], args...);
            log_stream[i].commit_msg();
        }
        else {
            // TODO mark a discarded message
        }        
    }
}

// Variadic method to potentially log a sequence of strings as a single message with the given log level:
template <typename ... T> static void do_log(LogLevel lvl, T ... args) noexcept
{
    log_current_line[DLOG_CONS] = lvl >= log_level[DLOG_CONS];
    log_current_line[DLOG_MAIN] = lvl >= log_level[DLOG_MAIN];
    push_to_log(args...);
}

template <typename ... T> static void do_log_cons(T ... args) noexcept
{
    log_current_line[DLOG_CONS] = true;
    log_current_line[DLOG_MAIN] = false;
    push_to_log(args...);
}

template <typename ... T> static void do_log_main(T ... args) noexcept
{
    log_current_line[DLOG_CONS] = false;
    log_current_line[DLOG_MAIN] = true;
    push_to_log(args...);
}

// Log a message. A newline will be appended.
void log(LogLevel lvl, const char *msg) noexcept
{
    do_log(lvl, "dinit: ", msg, "\n");
}

// Log part of a message. A series of calls to do_log_part must be followed by a call to do_log_commit.
template <typename T> static void do_log_part(int idx, T arg) noexcept
{
    if (log_current_line[idx]) {
        int amount = sum_length(arg);
        if (log_stream[idx].get_free() >= amount) {
            append(log_stream[idx], arg);
        }
        else {
            log_stream[idx].rollback_msg();
            log_current_line[idx] = false;
            // TODO mark discarded message
        }
    }
}

// Log part of a message. A series of calls to do_log_part must be followed by a call to do_log_commit.
template <typename T> static void do_log_part(T arg) noexcept
{
    do_log_part(DLOG_CONS, arg);
}

// Commit a message that was issued as a series of parts (via do_log_part).
static void do_log_commit(int idx) noexcept
{
    if (log_current_line[idx]) {
        log_stream[idx].commit_msg();
    }
}

// Log a multi-part message beginning
void logMsgBegin(LogLevel lvl, const char *msg) noexcept
{
    log_current_line[DLOG_CONS] = lvl >= log_level[DLOG_CONS];
    log_current_line[DLOG_MAIN] = lvl >= log_level[DLOG_MAIN];
    for (int i = 0; i < 2; i++) {
        do_log_part(i, "dinit: ");
        do_log_part(i, msg);
    }
}

// Continue a multi-part log message
void logMsgPart(const char *msg) noexcept
{
    do_log_part(DLOG_CONS, msg);
    do_log_part(DLOG_MAIN, msg);
}

// Complete a multi-part log message
void logMsgEnd(const char *msg) noexcept
{
    for (int i = 0; i < 2; i++) {
        do_log_part(i, msg);
        do_log_part(i, "\n");
        do_log_commit(i);
    }
}

void logServiceStarted(const char *service_name) noexcept
{
    do_log_cons("[  OK  ] ", service_name, "\n");
    do_log_main("dinit: service ", service_name, " started.\n");
}

void logServiceFailed(const char *service_name) noexcept
{
    do_log_cons("[FAILED] ", service_name, "\n");
    do_log_main("dinit: service ", service_name, " failed to start.\n");
}

void logServiceStopped(const char *service_name) noexcept
{
    do_log_cons("[STOPPD] ", service_name, "\n");
    do_log_main("dinit: service ", service_name, " stopped.\n");
}
