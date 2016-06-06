#include <iostream>
#include <algorithm>

#include <unistd.h>
#include <fcntl.h>

#include "dasync.h"

#include "service.h"
#include "dinit-log.h"
#include "cpbuffer.h"

extern EventLoop_t eventLoop;

LogLevel log_level = LogLevel::WARN;
LogLevel cons_log_level = LogLevel::WARN;

static bool log_to_console = false;   // whether we should output log messages to
                                     // console immediately
static bool log_current_line;  // Whether the current line is being logged

static ServiceSet *service_set = nullptr;  // Reference to service set

namespace {
class BufferedLogStream : public PosixFdWatcher<NullMutex>
{
    private:

    // Outgoing:
    bool partway = false;     // if we are partway throught output of a log message
    bool discarded = false;   // if we have discarded a message

    // A "special message" is not stored in the circular buffer; instead
    // it is delivered from an external buffer not managed by BufferedLogger.
    bool special = false;      // currently outputting special message?
    char *special_buf; // buffer containing special message
    int msg_index;     // index into special message

    CPBuffer<4096> log_buffer;
    
    public:
    
    // Incoming:
    int current_index = 0;    // current/next incoming message index

    int fd;

    void init(int fd)
    {
        this->fd = fd;
    }
    
    Rearm gotEvent(EventLoop_t *loop, int fd, int flags) noexcept override;

    // Check whether the console can be released.
    void flushForRelease();
    
    // Commit a log message
    void commit_msg()
    {
        bool was_first = current_index == 0;
        current_index = log_buffer.get_length();
        if (was_first && log_to_console) {
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


static void release_console()
{
    if (! log_to_console) {
        int flags = fcntl(1, F_GETFL, 0);
        fcntl(1, F_SETFL, flags & ~O_NONBLOCK);
        service_set->pullConsoleQueue();
    }
}

void BufferedLogStream::flushForRelease()
{
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
    // TODO correct for the case that this is *not* the console log stream.
    
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
                
                if (!log_to_console) {
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
            // spurious readiness - EAGAIN or EWOULDBLOCK?
            // other error?
            // TODO
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
                if (log_stream.current_index == 0 || !log_to_console) {
                    // No more messages buffered / stop logging to console:
                    release_console();
                    return Rearm::DISARM;
                }
            }
        }
        else {
            // TODO
            // EAGAIN / EWOULDBLOCK?
            // error?
            return Rearm::REARM;
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

bool is_log_flushed() noexcept
{
    return log_stream[DLOG_CONS].current_index == 0;
}

// Enable or disable console logging. If disabled, console logging will be disabled on the
// completion of output of the current message (if any), at which point the first service record
// queued in the service set will acquire the console.
void enable_console_log(bool enable) noexcept
{
    if (enable && ! log_to_console) {
        // Console is fd 1 - stdout
        // Set non-blocking IO:
        int flags = fcntl(1, F_GETFL, 0);
        fcntl(1, F_SETFL, flags | O_NONBLOCK);
        // Activate watcher:
        log_stream[DLOG_CONS].init(STDOUT_FILENO);
        log_stream[DLOG_CONS].setEnabled(&eventLoop, true);
        log_to_console = true;
    }
    else if (! enable && log_to_console) {
        log_to_console = false;
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
template <typename ... T> static void do_log(T ... args) noexcept
{
    int amount = sum_length(args...);
    if (log_stream[DLOG_CONS].get_free() >= amount) {
        append(log_stream[DLOG_CONS], args...);
        
        bool was_first = (log_stream[DLOG_CONS].current_index == 0);
        log_stream[DLOG_CONS].current_index += amount;
        if (was_first && log_to_console) {
            log_stream[DLOG_CONS].setEnabled(&eventLoop, true);
        }
    }
    else {
        // TODO mark a discarded message
    }
}

// Variadic method to potentially log a sequence of strings as a single message with the given log level:
template <typename ... T> static void do_log(LogLevel lvl, T ... args) noexcept
{
    if (lvl >= cons_log_level) {
        do_log(args...);
    }
}

// Log a message. A newline will be appended.
void log(LogLevel lvl, const char *msg) noexcept
{
    do_log(lvl, "dinit: ", msg, "\n");
}

// Log part of a message. A series of calls to do_log_part must be followed by a call to do_log_commit.
template <typename T> static void do_log_part(T arg) noexcept
{
    int amount = sum_length(arg);
    if (log_stream[DLOG_CONS].get_free() >= amount) {
        append(log_stream[DLOG_CONS], arg);
    }
    else {
        log_stream[DLOG_CONS].rollback_msg();
        log_current_line = false;
        // TODO mark discarded message
    }
}

// Commit a message that was issued as a series of parts (via do_log_part).
static void do_log_commit() noexcept
{
    if (log_current_line) {
        log_stream[DLOG_CONS].commit_msg();
    }
}

// Log a multi-part message beginning
void logMsgBegin(LogLevel lvl, const char *msg) noexcept
{
    // TODO use buffer
    log_current_line = lvl >= log_level;
    if (log_current_line) {
        if (log_to_console) {
            do_log_part("dinit: ");
            do_log_part(msg);
        }
    }
}

// Continue a multi-part log message
void logMsgPart(const char *msg) noexcept
{
    // TODO use buffer
    if (log_current_line) {
        if (log_to_console) {
            do_log_part(msg);
        }
    }
}

// Complete a multi-part log message
void logMsgEnd(const char *msg) noexcept
{
    // TODO use buffer
    if (log_current_line) {
        if (log_to_console) {
            do_log_part(msg);
            do_log_part("\n");
            do_log_commit();
        }
    }
}

void logServiceStarted(const char *service_name) noexcept
{
    do_log("[  OK  ] ", service_name, "\n");
}

void logServiceFailed(const char *service_name) noexcept
{
    do_log("[FAILED] ", service_name, "\n");
}

void logServiceStopped(const char *service_name) noexcept
{
    do_log("[STOPPD] ", service_name, "\n");
}
