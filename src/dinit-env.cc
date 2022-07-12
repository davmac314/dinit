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
void read_env_file(const char *env_file_path, bool log_warnings, environment &env)
{
    std::ifstream env_file(env_file_path);
    if (! env_file) return;

    env_file.exceptions(std::ios::badbit);

    auto &clocale = std::locale::classic();
    std::string line;
    int linenum = 0;

    while (std::getline(env_file, line)) {
        linenum++;
        auto lpos = line.begin();
        auto lend = line.end();
        while (lpos != lend && std::isspace(*lpos, clocale)) {
            ++lpos;
        }

        if (lpos == lend) continue; // empty line

        if (*lpos == '#') {
            continue;
        }
        if (*lpos == '=') {
            if (log_warnings) {
                log_bad_env(linenum);
            }
            continue;
        }

        // "!COMMAND" form.
        if (*lpos == '!') {
            ++lpos; // lpos = first char of command
            auto epos = lpos;
            do {
                ++epos;
            } while(epos != lend && !std::isspace(*epos, clocale));

            const char *lpos_p = line.data() + (lpos - line.begin());
            string_view cmd {lpos_p, (size_t)(epos - lpos)};

            if (cmd == "clear") {
                env.clear_no_inherit();
            }
            else if (log_warnings) {
                log_bad_env_cmd(linenum);
            }

            continue;
        }

        // ENV=VALUE form.
        auto name_begin = lpos++;
        // skip until '=' or whitespace:
        while (lpos != lend && *lpos != '=' && !std::isspace(*lpos, clocale)) ++lpos;
        auto name_end = lpos;
        //  skip whitespace:
        while (lpos != lend && std::isspace(*lpos, clocale)) ++lpos;
        if (lpos == lend || *lpos != '=') {
            if (log_warnings) {
                log_bad_env(linenum);
            }
            continue;
        }

        ++lpos;
        auto val_begin = lpos;
        while (lpos != lend && *lpos != '\n') ++lpos;
        auto val_end = lpos;

        if (val_begin != (name_end + 1) || val_begin != line.begin() || name_end != lend) {
            // there are spaces that we need to eliminate
            std::string name_and_val;
            name_and_val.reserve((name_end - name_begin) + 1 + (val_end - val_begin));
            name_and_val = line.substr(name_begin - line.begin(), name_end - name_begin);
            name_and_val.append(1, '=');
            name_and_val.append(val_begin, val_end);
            env.set_var(std::move(name_and_val));
        }
        else {
            line.shrink_to_fit();
            env.set_var(std::move(line));
        }
    }
}
