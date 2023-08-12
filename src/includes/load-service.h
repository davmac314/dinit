#include <iostream>
#include <list>
#include <limits>
#include <utility>
#include <vector>
#include <iterator>

#include <csignal>
#include <cstring>
#include <cstdlib>

#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <grp.h>
#include <pwd.h>

#include "dinit-env.h"
#include "dinit-utmp.h"
#include "dinit-util.h"
#include "service-constants.h"
#include "mconfig.h"

struct service_flags_t
{
    // on-start flags:
    bool rw_ready : 1;  // file system should be writable once this service starts
    bool log_ready : 1; // syslog should be available once this service starts

    // Other service options flags:
    bool runs_on_console : 1;   // run "in the foreground"
    bool starts_on_console : 1; // starts in the foreground
    bool shares_console : 1;    // run on console, but not exclusively
    bool unmask_intr : 1;       // (if runs/starts on console) unmask SIGINTR
    bool pass_cs_fd : 1;        // pass this service a control socket connection via fd
    bool start_interruptible : 1; // the startup of this service process is ok to interrupt with SIGINT
    bool skippable : 1;         // if interrupted the service is skipped (scripted services)
    bool signal_process_only : 1;  // signal the session process, not the whole group
    bool always_chain : 1;      // always start chain-to service on exit
    bool kill_all_on_stop : 1;  // kill all other processes before stopping this service

    service_flags_t() noexcept : rw_ready(false), log_ready(false),
            runs_on_console(false), starts_on_console(false), shares_console(false), unmask_intr(false),
            pass_cs_fd(false), start_interruptible(false), skippable(false), signal_process_only(false),
            always_chain(false), kill_all_on_stop(false)
    {
    }
};

// Resource limits for a particular service & particular resource
struct service_rlimits
{
    int resource_id; // RLIMIT_xxx identifying resource
    bool soft_set : 1;
    bool hard_set : 1;
    struct rlimit limits;

    service_rlimits(int id) : resource_id(id), soft_set(0), hard_set(0), limits({0,0}) { }
};

// Exception while loading a service
class service_load_exc
{
    public:
    std::string service_name;
    std::string exc_description;

    service_load_exc(const std::string &serviceName, std::string &&desc)
        : service_name(serviceName), exc_description(std::move(desc))
    {
    }

    protected:
    service_load_exc(std::string &&desc) : exc_description(std::move(desc))
    {
    }
};

class service_not_found : public service_load_exc
{
    public:
    service_not_found(const std::string &serviceName)
        : service_load_exc(serviceName, "service description not found.")
    {
    }
};

class service_load_error : public service_load_exc
{
    public:
    service_load_error(const std::string &serviceName, std::string &&path, int fail_errno)
        : service_load_exc(serviceName, path + ": " + strerror(fail_errno))
    {
    }
};

class service_cyclic_dependency : public service_load_exc
{
    public:
    service_cyclic_dependency(const std::string &serviceName)
        : service_load_exc(serviceName, "has cyclic dependency.")
    {
    }
};

class service_description_exc : public service_load_exc
{
    public:
    const unsigned line_num = -1;
    const char * const setting_name = nullptr;

    service_description_exc(unsigned line_num, std::string &&exc_info)
            : service_load_exc(std::move(exc_info)), line_num(line_num)
    {
    }

    service_description_exc(const char *setting_name, std::string &&exc_info)
            : service_load_exc(std::move(exc_info)), setting_name(setting_name)
    {
    }

    service_description_exc(const std::string &serviceName, std::string &&extraInfo, unsigned line_num)
        : service_load_exc(serviceName, std::move(extraInfo)), line_num(line_num)
    {
    }

    service_description_exc(const std::string &serviceName, std::string &&extraInfo, const char *setting_name)
        : service_load_exc(serviceName, std::move(extraInfo)), setting_name(setting_name)
    {
    }
};

namespace dinit_load {

using string = std::string;
using string_iterator = std::string::iterator;

// Utility function to skip white space. Returns an iterator at the
// first non-white-space position (or at end).
inline string_iterator skipws(string_iterator i, string_iterator end) noexcept
{
    using std::locale;
    using std::isspace;

    while (i != end) {
      if (! isspace(*i, locale::classic())) {
        break;
      }
      ++i;
    }
    return i;
}

// Signal name and number (the std::pair constructor isn't constexpr in C++11, so we use a custom type).
struct signal_name_number_pair {
    const char *first; // signal name
    int second; // number
};

// Supported signal names and the corresponding signal (number).
constexpr signal_name_number_pair signal_to_int_map[] = {
    { "none", 0 },
    { "NONE", 0 },
    { "HUP", SIGHUP },
    { "INT", SIGINT },
    { "QUIT", SIGQUIT },
    { "KILL", SIGKILL },
    { "USR1", SIGUSR1 },
    { "USR2", SIGUSR2 },
    { "TERM", SIGTERM },
    { "CONT", SIGCONT },
    { "STOP", SIGSTOP },
#ifdef SIGINFO
    { "INFO", SIGINFO },
#endif
};

// Convert a signal name (without "SIG" prefix) to the corresponding signal number. May only
// support a subset of signals. Returns 0 if signal name is "none"/"NONE", returns -1 if signal
// is otherwise unrecognised.
inline int signal_name_to_number(const std::string &signame) noexcept
{
    int sig = -1;
    for (const auto &signal: signal_to_int_map) {
        if (signal.first == signame) {
            sig = signal.second;
            break;
        }
    }
    return sig;
}

// Read a setting/variable name; return empty string if no valid name
//
// If env is set, dashes/dots are not allowed within names. They are not typically allowed by shells
// and they interfere with substitution patterns.
inline string read_config_name(string_iterator & i, string_iterator end, bool env = false) noexcept
{
    using std::locale;
    using std::ctype;
    using std::use_facet;

    // To avoid the horror of locales, we'll use the classic facet only, to identify digits, control
    // characters and punctuation. (Unless something is totally crazy, we are talking about ASCII or
    // a superset of it, but using the facet allows us to avoid that assumption). However, we're only
    // working with "narrow" char type so accuracy is limited. In general, that's not going to matter
    // much, but may allow certain unicode punctuation characters to be used as part of a name for example.
    const ctype<char> & facet = use_facet<ctype<char>>(locale::classic());

    string rval;

    // Don't allow empty name, numeric digit, or dash/dot at start of setting name
    if (i == end || (*i == '-' || *i == '.' || facet.is(ctype<char>::digit, *i))) {
        return {};
    }

    // Within the setting name, allow dash and dot unless parsing envvar name
    // also allow any non-control, non-punctuation non-space character.
    while (i != end && (((*i == '-' || *i == '.') && !env) || *i == '_'
            || (!facet.is(ctype<char>::cntrl, *i) && !facet.is(ctype<char>::punct, *i)
                    && !facet.is(ctype<char>::space, *i)))) {
        rval += *i;
        ++i;
    }
    return rval;
}

// Read a setting value.
//
// In general a setting value is a single-line string. It may contain multiple parts
// separated by white space (which is normally collapsed). A hash mark - # - denotes
// the end of the value and the beginning of a comment (it should be preceded by
// whitespace).
//
// Part of a value may be quoted using double quote marks, which prevents collapse
// of whitespace and interpretation of most special characters (the quote marks will
// not be considered part of the value). A backslash can precede a character (such
// as '#' or '"' or another backslash) to remove its special meaning. Newline
// characters are not allowed in values and cannot be quoted.
//
// This function expects the string to be in an ASCII-compatible encoding (the "classic" locale).
//
// Throws service_description_exc (with service name unset) on error.
//
// Params:
//    service_name - the name of the service to which the setting applies
//    i  -  reference to string iterator through the line
//    end -   iterator at end of line (not including newline character if any)
//    part_positions -  list of <int,int> to which the position of each setting value
//                      part will be added as [start,end). May be null.
inline string read_setting_value(unsigned line_num, string_iterator & i, string_iterator end,
        std::list<std::pair<unsigned,unsigned>> * part_positions = nullptr)
{
    using std::locale;
    using std::isspace;

    i = skipws(i, end);

    string rval;
    bool new_part = true;
    int part_start;

    while (i != end) {
        char c = *i;
        if (c == '\"') {
            if (new_part) {
                part_start = rval.length();
                new_part = false;
            }
            // quoted string
            ++i;
            while (i != end) {
                c = *i;
                if (c == '\"') break;
                else if (c == '\\') {
                    // A backslash escapes the following character.
                    ++i;
                    if (i != end) {
                        c = *i;
                        rval += c;
                    }
                    else {
                        throw service_description_exc(line_num, "line end follows backslash escape character (`\\')");
                    }
                }
                else {
                    rval += c;
                }
                ++i;
            }
            if (i == end) {
                // String wasn't terminated
                throw service_description_exc(line_num, "unterminated quoted string");
            }
        }
        else if (c == '\\') {
            if (new_part) {
                part_start = rval.length();
                new_part = false;
            }
            // A backslash escapes the next character
            ++i;
            if (i != end) {
                rval += *i;
            }
            else {
                throw service_description_exc(line_num, "backslash escape (`\\') not followed by character");
            }
        }
        else if (isspace(c, locale::classic())) {
            if (! new_part && part_positions != nullptr) {
                part_positions->emplace_back(part_start, rval.length());
                new_part = true;
            }
            i = skipws(i, end);
            if (i == end) break;
            if (*i == '#') break; // comment
            rval += ' ';  // collapse ws to a single space
            continue;
        }
        else if (c == '#') {
            // Possibly intended a comment; we require leading whitespace to reduce occurrence of accidental
            // comments in setting values.
            throw service_description_exc(line_num, "hashmark (`#') comment must be separated from setting value by whitespace");
        }
        else {
            if (new_part) {
                part_start = rval.length();
                new_part = false;
            }
            rval += c;
        }
        ++i;
    }

    // Got to end:
    if (part_positions != nullptr) {
        part_positions->emplace_back(part_start, rval.length());
    }

    return rval;
}

inline void fill_environment_userinfo(uid_t uid, const std::string &service_name, environment &env)
{
    if (uid == (uid_t)-1) {
        uid = geteuid();
    }

    char buf[std::numeric_limits<unsigned long long>::digits10 + 1];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)uid);

    errno = 0;
    struct passwd *pwent = getpwuid(uid);

    if (!pwent) {
        if (!errno) {
            throw service_load_exc(service_name, std::string("user id '") + buf + "' does not exist in system database");
        } else {
            throw service_load_exc(service_name, std::string("error accessing user database: ") + strerror(errno));
        }
    }

    std::string enval;

    // USER
    enval = "USER=";
    enval += pwent->pw_name;
    env.set_var(std::move(enval));
    // LOGNAME
    enval = "LOGNAME=";
    enval += pwent->pw_name;
    env.set_var(std::move(enval));
    // HOME
    enval = "HOME=";
    enval += pwent->pw_dir;
    env.set_var(std::move(enval));
    // SHELL
    enval = "SHELL=";
    enval += pwent->pw_shell;
    env.set_var(std::move(enval));
    // UID (non-standard, but useful)
    enval = "UID=";
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)pwent->pw_uid);
    enval += buf;
    env.set_var(std::move(enval));
    // GID (non-standard, but useful)
    enval = "GID=";
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)pwent->pw_gid);
    enval += buf;
    env.set_var(std::move(enval));
}

// Parse a userid parameter which may be a numeric user ID or a username. If a name, the
// userid is looked up via the system user database (getpwnam() function). In this case,
// the associated group is stored in the location specified by the group_p parameter if
// it is not null.
inline uid_t parse_uid_param(unsigned line_num, const std::string &param, const std::string &service_name,
        const char *setting_name, gid_t *group_p)
{
    const char * uid_err_msg = "specified user id contains invalid numeric characters "
            "or is outside allowed range.";

    // Could be a name or a numeric id. But we should assume numeric first, just in case
    // a user manages to give themselves a username that parses as a number.
    std::size_t ind = 0;
    try {
        // POSIX does not specify whether uid_t is a signed or unsigned type, but regardless
        // is is probably safe to assume that valid values are positive. We'll also assert
        // that the value range fits within "unsigned long long" since it seems unlikely
        // that would ever not be the case.
        static_assert((uintmax_t)std::numeric_limits<uid_t>::max()
                <= (uintmax_t)std::numeric_limits<unsigned long long>::max(), "uid_t is too large");
        unsigned long long v = std::stoull(param, &ind, 0);
        if (v > static_cast<unsigned long long>(std::numeric_limits<uid_t>::max())
                || ind != param.length()) {
            throw service_description_exc(service_name, std::string(setting_name) + ": " + uid_err_msg, line_num);
        }
        return v;
    }
    catch (std::out_of_range &exc) {
        throw service_description_exc(service_name, uid_err_msg, line_num);
    }
    catch (std::invalid_argument &exc) {
        // Ok, so it doesn't look like a number: proceed...
    }

    errno = 0;
    struct passwd * pwent = getpwnam(param.c_str());
    if (pwent == nullptr) {
        // Maybe an error, maybe just no entry.
        if (errno == 0) {
            throw service_description_exc(service_name, std::string(setting_name) + ": specified user \"" + param
                    + "\" does not exist in system database.", line_num);
        }
        else {
            throw service_description_exc(service_name, std::string("error accessing user database: ")
                    + strerror(errno), line_num);
        }
    }

    if (group_p) {
        *group_p = pwent->pw_gid;
    }

    return pwent->pw_uid;
}

inline gid_t parse_gid_param(unsigned line_num, const std::string &param, const char *setting_name,
        const std::string &service_name)
{
    const char * gid_err_msg = "specified group id contains invalid numeric characters or is "
            "outside allowed range.";

    // Could be a name or a numeric id. But we should assume numeric first, just in case
    // a user manages to give themselves a username that parses as a number.
    std::size_t ind = 0;
    try {
        // POSIX does not specify whether uid_t is an signed or unsigned, but regardless
        // is is probably safe to assume that valid values are positive. We'll also assume
        // that the value range fits with "unsigned long long" since it seems unlikely
        // that would ever not be the case.
        static_assert((uintmax_t)std::numeric_limits<gid_t>::max()
                <= (uintmax_t)std::numeric_limits<unsigned long long>::max(), "gid_t is too large");
        unsigned long long v = std::stoull(param, &ind, 0);
        if (v > static_cast<unsigned long long>(std::numeric_limits<gid_t>::max())
                || ind != param.length()) {
            throw service_description_exc(service_name, std::string(setting_name) + ": " + gid_err_msg, line_num);
        }
        return v;
    }
    catch (std::out_of_range &exc) {
        throw service_description_exc(service_name, std::string(setting_name) + ": " + gid_err_msg, line_num);
    }
    catch (std::invalid_argument &exc) {
        // Ok, so it doesn't look like a number: proceed...
    }

    errno = 0;
    struct group * grent = getgrnam(param.c_str());
    if (grent == nullptr) {
        // Maybe an error, maybe just no entry.
        if (errno == 0) {
            throw service_description_exc(service_name, std::string(setting_name) + ": specified group \"" + param
                    + "\" does not exist in system database.", line_num);
        }
        else {
            throw service_description_exc(service_name, std::string("error accessing group database: ")
                    + strerror(errno), line_num);
        }
    }

    return grent->gr_gid;
}

// Parse a permissions mask parameter value, specified as an octal (such as 0600)
inline int parse_perms(unsigned line_num, string &paramval, const std::string &servicename,
        const char * paramname)
{
    std::size_t ind = 0;
    try {
        int perms = std::stoi(paramval, &ind, 8);
        if (ind != paramval.length()) {
            throw std::logic_error("");
        }
        return perms;
    }
    catch (std::logic_error &exc) {
        throw service_description_exc(servicename, std::string(paramname) + ": badly-formed or "
                "out-of-range numeric value", line_num);
    }
}

// Parse a time, specified as a decimal number of seconds (with optional fractional component after decimal
// point or decimal comma).
inline void parse_timespec(unsigned line_num, const std::string &paramval, const std::string &servicename,
        const char * paramname, timespec &ts)
{
    decltype(ts.tv_sec) isec = 0;
    decltype(ts.tv_nsec) insec = 0;
    auto max_secs = std::numeric_limits<decltype(isec)>::max() / 10;
    auto len = paramval.length();
    decltype(len) i;
    for (i = 0; i < len; i++) {
        char ch = paramval[i];
        if (ch == '.' || ch == ',') {
            i++;
            break;
        }
        if (ch < '0' || ch > '9') {
            throw service_description_exc(servicename, std::string("bad value for ") + paramname, line_num);
        }
        // check for overflow
        if (isec >= max_secs) {
           throw service_description_exc(servicename, std::string("too-large value for ") + paramname, line_num);
        }
        isec *= 10;
        isec += ch - '0';
    }
    decltype(insec) insec_m = 100000000; // 10^8
    for ( ; i < len; i++) {
        char ch = paramval[i];
        if (ch < '0' || ch > '9') {
            throw service_description_exc(servicename, std::string("bad value for ") + paramname, line_num);
        }
        insec += (ch - '0') * insec_m;
        insec_m /= 10;
    }
    ts.tv_sec = isec;
    ts.tv_nsec = insec;
}

// Parse an unsigned numeric parameter value
inline unsigned long long parse_unum_param(unsigned line_num, const std::string &param,
        const std::string &service_name, unsigned long long max = std::numeric_limits<unsigned long long>::max())
{
    const char * num_err_msg = "specified value contains invalid numeric characters or is outside "
            "allowed range.";

    std::size_t ind = 0;
    try {
        unsigned long long v = std::stoull(param, &ind, 0);
        if (v > max || ind != param.length()) {
            throw service_description_exc(service_name, num_err_msg, line_num);
        }
        return v;
    }
    catch (std::out_of_range &exc) {
        throw service_description_exc(service_name, num_err_msg, line_num);
    }
    catch (std::invalid_argument &exc) {
        throw service_description_exc(service_name, num_err_msg, line_num);
    }
}

// In a vector, find or create rlimits for a particular resource type.
inline service_rlimits &find_rlimits(std::vector<service_rlimits> &all_rlimits, int resource_id)
{
    for (service_rlimits &limits : all_rlimits) {
        if (limits.resource_id == resource_id) {
            return limits;
        }
    }

    all_rlimits.emplace_back(resource_id);
    return all_rlimits.back();
}

// Parse resource limits setting (can specify both hard and soft limit).
inline void parse_rlimit(const std::string &line, unsigned line_num, const std::string &service_name,
        const char *param_name, service_rlimits &rlimit)
{
    // Examples:
    // 4:5 - soft:hard limits both set
    // 4:-   soft set, hard set to unlimited
    // 4:    soft set, hard limit unchanged
    // 4     soft and hard limit set to same limit

    if (line.empty()) {
        throw service_description_exc(service_name, std::string(param_name) + ": bad value.", line_num);
    }

    const char *cline = line.c_str();
    rlimit.hard_set = rlimit.soft_set = false;

    try {
        const char * index = cline;
        errno = 0;
        if (cline[0] != ':') {
            rlimit.soft_set = true;
            if (cline[0] == '-') {
                rlimit.limits.rlim_cur = RLIM_INFINITY;
                index = cline + 1;
            }
            else {
                errno = 0;
                char *nindex;
                unsigned long long limit = std::strtoull(cline, &nindex, 0);
                index = nindex;
                if (errno == ERANGE || limit > std::numeric_limits<rlim_t>::max()) throw std::out_of_range("");
                if (index == cline) throw std::invalid_argument("");
                rlimit.limits.rlim_cur = limit;
            }

            if (*index == 0) {
                rlimit.hard_set = true;
                rlimit.limits.rlim_max = rlimit.limits.rlim_cur;
                return;
            }

            if (*index != ':') {
                throw service_description_exc(service_name, std::string(param_name) + ": bad value.", line_num);
            }
        }

        index++;
        if (*index == 0) return;

        rlimit.hard_set = true;

        if (*index == '-') {
            rlimit.limits.rlim_max = RLIM_INFINITY;
            if (index[1] != 0) {
                throw service_description_exc(service_name, std::string(param_name) + ": bad value.", line_num);
            }
        }
        else {
            const char *hard_start = index;
            char *nindex;
            errno = 0;
            unsigned long long limit = std::strtoull(hard_start, &nindex, 0);
            index = nindex;
            if (errno == ERANGE || limit > std::numeric_limits<rlim_t>::max()) throw std::out_of_range("");
            if (index == hard_start) throw std::invalid_argument("");
            rlimit.limits.rlim_max = limit;
        }
    }
    catch (std::invalid_argument &exc) {
        throw service_description_exc(service_name, std::string(param_name) + ": bad value.", line_num);
    }
    catch (std::out_of_range &exc) {
        throw service_description_exc(service_name, std::string(param_name) + ": too-large value.", line_num);
    }
}

// Process an opened service file, line by line.
//    name - the service name
//    service_file - the service file input stream
//    func - a function of the form:
//             void(string &line, string &setting, string_iterator i, string_iterator end)
//           Called with:
//               line - the complete line (excluding newline character)
//               setting - the setting name, from the beginning of the line
//               i - iterator at the beginning of the setting value
//               end - iterator marking the end of the line
//
// May throw service load exceptions or I/O exceptions if enabled on stream.
template <typename T>
void process_service_file(string name, std::istream &service_file, T func)
{
    string line;
    unsigned line_num = 0;

    while (getline(service_file, line)) {
        ++line_num;
        string::iterator i = line.begin();
        string::iterator end = line.end();

        i = skipws(i, end);
        if (i != end) {
            if (*i == '#') {
                continue;  // comment line
            }
            string setting = read_config_name(i, end);
            i = skipws(i, end);
            if (setting.empty() || i == end || (*i != '=' && *i != ':')) {
                throw service_description_exc(name, "badly formed line.", line_num);
            }
            i = skipws(++i, end);

            func(line, line_num, setting, i, end);
        }
    }
}

// A dummy lint-reporting "function".
static auto dummy_lint = [](const char *){};

// Resolve leading variables in paths using the environment
static auto resolve_env_var = [](const string &name, environment::env_map const &envmap){
    return envmap.lookup(name);
};

// Substitute variable references in a value with their values. Specified offsets must give
// the location of separate arguments after word splitting and are adjusted appropriately.
// If you simply wish to substitute all variables in the given string, pass an offsets list
// containing one pair with the string's bounds (0, size). '$$' resolves to a single '$'.
//
// throws: setting_exception if a $-substitution is ill-formed, or if the command line is too long;
//         bad_alloc on allocation failure
template <typename T>
static void value_var_subst(const char *setting_name, std::string &line,
        std::list<std::pair<unsigned,unsigned>> &offsets, T &var_resolve,
        environment::env_map const &envmap)
{
    auto dindx = line.find('$');
    if (dindx == string::npos) {
        return;
    }

    if (line.length() > (size_t)std::numeric_limits<int>::max()) {
        // (avoid potential for overflow later)
        throw service_description_exc(setting_name, "value too long");
    }

    auto i = offsets.begin();
    unsigned xpos = 0; // position to copy from
    std::string r_line;
    int offadj = 0;

    while (i != offsets.end()) {

        i->first += offadj; // don't adjust end yet

        while (i->second > dindx) {
            r_line.append(line, xpos, dindx - xpos); // copy unmatched part
            if (line[dindx + 1] == '$') {
                // double dollar, collapse to single
                r_line += '$';
                xpos = dindx + 2;
                --offadj;
            }
            else {
                // variable
                auto token_end = std::next(line.begin(), i->second);
                bool brace = line[dindx + 1] == '{';
                auto i = std::next(line.begin(), dindx + 1 + int(brace));
                // read environment variable name
                string name = read_config_name(i, token_end, true);
                if (name.empty()) {
                    throw service_description_exc(setting_name, "invalid/missing variable name after '$'");
                }
                char altmode = '\0';
                bool colon = false;
                auto altbeg = i, altend = i;
                if (brace) {
                    /* ${foo+val}, ${foo-val}, ${foo:+val}, ${foo:-val} */
                    if (*i == ':') {
                        colon = true;
                        ++i;
                        if (*i != '+' && *i != '-') {
                            throw service_description_exc(setting_name, "invalid syntax in variable substitution");
                        }
                    }
                    if (*i == '+' || *i == '-') {
                        altmode = *i;
                        altbeg = ++i;
                        while (i != token_end && *i != '}') {
                            ++i;
                        }
                        altend = i;
                    }
                    if (*i != '}') {
                        throw service_description_exc(setting_name, "unmatched '{' in variable substitution");
                    }
                    ++i;
                }
                size_t line_len_before = r_line.size();
                auto *resolved = var_resolve(name, envmap);
                /* apply shell-like substitutions */
                if (altmode == '-') {
                    if (!resolved || (colon && !*resolved)) {
                        r_line.append(altbeg, altend);
                    } else if (resolved) {
                        r_line.append(resolved);
                    }
                } else if (altmode == '+') {
                    if (resolved && (!colon || *resolved)) {
                        r_line.append(altbeg, altend);
                    }
                } else if (resolved) {
                    r_line.append(resolved);
                }
                size_t line_len_after = r_line.size();

                if (line_len_after > (size_t)std::numeric_limits<int>::max()) {
                    // (avoid potential overflow)
                    throw service_description_exc(setting_name, "value too long (after substitution)");
                }

                xpos = i - line.begin();
                int name_len = xpos - dindx;

                offadj += (int)line_len_after - (int)line_len_before - name_len;
            }

            dindx = line.find('$', xpos);
        }

        i->second += offadj;
        ++i;

        while (i != offsets.end() && i->second < dindx) {
            i->first += offadj;
            i->second += offadj;
            ++i;
        }
    }

    r_line.append(line, xpos); // copy final unmatched part
    line = std::move(r_line);
}

// A wrapper type for service parameters. It is parameterised by dependency type.
template <class dep_type>
class service_settings_wrapper
{
    template <typename A, typename B> using pair = std::pair<A,B>;
    template <typename A> using list = std::list<A>;

    public:

    ha_string command;
    list<pair<unsigned,unsigned>> command_offsets; // [start,end) offset of each arg (inc. executable)
    ha_string stop_command;
    list<pair<unsigned,unsigned>> stop_command_offsets;
    string working_dir;
    string pid_file;
    string env_file;

    bool export_passwd_vars = false;
    bool export_service_name = false;

    service_type_t service_type = service_type_t::INTERNAL;
    list<dep_type> depends;
    list<std::string> before_svcs;
    list<std::string> after_svcs;
    log_type_id log_type = log_type_id::NONE;
    string logfile;
    int logfile_perms = 0600;
    uid_t logfile_uid = -1;
    gid_t logfile_uid_gid = -1; // Primary group of logfile owner if known
    gid_t logfile_gid = -1;
    unsigned max_log_buffer_sz = 4096;
    service_flags_t onstart_flags;
    int term_signal = SIGTERM;  // termination signal
    bool auto_restart = DEFAULT_AUTO_RESTART;
    bool smooth_recovery = false;
    string socket_path;
    int socket_perms = 0666;
    // Note: Posix allows that uid_t and gid_t may be unsigned types, but eg chown uses -1 as an
    // invalid value, so it's safe to assume that we can do the same:
    uid_t socket_uid = -1;
    gid_t socket_uid_gid = -1;  // primary group of socket user if known
    gid_t socket_gid = -1;
    // Restart limit interval / count; default is 10 seconds, 3 restarts:
    timespec restart_interval = { .tv_sec = 10, .tv_nsec = 0 };
    int max_restarts = 3;
    timespec restart_delay = { .tv_sec = 0, .tv_nsec = 200000000 };
    timespec stop_timeout = { .tv_sec = DEFAULT_STOP_TIMEOUT, .tv_nsec = 0 };
    timespec start_timeout = { .tv_sec = DEFAULT_START_TIMEOUT, .tv_nsec = 0 };
    std::vector<service_rlimits> rlimits;

    int readiness_fd = -1;      // readiness fd in service process
    string readiness_var;  // environment var to hold readiness fd

    uid_t run_as_uid = -1;
    gid_t run_as_uid_gid = -1; // primary group of "run as" uid if known
    gid_t run_as_gid = -1;

    string chain_to_name;
    string consumer_of_name;

    #if SUPPORT_CGROUPS
    string run_in_cgroup;
    #endif

    #if USE_UTMPX
    char inittab_id[sizeof(utmpx().ut_id)] = {0};
    char inittab_line[sizeof(utmpx().ut_line)] = {0};
    #endif

    // Finalise settings (after processing all setting lines), perform some basic sanity checks and
    // optionally some additional lint checks. May throw service_description_exc
    //
    // Note: we have the do_report_lint parameter to prevent code (and strings) being emitted for lint
    // checks even when the dummy_lint function is used. (Ideally the compiler would optimise them away).
    template <typename T, typename U = decltype(dummy_lint), typename V = decltype(resolve_env_var),
            bool do_report_lint = !std::is_same<U, decltype(dummy_lint)>::value>
    void finalise(T &report_error, environment::env_map const &envmap, U &report_lint = dummy_lint, V &var_subst = resolve_env_var)
    {
        if (service_type == service_type_t::PROCESS || service_type == service_type_t::BGPROCESS
                || service_type == service_type_t::SCRIPTED) {
            if (command.empty()) {
                report_error("'command' setting not specified.");
            }
        }

        if (do_report_lint && (service_type == service_type_t::INTERNAL
                || service_type == service_type_t::TRIGGERED)) {
            if (!command.empty()) {
                report_lint("'command' specified, but ignored for the specified (or default) service type.");
            }
            if (!stop_command.empty()) {
                report_lint("'stop-command' specified, but ignored for the specified (or default) service type.");
            }
            if (!working_dir.empty()) {
                report_lint("'working-dir' specified, but ignored for the specified (or default) service type.");
            }
            #if SUPPORT_CGROUPS
            if (!run_in_cgroup.empty()) {
                report_lint("'run-in-cgroup' specified, but ignored for the specified (or default) service type.");
            }
            #endif
            if (run_as_uid != (uid_t)-1) {
                report_lint("'run-as' specified, but ignored for the specified (or default) service type.");
            }
            if (!socket_path.empty()) {
                report_lint("'socket-listen' specified, but ignored for the specified (or default) service type'.");
            }
            #if USE_UTMPX
            if (inittab_id[0] != 0 || inittab_line[0] != 0) {
                report_lint("'inittab_line' or 'inittab_id' specified, but ignored for the specified (or default) service type.");
            }
            #endif
            if (onstart_flags.signal_process_only || onstart_flags.start_interruptible) {
                report_lint("signal options were specified, but ignored for the specified (or default) service type.");
            }
            if (onstart_flags.pass_cs_fd) {
                report_lint("option 'pass_cs_fd' was specified, but ignored for the specified (or default) service type.");
            }
            if (onstart_flags.skippable) {
                report_lint("option 'skippable' was specified, but ignored for the specified (or default) service type.");
            }
            if (log_type != log_type_id::NONE) {
                report_lint("option 'log_type' was specified, but ignored for the specified (or default) service type.");
            }
        }

        if (do_report_lint) {
            if (log_type != log_type_id::LOGFILE && !logfile.empty()) {
                report_lint("option 'logfile' was specified, but selected log type is not 'file'");
            }
            if (log_type == log_type_id::LOGFILE && logfile.empty()) {
                report_lint("option 'logfile' not set, but selected log type is 'file'");
            }
        }

        if (service_type == service_type_t::BGPROCESS) {
            if (pid_file.empty()) {
                report_error("process ID file ('pid-file') not specified for bgprocess service.");
            }

            if (readiness_fd != -1 || !readiness_var.empty()) {
                report_error("readiness notification ('ready-notification') is not supported "
                        "for bgprocess services.");
            }
        }

        if (onstart_flags.kill_all_on_stop && service_type != service_type_t::INTERNAL
                && service_type != service_type_t::SCRIPTED) {
            report_error("kill-all-on-stop can only be set on scripted or internal services.");
        }

        // Resolve paths via variable substitution
        {
            std::list<std::pair<unsigned, unsigned>> offsets;
            /* reserve item */
            offsets.emplace_back(0, 0);
            auto do_resolve = [&](const char *setting_name, string &setting_value) {
                try {
                    offsets.front().first = 0;
                    offsets.front().second = setting_value.size();
                    value_var_subst(setting_name, setting_value, offsets, var_subst, envmap);
                }
                catch (service_description_exc &exc) {
                    report_error((string() + setting_name + ": " + exc.exc_description).c_str());
                }
            };

            do_resolve("socket-listen", socket_path);
            do_resolve("logfile", logfile);
            do_resolve("working-dir", working_dir);
            do_resolve("pid-file", pid_file);
        }

        // If socket_gid hasn't been explicitly set, but the socket_uid was specified as a name (and
        // we therefore recovered the primary group), use the primary group of the specified user.
        if (socket_gid == (gid_t)-1) socket_gid = socket_uid_gid;
        // Also for logfile_uid/gid, we reset logfile ownership to dinit process uid/gid if uid/gid
        // wasn't specified by service
        if (logfile_uid == (uid_t) -1) logfile_uid = getuid();
        if (logfile_gid == (gid_t)-1) logfile_gid = logfile_uid_gid;
        if (logfile_gid == (gid_t)-1) logfile_gid = getgid();
        // likewise for "run as" gid/uid, but only if we aren't supporting supplementary group initialisation
        // (if we do support supplementary groups, run_as_gid==-1 means "use the user groups including
        // supplementary groups" whereas run_as_gid==X means "use group X with no supplementary groups").
#if USE_INITGROUPS
        if (run_as_uid_gid == (gid_t)-1 && run_as_gid == (gid_t)-1) {
            // run-as-UID specified by number; set run_as_gid to inhibit initialisation of
            // supplementary groups
            run_as_gid = getgid();
        }
#else // !USE_INITGROUPS
        if (run_as_gid == (gid_t)-1) run_as_gid = run_as_uid_gid;
#endif

        if (log_type != log_type_id::LOGFILE) {
            logfile.clear();
        }

        if (!value(service_type).is_in(service_type_t::PROCESS, service_type_t::BGPROCESS)) {
            if (!consumer_of_name.empty()) {
                report_error("only a process or bgprocess service can be a log consumer ('consume-for') another service.");
            }
        }
    }
};

// Process a service description line. In general, parse the setting value and record the parsed value
// in a service settings wrapper object. Errors will be reported via service_description_exc exception.
//
// type parameters:
//     settings_wrapper : wrapper for service settings
//     load_service_t   : type of load service function/lambda (see below)
//     process_dep_dir_t : type of process_dep_dir funciton/lambda (see below)
//
// parameters:
//     settings     : wrapper object for service settings
//     name         : name of the service being processed
//     line         : the current line of the service description file
//     setting      : the name of the setting (from the beginning of line)
//     i            : iterator at beginning of setting value (including whitespace)
//     end          : iterator at end of line
//     load_service : function to load a service
//                      arguments:  const char *service_name
//                      return: a value that can be used (with a dependency type) to construct a dependency
//                              in the 'depends' vector within the 'settings' object
//     process_dep_dir : function to process a dependency directory
//                      arguments: decltype(settings.depends) &dependencies
//                                 const string &waitsford - directory as specified in parameter
//                                 dependency_type dep_type - type of dependency to add
template <typename settings_wrapper,
    typename load_service_t,
    typename process_dep_dir_t>
void process_service_line(settings_wrapper &settings, const char *name, string &line, unsigned line_num,
        string &setting, string::iterator &i, string::iterator &end, load_service_t load_service,
        process_dep_dir_t process_dep_dir)
{
    if (setting == "command") {
        settings.command = read_setting_value(line_num, i, end, &settings.command_offsets);
    }
    else if (setting == "working-dir") {
        settings.working_dir = read_setting_value(line_num, i, end, nullptr);
    }
    else if (setting == "env-file") {
        settings.env_file = read_setting_value(line_num, i, end, nullptr);
    }
    #if SUPPORT_CGROUPS
    else if (setting == "run-in-cgroup") {
        settings.run_in_cgroup = read_setting_value(line_num, i, end, nullptr);
    }
    #endif
    else if (setting == "socket-listen") {
        settings.socket_path = read_setting_value(line_num, i, end, nullptr);
    }
    else if (setting == "socket-permissions") {
        string sock_perm_str = read_setting_value(line_num, i, end, nullptr);
        settings.socket_perms = parse_perms(line_num, sock_perm_str, name, "socket-permissions");
    }
    else if (setting == "socket-uid") {
        string sock_uid_s = read_setting_value(line_num, i, end, nullptr);
        settings.socket_uid = parse_uid_param(line_num, sock_uid_s, name, "socket-uid", &settings.socket_uid_gid);
    }
    else if (setting == "socket-gid") {
        string sock_gid_s = read_setting_value(line_num, i, end, nullptr);
        settings.socket_gid = parse_gid_param(line_num, sock_gid_s, "socket-gid", name);
    }
    else if (setting == "stop-command") {
        settings.stop_command = read_setting_value(line_num, i, end, &settings.stop_command_offsets);
    }
    else if (setting == "pid-file") {
        settings.pid_file = read_setting_value(line_num, i, end);
    }
    else if (setting == "depends-on") {
        string dependency_name = read_setting_value(line_num, i, end);
        settings.depends.emplace_back(load_service(dependency_name.c_str()), dependency_type::REGULAR);
    }
    else if (setting == "depends-ms") {
        string dependency_name = read_setting_value(line_num, i, end);
        settings.depends.emplace_back(load_service(dependency_name.c_str()), dependency_type::MILESTONE);
    }
    else if (setting == "waits-for") {
        string dependency_name = read_setting_value(line_num, i, end);
        settings.depends.emplace_back(load_service(dependency_name.c_str()), dependency_type::WAITS_FOR);
    }
    else if (setting == "waits-for.d") {
        string waitsford = read_setting_value(line_num, i, end);
        process_dep_dir(settings.depends, waitsford, dependency_type::WAITS_FOR);
    }
    else if (setting == "after") {
        string after_name = read_setting_value(line_num, i, end);
        settings.after_svcs.emplace_back(std::move(after_name));
    }
    else if (setting == "before") {
        string before_name = read_setting_value(line_num, i, end);
        settings.before_svcs.emplace_back(std::move(before_name));
    }
    else if (setting == "logfile") {
        settings.logfile = read_setting_value(line_num, i, end);
        if (!settings.logfile.empty() && settings.log_type == log_type_id::NONE) {
            settings.log_type = log_type_id::LOGFILE;
        }
    }
    else if (setting == "logfile-permissions") {
        string log_perm_str = read_setting_value(line_num, i, end, nullptr);
        settings.logfile_perms = parse_perms(line_num, log_perm_str, name, "logfile-permissions");
    }
    else if (setting == "logfile-uid") {
        string log_uid_s = read_setting_value(line_num, i, end, nullptr);
        settings.logfile_uid = parse_uid_param(line_num, log_uid_s, name, "logfile-uid", &settings.logfile_uid_gid);
    }
    else if (setting == "logfile-gid") {
        string log_gid_s = read_setting_value(line_num, i, end, nullptr);
        settings.logfile_gid = parse_gid_param(line_num, log_gid_s, name, "logfile-gid");
    }
    else if (setting == "log-type") {
        string log_type_str = read_setting_value(line_num, i, end);
        if (log_type_str == "file") {
            settings.log_type = log_type_id::LOGFILE;
        }
        else if (log_type_str == "buffer") {
            settings.log_type = log_type_id::BUFFER;
        }
        else if (log_type_str == "none") {
            settings.log_type = log_type_id::NONE;
        }
        else if (log_type_str == "pipe") {
            settings.log_type = log_type_id::PIPE;
        }
        else {
            throw service_description_exc(name, "log type must be one of: \"file\", \"buffer\" or \"none\"",
                    line_num);
        }
    }
    else if (setting == "log-buffer-size") {
        string log_buffer_size_str = read_setting_value(line_num, i, end);
        unsigned bufsize = (unsigned)parse_unum_param(line_num, log_buffer_size_str, name,
                std::numeric_limits<unsigned>::max() / 2);
        settings.max_log_buffer_sz = bufsize;
    }
    else if (setting == "consumer-of") {
        string consumed_svc_name = read_setting_value(line_num, i, end);
        settings.consumer_of_name = consumed_svc_name;
    }
    else if (setting == "restart") {
        string restart = read_setting_value(line_num, i, end);
        settings.auto_restart = (restart == "yes" || restart == "true");
    }
    else if (setting == "smooth-recovery") {
        string recovery = read_setting_value(line_num, i, end);
        settings.smooth_recovery = (recovery == "yes" || recovery == "true");
    }
    else if (setting == "type") {
        string type_str = read_setting_value(line_num, i, end);
        if (type_str == "scripted") {
            settings.service_type = service_type_t::SCRIPTED;
        }
        else if (type_str == "process") {
            settings.service_type = service_type_t::PROCESS;
        }
        else if (type_str == "bgprocess") {
            settings.service_type = service_type_t::BGPROCESS;
        }
        else if (type_str == "internal") {
            settings.service_type = service_type_t::INTERNAL;
        }
        else if (type_str == "triggered") {
            settings.service_type = service_type_t::TRIGGERED;
        }
        else {
            throw service_description_exc(name, "service type must be one of: \"scripted\","
                " \"process\", \"bgprocess\", \"internal\" or \"triggered\"", line_num);
        }
    }
    else if (setting == "options") {
        std::list<std::pair<unsigned,unsigned>> indices;
        string onstart_cmds = read_setting_value(line_num, i, end, &indices);
        for (auto indexpair : indices) {
            string option_txt = onstart_cmds.substr(indexpair.first,
                    indexpair.second - indexpair.first);
            if (option_txt == "starts-rwfs") {
                settings.onstart_flags.rw_ready = true;
            }
            else if (option_txt == "starts-log") {
                settings.onstart_flags.log_ready = true;
            }
            else if (option_txt == "runs-on-console") {
                settings.onstart_flags.runs_on_console = true;
                // A service that runs on the console necessarily starts on console:
                settings.onstart_flags.starts_on_console = true;
                settings.onstart_flags.shares_console = false;
            }
            else if (option_txt == "starts-on-console") {
                settings.onstart_flags.starts_on_console = true;
                settings.onstart_flags.shares_console = false;
            }
            else if (option_txt == "shares-console") {
                settings.onstart_flags.shares_console = true;
                settings.onstart_flags.runs_on_console = false;
                settings.onstart_flags.starts_on_console = false;
            }
            else if (option_txt == "unmask-intr") {
                settings.onstart_flags.unmask_intr = true;
            }
            else if (option_txt == "pass-cs-fd") {
                settings.onstart_flags.pass_cs_fd = true;
            }
            else if (option_txt == "start-interruptible") {
                settings.onstart_flags.start_interruptible = true;
            }
            else if (option_txt == "skippable") {
                settings.onstart_flags.skippable = true;
            }
            else if (option_txt == "signal-process-only") {
                settings.onstart_flags.signal_process_only = true;
            }
            else if (option_txt == "always-chain") {
                settings.onstart_flags.always_chain = true;
            }
            else if (option_txt == "kill-all-on-stop") {
                settings.onstart_flags.kill_all_on_stop = true;
            }
            else {
                throw service_description_exc(name, "Unknown option: " + option_txt, line_num);
            }
        }
    }
    else if (setting == "load-options") {
        std::list<std::pair<unsigned,unsigned>> indices;
        string load_opts = read_setting_value(line_num, i, end, &indices);
        for (auto indexpair : indices) {
            string option_txt = load_opts.substr(indexpair.first,
                    indexpair.second - indexpair.first);
            if (option_txt == "export-passwd-vars") {
                settings.export_passwd_vars = true;
            }
            else if (option_txt == "export-service-name") {
                settings.export_service_name = true;
            }
            else if (option_txt == "sub-vars") {
                // noop: for backwards compatibility only
                // we don't support no-sub-vars anymore, however
            }
            else {
                throw service_description_exc(name, "unknown load option: " + option_txt, line_num);
            }
        }
    }
    else if (setting == "term-signal" || setting == "termsignal") {
        // Note: "termsignal" supported for legacy reasons.
        string signame = read_setting_value(line_num, i, end, nullptr);
        int signo = signal_name_to_number(signame);
        if (signo == -1) {
            throw service_description_exc(name, "unknown/unsupported termination signal: "
                    + signame, line_num);
        }
        else {
            settings.term_signal = signo;
        }
    }
    else if (setting == "restart-limit-interval") {
        string interval_str = read_setting_value(line_num, i, end, nullptr);
        parse_timespec(line_num, interval_str, name, "restart-limit-interval", settings.restart_interval);
    }
    else if (setting == "restart-delay") {
        string rsdelay_str = read_setting_value(line_num, i, end, nullptr);
        parse_timespec(line_num, rsdelay_str, name, "restart-delay", settings.restart_delay);
    }
    else if (setting == "restart-limit-count") {
        string limit_str = read_setting_value(line_num, i, end, nullptr);
        settings.max_restarts = parse_unum_param(line_num, limit_str, name, std::numeric_limits<int>::max());
    }
    else if (setting == "stop-timeout") {
        string stoptimeout_str = read_setting_value(line_num, i, end, nullptr);
        parse_timespec(line_num, stoptimeout_str, name, "stop-timeout", settings.stop_timeout);
    }
    else if (setting == "start-timeout") {
        string starttimeout_str = read_setting_value(line_num, i, end, nullptr);
        parse_timespec(line_num, starttimeout_str, name, "start-timeout", settings.start_timeout);
    }
    else if (setting == "run-as") {
        string run_as_str = read_setting_value(line_num, i, end, nullptr);
        settings.run_as_uid = parse_uid_param(line_num, run_as_str, name, "run-as", &settings.run_as_uid_gid);
    }
    else if (setting == "chain-to") {
        settings.chain_to_name = read_setting_value(line_num, i, end, nullptr);
    }
    else if (setting == "ready-notification") {
        string notify_setting = read_setting_value(line_num, i, end, nullptr);
        if (starts_with(notify_setting, "pipefd:")) {
            settings.readiness_fd = parse_unum_param(line_num, notify_setting.substr(7 /* len 'pipefd:' */),
                    name, std::numeric_limits<int>::max());
        }
        else if (starts_with(notify_setting, "pipevar:")) {
            settings.readiness_var = notify_setting.substr(8 /* len 'pipevar:' */);
            if (settings.readiness_var.empty()) {
                throw service_description_exc(name, "invalid pipevar variable name "
                        "in ready-notification", line_num);
            }
        }
        else {
            throw service_description_exc(name, "unknown ready-notification setting: "
                    + notify_setting, line_num);
        }
    }
    else if (setting == "inittab-id") {
        string inittab_setting = read_setting_value(line_num, i, end, nullptr);
        #if USE_UTMPX
        if (inittab_setting.length() > sizeof(settings.inittab_id)) {
            throw service_description_exc(name, "inittab-id setting is too long", line_num);
        }
        strncpy(settings.inittab_id, inittab_setting.c_str(), sizeof(settings.inittab_id));
        #endif
    }
    else if (setting == "inittab-line") {
        string inittab_setting = read_setting_value(line_num, i, end, nullptr);
        #if USE_UTMPX
        if (inittab_setting.length() > sizeof(settings.inittab_line)) {
            throw service_description_exc(name, "inittab-line setting is too long", line_num);
        }
        strncpy(settings.inittab_line, inittab_setting.c_str(), sizeof(settings.inittab_line));
        #endif
    }
    else if (setting == "rlimit-nofile") {
        string nofile_setting = read_setting_value(line_num, i, end, nullptr);
        service_rlimits &nofile_limits = find_rlimits(settings.rlimits, RLIMIT_NOFILE);
        parse_rlimit(nofile_setting, line_num, name, "rlimit-nofile", nofile_limits);
    }
    else if (setting == "rlimit-core") {
        string core_setting = read_setting_value(line_num, i, end, nullptr);
        service_rlimits &nofile_limits = find_rlimits(settings.rlimits, RLIMIT_CORE);
        parse_rlimit(core_setting, line_num, name, "rlimit-core", nofile_limits);
    }
    else if (setting == "rlimit-data") {
        string data_setting = read_setting_value(line_num, i, end, nullptr);
        service_rlimits &nofile_limits = find_rlimits(settings.rlimits, RLIMIT_DATA);
        parse_rlimit(data_setting, line_num, name, "rlimit-data", nofile_limits);
    }
    else if (setting == "rlimit-addrspace") {
        #if defined(RLIMIT_AS)
            string addrspace_setting = read_setting_value(line_num, i, end, nullptr);
            service_rlimits &nofile_limits = find_rlimits(settings.rlimits, RLIMIT_AS);
            parse_rlimit(addrspace_setting, line_num, name, "rlimit-addrspace", nofile_limits);
        #endif
    }
    else {
        throw service_description_exc(name, "unknown setting: '" + setting + "'.", line_num);
    }
}

} // namespace dinit_load

using dinit_load::process_service_file;
