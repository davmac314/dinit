#include <iostream>
#include <fstream>

#include "dinit-log.h"
#include "dinit-env.h"

environment main_env;

// Log a parse error when reading the environment file.
static void log_bad_env(int linenum)
{
    log(loglevel_t::ERROR, "Invalid environment variable setting in environment file (line ", linenum, ")");
}

static void log_bad_env_cmd(int linenum)
{
    log(loglevel_t::ERROR, "Unknown command in environment file (line ", linenum, ")");
}

// Read and set environment variables from a file. May throw std::bad_alloc, std::system_error.
void read_env_file(const char *env_file_path, bool log_warnings, environment &env, bool throw_on_open_failure)
{
    read_env_file_inline(env_file_path, log_warnings, env, throw_on_open_failure, log_bad_env, log_bad_env_cmd);
}
