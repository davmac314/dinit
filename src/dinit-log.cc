#include <iostream>
#include <algorithm>

#include <ev.h>
#include <unistd.h>
#include <fcntl.h>

#include "service.h"
#include "dinit-log.h"
#include "cpbuffer.h"

LogLevel log_level = LogLevel::WARN;
LogLevel cons_log_level = LogLevel::WARN;
static bool log_to_console = false;   // whether we should output log messages to
                                     // console immediately
static bool log_current_line;  // Whether the current line is being logged

static ServiceSet *service_set = nullptr;  // Reference to service set

// Buffer for current log line:
constexpr static int log_linebuf_len = 120; // TODO needed?
static char * lineBuf = new char[log_linebuf_len];
static int lineBuf_idx = 0;


// Buffer of log lines:
// (One for main log, one for console)
static CPBuffer<4096> log_buffer[2];

// Each line is represented as a string of characters terminated by a
// newline. (If the newline is missing, the line is not complete).

constexpr static int DLOG_MAIN = 0; // main log facility
constexpr static int DLOG_CONS = 1; // console

constexpr static char DLOG_MAIN_FLAG = 1 << DLOG_MAIN;
constexpr static char DLOG_CONS_FLAG = 1 << DLOG_CONS;

static int current_index[2] = { 0, 0 };  // current/next message slot for (main, console)

static bool partway[2] = { false, false }; // part-way through writing log line?
static int msg_index[2] = { 0, 0 }; // index into the current message

static struct ev_io eviocb[2];

static bool discarded[2] = { false, false }; // have discarded a log line?
static bool special[2] = { false, false }; // writing from a special buffer?
static char *special_buf[2] = { nullptr, nullptr }; // special buffer

static void release_console()
{
    ev_io_stop(ev_default_loop(EVFLAG_AUTO), &eviocb[DLOG_CONS]);
    if (! log_to_console) {
        int flags = fcntl(1, F_GETFL, 0);
        fcntl(1, F_SETFL, flags & ~O_NONBLOCK);
        service_set->pullConsoleQueue();
    }
}

static void log_conn_callback(struct ev_loop * loop, ev_io * w, int revents) noexcept
{
    if (special[DLOG_CONS]) {
        char * start = special_buf[DLOG_CONS] + msg_index[DLOG_CONS];
        char * end = std::find(special_buf[DLOG_CONS] + msg_index[DLOG_CONS], (char *)nullptr, '\n');
        int r = write(1, start, end - start + 1);
        if (r >= 0) {
            if (start + r > end) {
                // All written: go on to next message in queue
                special[DLOG_CONS] = false;
                partway[DLOG_CONS] = false;
                msg_index[DLOG_CONS] = 0;
            }
            else {
                msg_index[DLOG_CONS] += r;
                return;
            }
        }
        else {
            // spurious readiness - EAGAIN or EWOULDBLOCK?
            // other error?
            // TODO
        }
        return;
    }
    else {
        // Writing from the regular circular buffer
        
        // TODO issue special message if we have discarded a log message
        
        if (current_index[DLOG_CONS] == 0) {
            release_console();
            return;
        }
        
        char *ptr = log_buffer[DLOG_CONS].get_ptr(0);
        int len = log_buffer[DLOG_CONS].get_contiguous_length(ptr);
        char *creptr = ptr + len;  // contiguous region end
        char *eptr = std::find(ptr, creptr, '\n');
        
        bool will_complete = false;  // will complete this message?
        if (eptr != creptr) {
            eptr++;  // include '\n'
            will_complete = true;
        }

        len = eptr - ptr;
        
        int r = write(1, ptr, len);

        if (r >= 0) {
            // msg_index[DLOG_CONS] += r;
            bool complete = (r == len) && will_complete;
            log_buffer[DLOG_CONS].consume(len);
            partway[DLOG_CONS] = ! complete;
            if (complete) {
                current_index[DLOG_CONS] -= len;
                if (current_index[DLOG_CONS] == 0 || !log_to_console) {
                    // No more messages buffered / stop logging to console:
                    release_console();
                }
            }
        }
        else {
            // TODO
            // EAGAIN / EWOULDBLOCK?
            // error?
            return;
        }
    }
    
    // We've written something by the time we get here. We could fall through to below, but
    // let's give other events a chance to be processed by returning now.
    return;
}

void init_log(ServiceSet *sset) noexcept
{
    service_set = sset;
    enable_console_log(true);
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
        ev_io_init(&eviocb[DLOG_CONS], log_conn_callback, 1, EV_WRITE);
        if (current_index[DLOG_CONS] > 0) {
            ev_io_start(ev_default_loop(EVFLAG_AUTO), &eviocb[DLOG_CONS]);
        }
        log_to_console = true;
    }
    else if (! enable && log_to_console) {
        log_to_console = false;
        if (! partway[DLOG_CONS]) {
            if (current_index[DLOG_CONS] > 0) {
                // Try to flush any messages that are currently buffered. (Console is non-blocking
                // so it will fail gracefully).
                log_conn_callback(ev_default_loop(EVFLAG_AUTO), &eviocb[DLOG_CONS], EV_WRITE);
            }
            else {
                release_console();
            }
        }
        // (if we're partway through logging a message, we release the console when
        // finished).
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
static void append(CPBuffer<4096> &buf, const char *s)
{
    buf.append(s, std::strlen(s));
}

template <typename U, typename ... T> static void append(CPBuffer<4096> &buf, U u, T ... t)
{
    append(buf, u);
    append(buf, t...);
}

// Variadic method to log a sequence of strings as a single message:
template <typename ... T> static void do_log(T ... args) noexcept
{
    int amount = sum_length(args...);
    if (log_buffer[DLOG_CONS].get_free() >= amount) {
        append(log_buffer[DLOG_CONS], args...);
        
        bool was_first = (current_index[DLOG_CONS] == 0);
        current_index[DLOG_CONS] += amount;
        if (was_first && log_to_console) {
            ev_io_start(ev_default_loop(EVFLAG_AUTO), & eviocb[DLOG_CONS]);
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

// Log a multi-part message beginning
void logMsgBegin(LogLevel lvl, const char *msg) noexcept
{
    // TODO use buffer
    log_current_line = lvl >= log_level;
    if (log_current_line) {
        if (log_to_console) {
            std::cout << "dinit: " << msg;
        }
    }
}

// Continue a multi-part log message
void logMsgPart(const char *msg) noexcept
{
    // TODO use buffer
    if (log_current_line) {
        if (log_to_console) {
            std::cout << msg;
        }
    }
}

// Complete a multi-part log message
void logMsgEnd(const char *msg) noexcept
{
    // TODO use buffer
    if (log_current_line) {
        if (log_to_console) {
            std::cout << msg << std::endl;
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
