#ifndef DINIT_LOG_H
#define DINIT_LOG_H

// Logging for Dinit

#include <string>

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR,
    ZERO    // log absolutely nothing
};

extern LogLevel log_level;
extern bool log_to_console;

void log(LogLevel lvl, const char *msg) noexcept;
void logServiceStarted(const char *service_name) noexcept;
void logServiceFailed(const char *service_name) noexcept;
void logServiceStopped(const char *service_name) noexcept;

static inline void log(LogLevel lvl, const std::string &str)
{
    log(lvl, str.c_str());
}

static inline void logServiceStarted(const std::string &str)
{
    logServiceStarted(str.c_str());
}

static inline void logServiceFailed(const std::string &str)
{
    logServiceFailed(str.c_str());
}

static inline void logServiceStopped(const std::string &str)
{
    logServiceStopped(str.c_str());
}

#endif
