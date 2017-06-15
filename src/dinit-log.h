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
void logMsgBegin(LogLevel lvl, const char *msg) noexcept;
void logMsgPart(const char *msg) noexcept;
void logMsgEnd(const char *msg) noexcept;
void logServiceStarted(const char *service_name) noexcept;
void logServiceFailed(const char *service_name) noexcept;
void logServiceStopped(const char *service_name) noexcept;

// Convenience methods which perform type conversion of the argument.
// There is some duplication here that could possibly be avoided, but
// it doesn't seem like a big deal.
static inline void log(LogLevel lvl, const std::string &str) noexcept
{
    log(lvl, str.c_str());
}

static inline void logMsgBegin(LogLevel lvl, const std::string &str) noexcept
{
    logMsgBegin(lvl, str.c_str());
}

static inline void logMsgBegin(LogLevel lvl, int a) noexcept
{
    constexpr int bufsz = (CHAR_BIT * sizeof(int) - 1) / 3 + 2;
    char nbuf[bufsz];
    snprintf(nbuf, bufsz, "%d", a);
    logMsgBegin(lvl, nbuf);
}

static inline void logMsgPart(const std::string &str) noexcept
{
    logMsgPart(str.c_str());
}

static inline void logMsgPart(int a) noexcept
{
    constexpr int bufsz = (CHAR_BIT * sizeof(int) - 1) / 3 + 2;
    char nbuf[bufsz];
    snprintf(nbuf, bufsz, "%d", a);
    logMsgPart(nbuf);
}

static inline void logMsgEnd(const std::string &str) noexcept
{
    logMsgEnd(str.c_str());
}

static inline void logMsgEnd(int a) noexcept
{
    constexpr int bufsz = (CHAR_BIT * sizeof(int) - 1) / 3 + 2;
    char nbuf[bufsz];
    snprintf(nbuf, bufsz, "%d", a);
    logMsgEnd(nbuf);
}

static inline void logServiceStarted(const std::string &str) noexcept
{
    logServiceStarted(str.c_str());
}

static inline void logServiceFailed(const std::string &str) noexcept
{
    logServiceFailed(str.c_str());
}

static inline void logServiceStopped(const std::string &str) noexcept
{
    logServiceStopped(str.c_str());
}

// It's not intended that methods in this namespace be called directly:
namespace dinit_log {
    template <typename A> static inline void logParts(A a) noexcept
    {
        logMsgEnd(a);
    }

    template <typename A, typename ...B> static inline void logParts(A a, B... b) noexcept
    {
        logMsgPart(a);
        logParts(b...);
    }
}

// Variadic 'log' method.
template <typename A, typename ...B> static inline void log(LogLevel lvl, A a, B ...b) noexcept
{
    logMsgBegin(lvl, a);
    dinit_log::logParts(b...);
}

#endif
