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

#include <string>
#include <cstdio>
#include <climits>

class service_set;

enum class loglevel_t {
    DEBUG,
    INFO,
    WARN,
    ERROR,
    ZERO    // log absolutely nothing
};

constexpr static int DLOG_MAIN = 0; // main log facility
constexpr static int DLOG_CONS = 1; // console

// These are defined in dinit-log.cc:
extern loglevel_t log_level[2];
extern bool console_service_status;  // show service status messages to console?

void enable_console_log(bool do_enable) noexcept;
void init_log(service_set *sset, bool syslog_format);
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
