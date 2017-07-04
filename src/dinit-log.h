#ifndef DINIT_LOG_H
#define DINIT_LOG_H

// Logging for Dinit

#include <string>
#include <cstdio>
#include <climits>

class service_set;

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR,
    ZERO    // log absolutely nothing
};

extern LogLevel log_level[2];
void enable_console_log(bool do_enable) noexcept;
void init_log(service_set *sset);
void setup_main_log(int fd);
bool is_log_flushed() noexcept;
void discard_console_log_buffer() noexcept;

void log(LogLevel lvl, const char *msg) noexcept;
void log_msg_begin(LogLevel lvl, const char *msg) noexcept;
void log_msg_part(const char *msg) noexcept;
void log_msg_end(const char *msg) noexcept;
void log_service_started(const char *service_name) noexcept;
void log_service_failed(const char *service_name) noexcept;
void log_service_stopped(const char *service_name) noexcept;

// Convenience methods which perform type conversion of the argument.
// There is some duplication here that could possibly be avoided, but
// it doesn't seem like a big deal.
static inline void log(LogLevel lvl, const std::string &str) noexcept
{
    log(lvl, str.c_str());
}

static inline void log_msg_begin(LogLevel lvl, const std::string &str) noexcept
{
    log_msg_begin(lvl, str.c_str());
}

static inline void log_msg_begin(LogLevel lvl, int a) noexcept
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
    template <typename A> static inline void log_parts(A a) noexcept
    {
        log_msg_end(a);
    }

    template <typename A, typename ...B> static inline void log_parts(A a, B... b) noexcept
    {
        log_msg_part(a);
        log_parts(b...);
    }
}

// Variadic 'log' method.
template <typename A, typename ...B> static inline void log(LogLevel lvl, A a, B ...b) noexcept
{
    log_msg_begin(lvl, a);
    dinit_log::log_parts(b...);
}

#endif
