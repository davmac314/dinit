#include <iostream>
#include "dinit-log.h"

LogLevel log_level = LogLevel::WARN;
bool log_to_console = true;    // whether we should output log messages to console

// Log a message
void log(LogLevel lvl, const char *msg) noexcept
{
    if (lvl >= log_level) {
        if (log_to_console) {
            std::cout << "dinit: " << msg << std::endl;
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
