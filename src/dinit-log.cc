#include <iostream>
#include "dinit-log.h"

LogLevel log_level = LogLevel::WARN;
static bool log_to_console = true;   // whether we should output log messages to
                                     // console immediately
static bool log_current_line;  // Whether the current line is being logged

void enable_console_log(bool enable) noexcept
{
    log_to_console = enable;
}

// Log a message
void log(LogLevel lvl, const char *msg) noexcept
{
    if (lvl >= log_level) {
        if (log_to_console) {
            std::cout << "dinit: " << msg << std::endl;
        }
    }
}

// Log a multi-part message beginning
void logMsgBegin(LogLevel lvl, const char *msg) noexcept
{
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
    if (log_current_line) {
        if (log_to_console) {
            std::cout << msg;
        }
    }
}

// Complete a multi-part log message
void logMsgEnd(const char *msg) noexcept
{
    if (log_current_line) {
        if (log_to_console) {
            std::cout << msg << std::endl;
        }
    }
}

void logServiceStarted(const char *service_name) noexcept
{
    if (log_to_console) {
        std::cout << "[  OK  ] " << service_name << std::endl;
    }
}

void logServiceFailed(const char *service_name) noexcept
{
    if (log_to_console) {
        std::cout << "[FAILED] " << service_name << std::endl;
    }
}

void logServiceStopped(const char *service_name) noexcept
{
    if (log_to_console) {
        std::cout << "[STOPPED] " << service_name << std::endl;
    }
}
