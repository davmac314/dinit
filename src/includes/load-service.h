#include <iostream>
#include <list>
#include <limits>
#include <csignal>
#include <cstring>

#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <grp.h>
#include <pwd.h>

struct service_flags_t
{
    // on-start flags:
    bool rw_ready : 1;  // file system should be writable once this service starts
    bool log_ready : 1; // syslog should be available once this service starts

    // Other service options flags:
    bool no_sigterm : 1;  // do not send SIGTERM
    bool runs_on_console : 1;  // run "in the foreground"
    bool starts_on_console : 1; // starts in the foreground
    bool shares_console : 1;    // run on console, but not exclusively
    bool pass_cs_fd : 1;  // pass this service a control socket connection via fd
    bool start_interruptible : 1; // the startup of this service process is ok to interrupt with SIGINT
    bool skippable : 1;   // if interrupted the service is skipped (scripted services)
    bool signal_process_only : 1;  // signal the session process, not the whole group

    service_flags_t() noexcept : rw_ready(false), log_ready(false), no_sigterm(false),
            runs_on_console(false), starts_on_console(false), shares_console(false),
            pass_cs_fd(false), start_interruptible(false), skippable(false), signal_process_only(false)
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

    protected:
    service_load_exc(const std::string &serviceName, std::string &&desc) noexcept
        : service_name(serviceName), exc_description(std::move(desc))
    {
    }
};

class service_not_found : public service_load_exc
{
    public:
    service_not_found(const std::string &serviceName) noexcept
        : service_load_exc(serviceName, "Service description not found.")
    {
    }
};

class service_cyclic_dependency : public service_load_exc
{
    public:
    service_cyclic_dependency(const std::string &serviceName) noexcept
        : service_load_exc(serviceName, "Has cyclic dependency.")
    {
    }
};

class service_description_exc : public service_load_exc
{
    public:
    service_description_exc(const std::string &serviceName, std::string &&extraInfo) noexcept
        : service_load_exc(serviceName, std::move(extraInfo))
    {
    }
};

namespace dinit_load {

using string = std::string;
using string_iterator = std::string::iterator;

// exception thrown when encountering a syntax issue when reading a setting value
class setting_exception
{
    std::string info;

    public:
    setting_exception(const std::string &&exc_info) : info(std::move(exc_info))
    {
    }

    std::string &get_info()
    {
        return info;
    }
};


// Utility function to skip white space. Returns an iterator at the
// first non-white-space position (or at end).
inline string_iterator skipws(string_iterator i, string_iterator end)
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

// Convert a signal name to the corresponding signal number
inline int signal_name_to_number(std::string &signame)
{
    if (signame == "HUP") return SIGHUP;
    if (signame == "INT") return SIGINT;
    if (signame == "QUIT") return SIGQUIT;
    if (signame == "USR1") return SIGUSR1;
    if (signame == "USR2") return SIGUSR2;
    if (signame == "KILL") return SIGKILL;
    return -1;
}

// Read a setting name.
inline string read_setting_name(string_iterator & i, string_iterator end)
{
    using std::locale;
    using std::ctype;
    using std::use_facet;

    const ctype<char> & facet = use_facet<ctype<char> >(locale::classic());

    string rval;
    // Allow alphabetical characters, and dash (-) in setting name
    while (i != end && (*i == '-' || *i == '.' || facet.is(ctype<char>::alpha, *i))) {
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
// Throws setting_exception on error.
//
// Params:
//    service_name - the name of the service to which the setting applies
//    i  -  reference to string iterator through the line
//    end -   iterator at end of line (not including newline character if any)
//    part_positions -  list of <int,int> to which the position of each setting value
//                      part will be added as [start,end). May be null.
inline string read_setting_value(string_iterator & i, string_iterator end,
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
                        throw setting_exception("Line end follows backslash escape character (`\\')");
                    }
                }
                else {
                    rval += c;
                }
                ++i;
            }
            if (i == end) {
                // String wasn't terminated
                throw setting_exception("Unterminated quoted string");
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
                throw setting_exception("Backslash escape (`\\') not followed by character");
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
            throw setting_exception("hashmark (`#') comment must be separated from setting value by whitespace");
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

// Parse a userid parameter which may be a numeric user ID or a username. If a name, the
// userid is looked up via the system user database (getpwnam() function). In this case,
// the associated group is stored in the location specified by the group_p parameter iff
// it is not null and iff it contains the value -1.
inline uid_t parse_uid_param(const std::string &param, const std::string &service_name, gid_t *group_p)
{
    const char * uid_err_msg = "Specified user id contains invalid numeric characters "
            "or is outside allowed range.";

    // Could be a name or a numeric id. But we should assume numeric first, just in case
    // a user manages to give themselves a username that parses as a number.
    std::size_t ind = 0;
    try {
        // POSIX does not specify whether uid_t is an signed or unsigned, but regardless
        // is is probably safe to assume that valid values are positive. We'll also assert
        // that the value range fits within "unsigned long long" since it seems unlikely
        // that would ever not be the case.
        static_assert((uintmax_t)std::numeric_limits<uid_t>::max()
                <= (uintmax_t)std::numeric_limits<unsigned long long>::max(), "uid_t is too large");
        unsigned long long v = std::stoull(param, &ind, 0);
        if (v > static_cast<unsigned long long>(std::numeric_limits<uid_t>::max())
                || ind != param.length()) {
            throw service_description_exc(service_name, uid_err_msg);
        }
        return v;
    }
    catch (std::out_of_range &exc) {
        throw service_description_exc(service_name, uid_err_msg);
    }
    catch (std::invalid_argument &exc) {
        // Ok, so it doesn't look like a number: proceed...
    }

    errno = 0;
    struct passwd * pwent = getpwnam(param.c_str());
    if (pwent == nullptr) {
        // Maybe an error, maybe just no entry.
        if (errno == 0) {
            throw service_description_exc(service_name, "Specified user \"" + param
                    + "\" does not exist in system database.");
        }
        else {
            throw service_description_exc(service_name, std::string("Error accessing user database: ")
                    + strerror(errno));
        }
    }

    if (group_p && *group_p != (gid_t)-1) {
        *group_p = pwent->pw_gid;
    }

    return pwent->pw_uid;
}

inline gid_t parse_gid_param(const std::string &param, const std::string &service_name)
{
    const char * gid_err_msg = "Specified group id contains invalid numeric characters or is "
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
            throw service_description_exc(service_name, gid_err_msg);
        }
        return v;
    }
    catch (std::out_of_range &exc) {
        throw service_description_exc(service_name, gid_err_msg);
    }
    catch (std::invalid_argument &exc) {
        // Ok, so it doesn't look like a number: proceed...
    }

    errno = 0;
    struct group * grent = getgrnam(param.c_str());
    if (grent == nullptr) {
        // Maybe an error, maybe just no entry.
        if (errno == 0) {
            throw service_description_exc(service_name, "Specified group \"" + param
                    + "\" does not exist in system database.");
        }
        else {
            throw service_description_exc(service_name, std::string("Error accessing group database: ")
                    + strerror(errno));
        }
    }

    return grent->gr_gid;
}

// Parse a time, specified as a decimal number of seconds (with optional fractional component after decimal
// point or decimal comma).
inline void parse_timespec(const std::string &paramval, const std::string &servicename,
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
            throw service_description_exc(servicename, std::string("Bad value for ") + paramname);
        }
        // check for overflow
        if (isec >= max_secs) {
           throw service_description_exc(servicename, std::string("Too-large value for ") + paramname);
        }
        isec *= 10;
        isec += ch - '0';
    }
    decltype(insec) insec_m = 100000000; // 10^8
    for ( ; i < len; i++) {
        char ch = paramval[i];
        if (ch < '0' || ch > '9') {
            throw service_description_exc(servicename, std::string("Bad value for ") + paramname);
        }
        insec += (ch - '0') * insec_m;
        insec_m /= 10;
    }
    ts.tv_sec = isec;
    ts.tv_nsec = insec;
}

// Parse an unsigned numeric parameter value
inline unsigned long long parse_unum_param(const std::string &param, const std::string &service_name,
        unsigned long long max = std::numeric_limits<unsigned long long>::max())
{
    const char * num_err_msg = "Specified value contains invalid numeric characters or is outside "
            "allowed range.";

    std::size_t ind = 0;
    try {
        unsigned long long v = std::stoull(param, &ind, 0);
        if (v > max || ind != param.length()) {
            throw service_description_exc(service_name, num_err_msg);
        }
        return v;
    }
    catch (std::out_of_range &exc) {
        throw service_description_exc(service_name, num_err_msg);
    }
    catch (std::invalid_argument &exc) {
        throw service_description_exc(service_name, num_err_msg);
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
inline void parse_rlimit(const std::string &line, const std::string &service_name, const char *param_name,
        service_rlimits &rlimit)
{
    // Examples:
    // 4:5 - soft:hard limits both set
    // 4:-   soft set, hard set to unlimited
    // 4:    soft set, hard limit unchanged
    // 4     soft and hard limit set to same limit

    if (line.empty()) {
        throw service_description_exc(service_name, std::string("Bad value for ") + param_name);
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
                throw service_description_exc(service_name, std::string("Bad value for ") + param_name);
            }
        }

        index++;
        if (*index == 0) return;

        if (*index == '-') {
            rlimit.limits.rlim_max = RLIM_INFINITY;
            if (index[1] != 0) {
                throw service_description_exc(service_name, std::string("Bad value for ") + param_name);
            }
        }
        else {
            const char *hard_start = index;
            char *nindex;
            unsigned long long limit = std::strtoull(cline, &nindex, 0);
            index = nindex;
            if (errno == ERANGE || limit > std::numeric_limits<rlim_t>::max()) throw std::out_of_range("");
            if (index == hard_start) throw std::invalid_argument("");
            rlimit.limits.rlim_max = limit;
        }
    }
    catch (std::invalid_argument &exc) {
        throw service_description_exc(service_name, std::string("Bad value for ") + param_name);
    }
    catch (std::out_of_range &exc) {
        throw service_description_exc(service_name, std::string("Too-large value for ") + param_name);
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

    while (getline(service_file, line)) {
        string::iterator i = line.begin();
        string::iterator end = line.end();

        i = skipws(i, end);
        if (i != end) {
            if (*i == '#') {
                continue;  // comment line
            }
            string setting = read_setting_name(i, end);
            i = skipws(i, end);
            if (i == end || (*i != '=' && *i != ':')) {
                throw service_description_exc(name, "Badly formed line.");
            }
            i = skipws(++i, end);

            func(line, setting, i, end);
        }
    }
}

} // namespace dinit_load

using dinit_load::process_service_file;
