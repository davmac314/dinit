#ifndef DINIT_LOG_H
#define DINIT_LOG_H

// Logging for Dinit.
//
// The main log function is the variadic template 'log' function:
//
//     void log(loglevel_t, ...)
//
// It takes a list of items comprising a single log message, including strings (C/C++ style), and integers.
// The loglevel argument determines if the message will actually be logged (according to the configured log
// level of the log mechanisms).
//
// We have two separate log "streams": one for the console/stdout, one for the syslog facility (or log
// file). Both have a circular buffer. Log messages are appended to the circular buffer (for a syslog
// stream, the messages are prepended with a syslog priority indicator). Both streams start out inactive
// (release = true in buffered_log_stream), which means they will buffer messages but not write them.
//
// Service start/stop messages for the console stream are formatted differently, with a "visual" flavour.
// The console stream is treated as informational and in some circumstances messages will be discarded
// from its buffer with no warning.
//
// If a stream buffer becomes full mid-message, the message is discarded and we mark the stream as "message
// discarded". Once the message at the front of the buffer has been fully output we check for the mark and
// if set, issue a message informing that log messages have been discarded, before resuming regular output
// from the buffer. (Because the buffer is full, we can't store the "message discarded" message in it; we
// temporarily switch to a "special buffer" which just contains the "message discarded" text. Current the
// special buffer mechanism is used only for this purpose).
//
// The console log stream needs to be able to release the console, if a service is waiting to acquire it.
// This is accomplished by calling flush_for_release() which then completes the output of the current
// message (if any) and then assigns the console to a waiting service. Once the console is no longer used
// by any service, the service_set will return it to the log; the time between release and re-acquire is
// checked and, if it's too large, the entire buffer is discarded; this avoids suddenly and confusingly
// displaying stale messages.

#include <string>
#include <cstdio>
#include <climits>

class service_set;

enum class loglevel_t {
    DEBUG,
    NOTICE,
    WARN,
    ERROR,
    ZERO    // log absolutely nothing
};

constexpr static int DLOG_MAIN = 0; // main log facility
constexpr static int DLOG_CONS = 1; // console

constexpr static int DLOG_NUM = 2;

// These are defined in dinit-log.cc:
extern loglevel_t log_level[2];
extern bool console_service_status;  // show service status messages to console?

void enable_console_log(bool do_enable) noexcept;
void init_log(bool syslog_format);
void setup_log_console_handoff(service_set *sset);
void close_log();
void setup_main_log(int fd);
bool is_log_flushed() noexcept;
void discard_console_log_buffer() noexcept;

// Log a simple string:
void log(loglevel_t lvl, const char *msg) noexcept;
// Log a simple string, optionally without logging to console:
void log(loglevel_t lvl, bool to_cons, const char *msg) noexcept;

// Log a message in parts; a beginnning, various middle parts, and an end part. Calls to these functions
// must not be interleaved with calls to other logging functions.
void log_msg_begin(loglevel_t lvl, const char *msg) noexcept;
void log_msg_part(const char *msg) noexcept;
void log_msg_end(const char *msg) noexcept;

// Defined below:
void log_service_started(const char *service_name) noexcept;
void log_service_failed(const char *service_name) noexcept;
void log_service_stopped(const char *service_name) noexcept;

// Convenience methods which perform type conversion of the argument.
// There is some duplication here that could possibly be avoided, but
// it doesn't seem like a big deal.
static inline void log(loglevel_t lvl, const std::string &str) noexcept
{
    log(lvl, str.c_str());
}

static inline void log_msg_begin(loglevel_t lvl, const std::string &str) noexcept
{
    log_msg_begin(lvl, str.c_str());
}

static inline void log_msg_begin(loglevel_t lvl, int a) noexcept
{
    constexpr int bufsz = (CHAR_BIT * sizeof(int) - 1) / 3 + 2;
    char nbuf[bufsz];
    snprintf(nbuf, bufsz, "%d", a);
    log_msg_begin(lvl, nbuf);
}

static inline void log_msg_part(const std::string &str) noexcept
{
    log_msg_part(str.c_str());
}

static inline void log_msg_part(int a) noexcept
{
    constexpr int bufsz = (CHAR_BIT * sizeof(int) - 1) / 3 + 2;
    char nbuf[bufsz];
    snprintf(nbuf, bufsz, "%d", a);
    log_msg_part(nbuf);
}

static inline void log_msg_end(const std::string &str) noexcept
{
    log_msg_end(str.c_str());
}

static inline void log_msg_end(int a) noexcept
{
    constexpr int bufsz = (CHAR_BIT * sizeof(int) - 1) / 3 + 2;
    char nbuf[bufsz];
    snprintf(nbuf, bufsz, "%d", a);
    log_msg_end(nbuf);
}

static inline void log_service_started(const std::string &str) noexcept
{
    log_service_started(str.c_str());
}

static inline void log_service_failed(const std::string &str) noexcept
{
    log_service_failed(str.c_str());
}

static inline void log_service_stopped(const std::string &str) noexcept
{
    log_service_stopped(str.c_str());
}

// It's not intended that methods in this namespace be called directly:
namespace dinit_log {
    template <typename A> static inline void log_parts(const A &a) noexcept
    {
        log_msg_end(a);
    }

    template <typename A, typename ...B> static inline void log_parts(const A &a, const B & ...b) noexcept
    {
        log_msg_part(a);
        log_parts(b...);
    }
}

// Variadic 'log' method.
template <typename A, typename ...B> static inline void log(loglevel_t lvl, const A &a, const B & ...b) noexcept
{
    log_msg_begin(lvl, a);
    dinit_log::log_parts(b...);
}

#endif
