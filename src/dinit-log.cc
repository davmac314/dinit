#include <algorithm>

#include <unistd.h>
#include <fcntl.h>
#include <sys/syslog.h>
#include <sys/uio.h>

#include "dasynq.h"

#include "service.h"
#include "dinit-log.h"
#include "cpbuffer.h"

// Dinit logging subsystem.
//
// Note that most actual functions for logging messages are found in the header, dinit-log.h.
// See documentation there also.

extern eventloop_t event_loop;
extern bool external_log_open;

static bool log_current_line[DLOG_NUM];  // Whether the current line is being logged (for console, main log)
static bool log_format_syslog[DLOG_NUM] = { true, false };
static_assert(DLOG_NUM == 2, "number of log streams has changed");
static_assert(DLOG_MAIN == 0, "main log index has changed");

static service_set *services = nullptr;  // Reference to service set

loglevel_t log_level[DLOG_NUM] = { loglevel_t::NOTICE, loglevel_t::WARN };
static_assert(DLOG_NUM == 2, "number of log streams has changed");

bool console_service_status = true;  // show service status messages to console?


dasynq::time_val release_time; // time the log was released

using rearm = dasynq::rearm;

namespace {
class buffered_log_stream : public eventloop_t::fd_watcher_impl<buffered_log_stream>
{
    private:

    // Outgoing:
    bool partway = false;     // if we are partway throught output of a log message
    bool discarded = false;   // if we have discarded a message
    bool release = true;      // if we should inhibit output and release console when possible

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
    void flush_for_release();
    bool is_release_set() { return release; }
    
    // Commit a log message
    void commit_msg()
    {
        bool was_first = current_index == 0;
        current_index = log_buffer.get_length();
        if (was_first && ! release) {
            set_enabled(event_loop, true);
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

    void watch_removed() noexcept override;

    private:
    void release_console();
};

// Two log streams:
// (One for main log, one for console)
buffered_log_stream log_stream[DLOG_NUM];

void buffered_log_stream::release_console()
{
    if (release) {
        int flags = fcntl(1, F_GETFL, 0);
        fcntl(1, F_SETFL, flags & ~O_NONBLOCK);
        if (services != nullptr) {
            services->pull_console_queue();
        }
        if (release) {
            // release still set, we didn't immediately get the console back; record the
            // time at which we released:
            event_loop.get_time(release_time, clock_type::MONOTONIC);
        }
    }
}

void buffered_log_stream::flush_for_release()
{
    if (release) return;

    release = true;
    
    // Try to flush any messages that are currently buffered. (Console is non-blocking
    // so it will fail gracefully).
    rearm rearm_val = fd_event(event_loop, fd, dasynq::OUT_EVENTS);
    if (rearm_val == rearm::DISARM) {
        // Console has already been released at this point.
        set_enabled(event_loop, false);
    }
    if (rearm_val == rearm::REMOVE) {
        deregister(event_loop);
    }
    // fd_event didn't want to disarm, so must be partway through a message; will
    // release when it's finished.
}

rearm buffered_log_stream::fd_event(eventloop_t &loop, int fd, int flags) noexcept
{
    if ((! partway) && (! special) && discarded) {
        special_buf = "dinit: *** log message discarded due to full buffer ***\n";
        special = true;
        discarded = false;
        msg_index = 0;
    }

    if ((! partway) && special) {
        const char * start = special_buf + msg_index;
        const char * end = start;
        while (*end != '\n') end++;
        int r = bp_sys::write(fd, start, end - start + 1);
        if (r >= 0) {
            if (start + r > end) {
                // All written: go on to next message in queue
                special = false;
                discarded = false;
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
        unsigned len = log_buffer.get_contiguous_length(ptr);
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
        if (!will_complete && len != log_buffer.get_length()) {
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
        
        ssize_t r = bp_sys::writev(fd, logiov, iovs_to_write);

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

void buffered_log_stream::watch_removed() noexcept
{
    if (fd > STDERR_FILENO) {
        bp_sys::close(fd);
    }
    fd = -1;
    release = true; // prevent attempt to enable watch

    // Here we rely on there only being two logs, console and "main"; we can check if we are the
    // main log via identity:
    if (&log_stream[DLOG_MAIN] == this) {
        external_log_open = false;
    }
}

} // end namespace

// Initialise the logging subsystem
// Potentially throws std::bad_alloc or std::system_error
void init_log(bool syslog_format)
{
    log_stream[DLOG_CONS].add_watch(event_loop, STDOUT_FILENO, dasynq::OUT_EVENTS, false);
    enable_console_log(true);

    // The main (non-console) log won't be active yet, but we set the format here so that we
    // buffer messages in the correct format:
    log_format_syslog[DLOG_MAIN] = syslog_format;
}

void setup_log_console_handoff(service_set *sset)
{
    services = sset;
}

// Close logging subsystem
void close_log()
{
    if (log_stream[DLOG_CONS].fd != -1) log_stream[DLOG_CONS].deregister(event_loop);
    if (log_stream[DLOG_MAIN].fd != -1) log_stream[DLOG_MAIN].deregister(event_loop);
}

// Set up the main log to output to the given file descriptor.
// Potentially throws std::bad_alloc or std::system_error
void setup_main_log(int fd)
{
    log_stream[DLOG_MAIN].init(fd);
    log_stream[DLOG_MAIN].add_watch(event_loop, fd, dasynq::OUT_EVENTS);
}

bool is_log_flushed() noexcept
{
    return log_stream[DLOG_CONS].current_index == 0 &&
            (log_stream[DLOG_MAIN].fd == -1 || log_stream[DLOG_MAIN].current_index == 0);
}

// Enable or disable console logging. If disabled, console logging will be disabled on the
// completion of output of the current message (if any), at which point the first service record
// queued in the service set will acquire the console.
void enable_console_log(bool enable) noexcept
{
    bool log_to_console = ! log_stream[DLOG_CONS].is_release_set();
    if (enable && ! log_to_console) {
        // Set non-blocking IO:
        int flags = fcntl(STDOUT_FILENO, F_GETFL, 0);
        fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK);
        // Activate watcher:
        log_stream[DLOG_CONS].init(STDOUT_FILENO);
        log_stream[DLOG_CONS].set_enabled(event_loop, true);
    }
    else if (! enable && log_to_console) {
        log_stream[DLOG_CONS].flush_for_release();
    }
}

void discard_console_log_buffer() noexcept
{
    // Only discard if more than a second has passed since we released the console.
    dasynq::time_val current_time;
    event_loop.get_time(current_time, clock_type::MONOTONIC);
    if (current_time - release_time >= dasynq::time_val(1, 0)) {
        log_stream[DLOG_CONS].discard();
    }
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
static void append(buffered_log_stream &buf, const char *s)
{
    buf.append(s, std::strlen(s));
}

template <typename ... T> static void append(buffered_log_stream &buf, const char *u, T ... t)
{
    append(buf, u);
    append(buf, t...);
}

static int log_level_to_syslog_level(loglevel_t l)
{
    switch (l) {
    case loglevel_t::DEBUG:
        return LOG_DEBUG;
    case loglevel_t::NOTICE:
        return LOG_NOTICE;
    case loglevel_t::WARN:
        return LOG_WARNING;
    case loglevel_t::ERROR:
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
template <typename ... T> static void do_log(loglevel_t lvl, bool to_cons, T ... args) noexcept
{
    log_current_line[DLOG_CONS] = (lvl >= log_level[DLOG_CONS]) && to_cons;
    log_current_line[DLOG_MAIN] = (lvl >= log_level[DLOG_MAIN]);
    push_to_log(DLOG_CONS, args...);
    
    if (log_current_line[DLOG_MAIN]) {
        if (log_format_syslog[DLOG_MAIN]) {
            char svcbuf[10];
            snprintf(svcbuf, 10, "<%d>", LOG_DAEMON | log_level_to_syslog_level(lvl));
            push_to_log(DLOG_MAIN, svcbuf, args...);
        }
        else {
            push_to_log(DLOG_MAIN, args...);
        }
    }
}

template <typename ... T> static void do_log_cons(T ... args) noexcept
{
    if (console_service_status) {
        log_current_line[DLOG_CONS] = true;
        log_current_line[DLOG_MAIN] = false;
        push_to_log(DLOG_CONS, args...);
    }
}

// Log to the main facility at NOTICE level
template <typename ... T> static void do_log_main(T ... args) noexcept
{
    log_current_line[DLOG_CONS] = false;
    log_current_line[DLOG_MAIN] = true;
    
    if (log_format_syslog[DLOG_MAIN]) {
        char svcbuf[10];
        snprintf(svcbuf, 10, "<%d>", LOG_DAEMON | LOG_NOTICE);
        push_to_log(DLOG_MAIN, svcbuf, args...);
    }
    else {
        push_to_log(DLOG_MAIN, args...);
    }
}

// Log a message. A newline will be appended.
void log(loglevel_t lvl, const char *msg) noexcept
{
    do_log(lvl, true, "dinit: ", msg, "\n");
}

void log(loglevel_t lvl, bool to_cons, const char *msg) noexcept
{
    do_log(lvl, to_cons, "dinit: ", msg, "\n");
}

// Log part of a message. A series of calls to do_log_part must be followed by a call to do_log_commit.
static void do_log_part(int idx, const char *arg) noexcept
{
    if (log_current_line[idx]) {
        int amount = sum_length(arg);
        if (log_stream[idx].get_free() >= amount) {
            append(log_stream[idx], arg);
        }
        else {
            // we have to discard the message
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
void log_msg_begin(loglevel_t lvl, const char *msg) noexcept
{
    log_current_line[DLOG_CONS] = lvl >= log_level[DLOG_CONS];
    log_current_line[DLOG_MAIN] = lvl >= log_level[DLOG_MAIN];

    // Prepend the syslog priority level string ("<N>") for the main log:
    if (log_current_line[DLOG_MAIN]) {
        if (log_format_syslog[DLOG_MAIN]) {
            char svcbuf[10];
            snprintf(svcbuf, 10, "<%d>", LOG_DAEMON | log_level_to_syslog_level(lvl));
            do_log_part(DLOG_MAIN, svcbuf);
        }
    }

    for (int i = 0; i < DLOG_NUM; i++) {
        do_log_part(i, "dinit: ");
        do_log_part(i, msg);
    }
}

// Continue a multi-part log message
void log_msg_part(const char *msg) noexcept
{
    do_log_part(DLOG_CONS, msg);
    do_log_part(DLOG_MAIN, msg);
}

// Complete a multi-part log message
void log_msg_end(const char *msg) noexcept
{
    for (int i = 0; i < DLOG_NUM; i++) {
        do_log_part(i, msg);
        do_log_part(i, "\n");
        do_log_commit(i);
    }
}

void log_service_started(const char *service_name) noexcept
{
    do_log_cons("[  OK  ] ", service_name, "\n");
    do_log_main("dinit: service ", service_name, " started.\n");
}

void log_service_failed(const char *service_name) noexcept
{
    do_log_cons("[FAILED] ", service_name, "\n");
    do_log_main("dinit: service ", service_name, " failed to start.\n");
}

void log_service_stopped(const char *service_name) noexcept
{
    do_log_cons("[STOPPD] ", service_name, "\n");
    do_log_main("dinit: service ", service_name, " stopped.\n");
}
