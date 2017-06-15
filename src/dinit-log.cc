#include <iostream>
#include <algorithm>

#include <unistd.h>
#include <fcntl.h>
#include <sys/syslog.h>
#include <sys/uio.h>

#include "dasynq.h"

#include "service.h"
#include "dinit-log.h"
#include "cpbuffer.h"

extern eventloop_t eventLoop;

static bool log_current_line[2];  // Whether the current line is being logged (for console, main log)
LogLevel log_level[2] = { LogLevel::WARN, LogLevel::WARN };

static service_set *services = nullptr;  // Reference to service set

namespace {
class BufferedLogStream : public eventloop_t::fd_watcher_impl<BufferedLogStream>
{
    private:

    // Outgoing:
    bool partway = false;     // if we are partway throught output of a log message
    bool discarded = false;   // if we have discarded a message
    bool release = true;      // if we should inhibit output and release console

    // A "special message" is not stored in the circular buffer; instead
    // it is delivered from an external buffer not managed by BufferedLogger.
    bool special = false;      // currently outputting special message?
    const char *special_buf; // buffer containing special message
    int msg_index;     // index into special message

    cpbuffer<4096> log_buffer;
    
    public:
    
    // Incoming:
    int current_index = 0;    // current/next incoming message index

    int fd = -1;

    void init(int fd)
    {
        this->fd = fd;
        release = false;
    }
    
    rearm fd_event(eventloop_t &loop, int fd, int flags) noexcept;

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
            set_enabled(eventLoop, true);
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
    
    // Discard buffer; call only when the stream isn't active.
    void discard()
    {
        current_index = 0;
        log_buffer.trim_to(0);
    }

    // Mark that a message was discarded due to full buffer
    void mark_discarded()
    {
        discarded = true;
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
        services->pull_console_queue();
    }
}

void BufferedLogStream::flushForRelease()
{
    release = true;
    
    // Try to flush any messages that are currently buffered. (Console is non-blocking
    // so it will fail gracefully).
    if (fd_event(eventLoop, fd, OUT_EVENTS) == rearm::DISARM) {
        // Console has already been released at this point.
        set_enabled(eventLoop, false);
    }
    // fd_event didn't want to disarm, so must be partway through a message; will
    // release when it's finished.
}

rearm BufferedLogStream::fd_event(eventloop_t &loop, int fd, int flags) noexcept
{
    if ((! partway) && (! special) && discarded) {
        special_buf = "dinit: *** message discarded due to full buffer ****\n";
        msg_index = 0;
    }

    if ((! partway) && special) {
        const char * start = special_buf + msg_index;
        const char * end = std::find(special_buf + msg_index, (const char *)nullptr, '\n');
        int r = write(fd, start, end - start + 1);
        if (r >= 0) {
            if (start + r > end) {
                // All written: go on to next message in queue
                special = false;
                partway = false;
                msg_index = 0;
                
                if (release) {
                    release_console();
                    return rearm::DISARM;
                }
            }
            else {
                msg_index += r;
                return rearm::REARM;
            }
        }
        else if (errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK) {
            return rearm::REMOVE;
        }
        return rearm::REARM;
    }
    else {
        // Writing from the regular circular buffer
        
        if (current_index == 0) {
            release_console();
            return rearm::DISARM;
        }
        
        // We try to find a complete line (terminated by '\n') in the buffer, and write it
        // out. Since it may span the circular buffer end, it may consist of two distinct spans,
        // and so we use writev to write them atomically.
        
        struct iovec logiov[2];
        
        char *ptr = log_buffer.get_ptr(0);
        int len = log_buffer.get_contiguous_length(ptr);
        char *creptr = ptr + len;  // contiguous region end
        char *eptr = std::find(ptr, creptr, '\n');
        
        bool will_complete = false;  // will complete this message?
        if (eptr != creptr) {
            eptr++;  // include '\n'
            will_complete = true;
        }

        len = eptr - ptr;
        
        logiov[0].iov_base = ptr;
        logiov[0].iov_len = len;
        int iovs_to_write = 1;
        
        // Do we need the second span?
        if (! will_complete && len != log_buffer.get_length()) {
            ptr = log_buffer.get_buf_base();
            creptr = ptr + log_buffer.get_length() - len;
            eptr = std::find(ptr, creptr, '\n');
            if (eptr != creptr) {
                eptr++; // include '\n'
                // It should not ever be the case that we do not now have a complete message
                will_complete = true;
            }
            logiov[1].iov_base = ptr;
            logiov[1].iov_len = eptr - ptr;
            len += logiov[1].iov_len;
            iovs_to_write = 2;
        }
        
        ssize_t r = writev(fd, logiov, iovs_to_write);

        if (r >= 0) {
            bool complete = (r == len) && will_complete;
            log_buffer.consume(len);
            partway = ! complete;
            if (complete) {
                current_index -= len;
                if (current_index == 0 || release) {
                    // No more messages buffered / stop logging to console:
                    release_console();
                    return rearm::DISARM;
                }
            }
        }
        else if (errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK) {
            return rearm::REMOVE;
        }
    }
    
    // We've written something by the time we get here. We could fall through to below, but
    // let's give other events a chance to be processed by returning now.
    return rearm::REARM;
}

// Initialise the logging subsystem
// Potentially throws std::bad_alloc or std::system_error
void init_log(service_set *sset)
{
    services = sset;
    log_stream[DLOG_CONS].add_watch(eventLoop, STDOUT_FILENO, OUT_EVENTS, false);
    enable_console_log(true);
}

// Set up the main log to output to the given file descriptor.
// Potentially throws std::bad_alloc or std::system_error
void setup_main_log(int fd)
{
    log_stream[DLOG_MAIN].init(fd);
    log_stream[DLOG_MAIN].add_watch(eventLoop, fd, OUT_EVENTS);
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
        log_stream[DLOG_CONS].set_enabled(eventLoop, true);
    }
    else if (! enable && log_to_console) {
        log_stream[DLOG_CONS].flushForRelease();
    }
}

void discard_console_log_buffer() noexcept
{
    log_stream[DLOG_CONS].discard();
}

// Variadic method to calculate the sum of string lengths:
static int sum_length(const char *arg) noexcept
{
    return std::strlen(arg);
}

template <typename ... T> static int sum_length(const char * first, T ... args) noexcept
{
    return sum_length(first) + sum_length(args...);
}

// Variadic method to append strings to a buffer:
static void append(BufferedLogStream &buf, const char *s)
{
    buf.append(s, std::strlen(s));
}

template <typename ... T> static void append(BufferedLogStream &buf, const char *u, T ... t)
{
    append(buf, u);
    append(buf, t...);
}

static int log_level_to_syslog_level(LogLevel l)
{
    switch (l) {
    case LogLevel::DEBUG:
        return LOG_DEBUG;
    case LogLevel::INFO:
        return LOG_INFO;
    case LogLevel::WARN:
        return LOG_WARNING;
    case LogLevel::ERROR:
        return LOG_ERR;
    default: ;
    }
    
    return LOG_CRIT;
}

// Variadic method to log a sequence of strings as a single message to a particular facility:
template <typename ... T> static void push_to_log(int idx, T ... args) noexcept
{
    if (! log_current_line[idx]) return;
    int amount = sum_length(args...);
    if (log_stream[idx].get_free() >= amount) {
        append(log_stream[idx], args...);
        log_stream[idx].commit_msg();
    }
    else {
        log_stream[idx].mark_discarded();
    }
}

// Variadic method to potentially log a sequence of strings as a single message with the given log level:
template <typename ... T> static void do_log(LogLevel lvl, T ... args) noexcept
{
    log_current_line[DLOG_CONS] = lvl >= log_level[DLOG_CONS];
    log_current_line[DLOG_MAIN] = lvl >= log_level[DLOG_MAIN];
    push_to_log(DLOG_CONS, args...);
    
    if (log_current_line[DLOG_MAIN]) {
        char svcbuf[10];
        snprintf(svcbuf, 10, "<%d>", LOG_DAEMON | log_level_to_syslog_level(lvl));
        
        push_to_log(DLOG_MAIN, svcbuf, args...);
    }
}

template <typename ... T> static void do_log_cons(T ... args) noexcept
{
    log_current_line[DLOG_CONS] = true;
    log_current_line[DLOG_MAIN] = false;
    push_to_log(DLOG_CONS, args...);
}

// Log to the main facility at NOTICE level
template <typename ... T> static void do_log_main(T ... args) noexcept
{
    log_current_line[DLOG_CONS] = false;
    log_current_line[DLOG_MAIN] = true;
    
    char svcbuf[10];
    snprintf(svcbuf, 10, "<%d>", LOG_DAEMON | LOG_NOTICE);
    
    push_to_log(DLOG_MAIN, svcbuf, args...);
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
            log_stream[idx].mark_discarded();
        }
    }
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

    if (log_current_line[DLOG_MAIN]) {
        char svcbuf[10];
        snprintf(svcbuf, 10, "<%d>", LOG_DAEMON | log_level_to_syslog_level(lvl));
        do_log_part(DLOG_MAIN, svcbuf);
    }

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
