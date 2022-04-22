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

#include "dinit-utmp.h"
#include "dinit-util.h"
#include "service-constants.h"

struct service_flags_t
{
    // on-start flags:
    bool rw_ready : 1;  // file system should be writable once this service starts
    bool log_ready : 1; // syslog should be available once this service starts

    // Other service options flags:
    bool runs_on_console : 1;  // run "in the foreground"
    bool starts_on_console : 1; // starts in the foreground
    bool shares_console : 1;    // run on console, but not exclusively
    bool pass_cs_fd : 1;  // pass this service a control socket connection via fd
    bool start_interruptible : 1; // the startup of this service process is ok to interrupt with SIGINT
    bool skippable : 1;   // if interrupted the service is skipped (scripted services)
    bool signal_process_only : 1;  // signal the session process, not the whole group
    bool always_chain : 1; // always start chain-to service on exit

    service_flags_t() noexcept : rw_ready(false), log_ready(false),
            runs_on_console(false), starts_on_console(false), shares_console(false),
            pass_cs_fd(false), start_interruptible(false), skippable(false), signal_process_only(false),
            always_chain(false)
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
    service_load_exc(const std::string &serviceName, std::string &&desc)
        : service_name(serviceName), exc_description(std::move(desc))
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
    service_description_exc(const std::string &serviceName, std::string &&extraInfo)
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

// Convert a signal name to the corresponding signal number
inline int signal_name_to_number(std::string &signame) noexcept
{
    if (signame == "none" || signame == "NONE") return 0;
    if (signame == "HUP") return SIGHUP;
    if (signame == "INT") return SIGINT;
    if (signame == "TERM") return SIGTERM;
    if (signame == "QUIT") return SIGQUIT;
    if (signame == "USR1") return SIGUSR1;
    if (signame == "USR2") return SIGUSR2;
    if (signame == "KILL") return SIGKILL;
    return -1;
}

// Read a setting/variable name; return empty string if no valid name
inline string read_config_name(string_iterator & i, string_iterator end) noexcept
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

    // Within the setting name, allow dash and dot; also allow any non-control, non-punctuation,
    // non-space character.
    while (i != end && (*i == '-' || *i == '.' || *i == '_'
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
                        throw setting_exception("line end follows backslash escape character (`\\')");
                    }
                }
                else {
                    rval += c;
                }
                ++i;
            }
            if (i == end) {
                // String wasn't terminated
                throw setting_exception("unterminated quoted string");
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
                throw setting_exception("backslash escape (`\\') not followed by character");
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
// the associated group is stored in the location specified by the group_p parameter if
// it is not null.
inline uid_t parse_uid_param(const std::string &param, const std::string &service_name, const char *setting_name, gid_t *group_p)
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
            throw service_description_exc(service_name, std::string(setting_name) + ": " + uid_err_msg);
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
            throw service_description_exc(service_name, std::string(setting_name) + ": specified user \"" + param
                    + "\" does not exist in system database.");
        }
        else {
            throw service_description_exc(service_name, std::string("error accessing user database: ")
                    + strerror(errno));
        }
    }

    if (group_p) {
        *group_p = pwent->pw_gid;
    }

    return pwent->pw_uid;
}

inline gid_t parse_gid_param(const std::string &param, const char *setting_name, const std::string &service_name)
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
            throw service_description_exc(service_name, std::string(setting_name) + ": " + gid_err_msg);
        }
        return v;
    }
    catch (std::out_of_range &exc) {
        throw service_description_exc(service_name, std::string(setting_name) + ": " + gid_err_msg);
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
                    + "\" does not exist in system database.");
        }
        else {
            throw service_description_exc(service_name, std::string("error accessing group database: ")
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
            throw service_description_exc(servicename, std::string("bad value for ") + paramname);
        }
        // check for overflow
        if (isec >= max_secs) {
           throw service_description_exc(servicename, std::string("too-large value for ") + paramname);
        }
        isec *= 10;
        isec += ch - '0';
    }
    decltype(insec) insec_m = 100000000; // 10^8
    for ( ; i < len; i++) {
        char ch = paramval[i];
        if (ch < '0' || ch > '9') {
            throw service_description_exc(servicename, std::string("bad value for ") + paramname);
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
    const char * num_err_msg = "specified value contains invalid numeric characters or is outside "
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
        throw service_description_exc(service_name, std::string(param_name) + ": bad value.");
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
                throw service_description_exc(service_name, std::string(param_name) + ": bad value.");
            }
        }

        index++;
        if (*index == 0) return;

        rlimit.hard_set = true;

        if (*index == '-') {
            rlimit.limits.rlim_max = RLIM_INFINITY;
            if (index[1] != 0) {
                throw service_description_exc(service_name, std::string(param_name) + ": bad value.");
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
        throw service_description_exc(service_name, std::string(param_name) + ": bad value.");
    }
    catch (std::out_of_range &exc) {
        throw service_description_exc(service_name, std::string(param_name) + ": too-large value.");
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
            string setting = read_config_name(i, end);
            i = skipws(i, end);
            if (setting.empty() || i == end || (*i != '=' && *i != ':')) {
                throw service_description_exc(name, "badly formed line.");
            }
            i = skipws(++i, end);

            func(line, setting, i, end);
        }
    }
}

// A dummy lint-reporting "function".
static auto dummy_lint = [](const char *){};

// Resolve leading variables in paths using the environment
static auto resolve_env_var = [](const string &name){
    const char *r = getenv(name.c_str());
    if (r == nullptr) {
        return "";
    }
    return r;
};

// Resolve a path with variable substitutions ($varname). '$$' resolves to a single '$'.
// Throws setting_exception on failure.
//    p           - path to resolve
//    var_resolve - function to translate names to values; returning string or const char *;
//                  may throw setting_exception
template <typename T>
inline std::string resolve_path(std::string &&p, T &var_resolve)
{
    auto dpos = p.find('$');
    if (dpos == string::npos) {
        // shortcut the case where there are no substitutions:
        return std::move(p);
    }

    string r;
    string::size_type last_pos = 0;

    do {
        r.append(p, last_pos, dpos - last_pos); // non-substituted portion
        ++dpos;
        if (dpos < p.size() && p[dpos] == '$') {
            // double '$' resolves to a single '$' in output
            r += '$';
            last_pos = dpos + 1;
            dpos = p.find('$', last_pos);
            continue;
        }
        auto i = std::next(p.begin(), dpos);
        string name = read_config_name(i, p.end());
        if (name.empty()) {
            throw setting_exception("invalid/missing variable name after '$'");
        }
        string value = var_resolve(name);
        r.append(value);
        last_pos = i - p.begin();
        dpos = p.find('$', last_pos);
    } while (dpos != string::npos);

    r.append(p, last_pos, string::npos);

    return r;
}

// Substitute variable references in a command line with their values. Specified offsets must give
// the location of separate arguments after word splitting and are adjusted appropriately.
//
// throws: setting_exception if a $-substitution is ill-formed, or if the command line is too long;
//         bad_alloc on allocation failure
template <typename T>
static void cmdline_var_subst(std::string &line, std::list<std::pair<unsigned,unsigned>> &offsets,
        T &var_resolve)
{
    auto dindx = line.find('$');
    if (dindx == string::npos) {
        return;
    }

    if (line.length() > (size_t)std::numeric_limits<int>::max()) {
        // (avoid potential for overflow later)
        throw setting_exception("command line too long");
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
                auto i = std::next(line.begin(), dindx + 1);
                string name = read_config_name(i, line.end());
                if (name.empty()) {
                    throw setting_exception("invalid/missing variable name after '$'");
                }
                size_t line_len_before = r_line.size();
                r_line.append(var_resolve(name));
                size_t line_len_after = r_line.size();

                if (line_len_after > (size_t)std::numeric_limits<int>::max()) {
                    // (avoid potential overflow)
                    throw setting_exception("command line too long (after substitution)");
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
    return;
}

// A wrapper type for service parameters. It is parameterised by dependency type.
template <class dep_type>
class service_settings_wrapper
{
    template <typename A, typename B> using pair = std::pair<A,B>;
    template <typename A> using list = std::list<A>;

    public:

    string command;
    list<pair<unsigned,unsigned>> command_offsets; // [start,end) offset of each arg (inc. executable)
    string stop_command;
    list<pair<unsigned,unsigned>> stop_command_offsets;
    string working_dir;
    string pid_file;
    string env_file;

    bool do_sub_vars = false;

    service_type_t service_type = service_type_t::INTERNAL;
    std::list<dep_type> depends;
    string logfile;
    service_flags_t onstart_flags;
    int term_signal = SIGTERM;  // termination signal
    bool auto_restart = false;
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
    timespec stop_timeout = { .tv_sec = 10, .tv_nsec = 0 };
    timespec start_timeout = { .tv_sec = 60, .tv_nsec = 0 };
    std::vector<service_rlimits> rlimits;

    int readiness_fd = -1;      // readiness fd in service process
    std::string readiness_var;  // environment var to hold readiness fd

    uid_t run_as_uid = -1;
    gid_t run_as_uid_gid = -1; // primary group of "run as" uid if known
    gid_t run_as_gid = -1;

    string chain_to_name;

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
    void finalise(T &report_error, U &report_lint = dummy_lint, V &var_subst = resolve_env_var)
    {
        if (service_type == service_type_t::PROCESS || service_type == service_type_t::BGPROCESS
                || service_type == service_type_t::SCRIPTED) {
            if (command.empty()) {
                report_error("'command' setting not specified.");
            }
        }

        if (do_report_lint && service_type == service_type_t::INTERNAL) {
            if (!command.empty()) {
                report_lint("'command' specified, but 'type' is internal (or not specified).");
            }
            if (!stop_command.empty()) {
                report_lint("'stop-command' specified, but 'type' is internal (or not specified).");
            }
            if (!working_dir.empty()) {
                report_lint("'working-dir' specified, but 'type' is internal (or not specified).");
            }
            if (run_as_uid != (uid_t)-1) {
                report_lint("'run-as' specified, but 'type' is internal (or not specified).");
            }
            if (!socket_path.empty()) {
                report_lint("'socket-listen' specified, but 'type' is internal (or not specified).");
            }
            #if USE_UTMPX
            if (inittab_id[0] != 0 || inittab_line[0] != 0) {
                report_lint("'inittab_line' or 'inittab_id' specified, but 'type' is internal (or not specified).");
            }
            #endif
            if (onstart_flags.signal_process_only || onstart_flags.start_interruptible) {
                report_lint("signal options were specified, but 'type' is internal (or not specified).");
            }
            if (onstart_flags.pass_cs_fd) {
                report_lint("option 'pass_cs_fd' was specified, but 'type' is internal (or not specified).");
            }
            if (onstart_flags.skippable) {
                report_lint("option 'skippable' was specified, but 'type' is internal (or not specified).");
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

        // Resolve paths via variable substitution
        {
            auto do_resolve = [&](const char *setting_name, string &setting_value) {
                try {
                    setting_value = resolve_path(std::move(setting_value), var_subst);
                }
                catch (setting_exception &exc) {
                    report_error((string() + setting_name + ": " + exc.get_info()).c_str());
                }
            };

            do_resolve("socket-listen", socket_path);
            do_resolve("logfile", logfile);
            do_resolve("env-file", env_file);
            do_resolve("working-dir", working_dir);
            do_resolve("pid-file", pid_file);
        }

        // If socket_gid hasn't been explicitly set, but the socket_uid was specified as a name (and
        // we therefore recovered the primary group), use the primary group of the specified user.
        if (socket_gid == (gid_t)-1) socket_gid = socket_uid_gid;
        // likewise for "run as" gid/uid.
        if (run_as_gid == (gid_t)-1) run_as_gid = run_as_uid_gid;
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
void process_service_line(settings_wrapper &settings, const char *name, string &line, string &setting,
        string::iterator &i, string::iterator &end, load_service_t load_service,
        process_dep_dir_t process_dep_dir)
{
    if (setting == "command") {
        settings.command = read_setting_value(i, end, &settings.command_offsets);
    }
    else if (setting == "working-dir") {
        settings.working_dir = read_setting_value(i, end, nullptr);
    }
    else if (setting == "env-file") {
        settings.env_file = read_setting_value(i, end, nullptr);
    }
    else if (setting == "socket-listen") {
        settings.socket_path = read_setting_value(i, end, nullptr);
    }
    else if (setting == "socket-permissions") {
        string sock_perm_str = read_setting_value(i, end, nullptr);
        std::size_t ind = 0;
        try {
            settings.socket_perms = std::stoi(sock_perm_str, &ind, 8);
            if (ind != sock_perm_str.length()) {
                throw std::logic_error("");
            }
        }
        catch (std::logic_error &exc) {
            throw service_description_exc(name, "socket-permissions: badly-formed or "
                    "out-of-range numeric value");
        }
    }
    else if (setting == "socket-uid") {
        string sock_uid_s = read_setting_value(i, end, nullptr);
        settings.socket_uid = parse_uid_param(sock_uid_s, name, "socket-uid", &settings.socket_uid_gid);
    }
    else if (setting == "socket-gid") {
        string sock_gid_s = read_setting_value(i, end, nullptr);
        settings.socket_gid = parse_gid_param(sock_gid_s, "socket-gid", name);
    }
    else if (setting == "stop-command") {
        settings.stop_command = read_setting_value(i, end, &settings.stop_command_offsets);
    }
    else if (setting == "pid-file") {
        settings.pid_file = read_setting_value(i, end);
    }
    else if (setting == "depends-on") {
        string dependency_name = read_setting_value(i, end);
        settings.depends.emplace_back(load_service(dependency_name.c_str()), dependency_type::REGULAR);
    }
    else if (setting == "depends-ms") {
        string dependency_name = read_setting_value(i, end);
        settings.depends.emplace_back(load_service(dependency_name.c_str()), dependency_type::MILESTONE);
    }
    else if (setting == "waits-for") {
        string dependency_name = read_setting_value(i, end);
        settings.depends.emplace_back(load_service(dependency_name.c_str()), dependency_type::WAITS_FOR);
    }
    else if (setting == "waits-for.d") {
        string waitsford = read_setting_value(i, end);
        process_dep_dir(settings.depends, waitsford, dependency_type::WAITS_FOR);
    }
    else if (setting == "logfile") {
        settings.logfile = read_setting_value(i, end);
    }
    else if (setting == "restart") {
        string restart = read_setting_value(i, end);
        settings.auto_restart = (restart == "yes" || restart == "true");
    }
    else if (setting == "smooth-recovery") {
        string recovery = read_setting_value(i, end);
        settings.smooth_recovery = (recovery == "yes" || recovery == "true");
    }
    else if (setting == "type") {
        string type_str = read_setting_value(i, end);
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
        else {
            throw service_description_exc(name, "service type must be one of: \"scripted\","
                " \"process\", \"bgprocess\" or \"internal\"");
        }
    }
    else if (setting == "options") {
        std::list<std::pair<unsigned,unsigned>> indices;
        string onstart_cmds = read_setting_value(i, end, &indices);
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
            else {
                throw service_description_exc(name, "Unknown option: " + option_txt);
            }
        }
    }
    else if (setting == "load-options") {
        std::list<std::pair<unsigned,unsigned>> indices;
        string load_opts = read_setting_value(i, end, &indices);
        for (auto indexpair : indices) {
            string option_txt = load_opts.substr(indexpair.first,
                    indexpair.second - indexpair.first);
            if (option_txt == "sub-vars") {
                // substitute environment variables in command line
                settings.do_sub_vars = true;
            }
            else if (option_txt == "no-sub-vars") {
                settings.do_sub_vars = false;
            }
            else {
                throw service_description_exc(name, "unknown load option: " + option_txt);
            }
        }
    }
    else if (setting == "term-signal" || setting == "termsignal") {
        // Note: "termsignal" supported for legacy reasons.
        string signame = read_setting_value(i, end, nullptr);
        int signo = signal_name_to_number(signame);
        if (signo == -1) {
            throw service_description_exc(name, "unknown/unsupported termination signal: "
                    + signame);
        }
        else {
            settings.term_signal = signo;
        }
    }
    else if (setting == "restart-limit-interval") {
        string interval_str = read_setting_value(i, end, nullptr);
        parse_timespec(interval_str, name, "restart-limit-interval", settings.restart_interval);
    }
    else if (setting == "restart-delay") {
        string rsdelay_str = read_setting_value(i, end, nullptr);
        parse_timespec(rsdelay_str, name, "restart-delay", settings.restart_delay);
    }
    else if (setting == "restart-limit-count") {
        string limit_str = read_setting_value(i, end, nullptr);
        settings.max_restarts = parse_unum_param(limit_str, name, std::numeric_limits<int>::max());
    }
    else if (setting == "stop-timeout") {
        string stoptimeout_str = read_setting_value(i, end, nullptr);
        parse_timespec(stoptimeout_str, name, "stop-timeout", settings.stop_timeout);
    }
    else if (setting == "start-timeout") {
        string starttimeout_str = read_setting_value(i, end, nullptr);
        parse_timespec(starttimeout_str, name, "start-timeout", settings.start_timeout);
    }
    else if (setting == "run-as") {
        string run_as_str = read_setting_value(i, end, nullptr);
        settings.run_as_uid = parse_uid_param(run_as_str, name, "run-as", &settings.run_as_uid_gid);
    }
    else if (setting == "chain-to") {
        settings.chain_to_name = read_setting_value(i, end, nullptr);
    }
    else if (setting == "ready-notification") {
        string notify_setting = read_setting_value(i, end, nullptr);
        if (starts_with(notify_setting, "pipefd:")) {
            settings.readiness_fd = parse_unum_param(notify_setting.substr(7 /* len 'pipefd:' */),
                    name, std::numeric_limits<int>::max());
        }
        else if (starts_with(notify_setting, "pipevar:")) {
            settings.readiness_var = notify_setting.substr(8 /* len 'pipevar:' */);
            if (settings.readiness_var.empty()) {
                throw service_description_exc(name, "invalid pipevar variable name "
                        "in ready-notification");
            }
        }
        else {
            throw service_description_exc(name, "unknown ready-notification setting: "
                    + notify_setting);
        }
    }
    else if (setting == "inittab-id") {
        string inittab_setting = read_setting_value(i, end, nullptr);
        #if USE_UTMPX
        if (inittab_setting.length() > sizeof(settings.inittab_id)) {
            throw service_description_exc(name, "inittab-id setting is too long");
        }
        strncpy(settings.inittab_id, inittab_setting.c_str(), sizeof(settings.inittab_id));
        #endif
    }
    else if (setting == "inittab-line") {
        string inittab_setting = read_setting_value(i, end, nullptr);
        #if USE_UTMPX
        if (inittab_setting.length() > sizeof(settings.inittab_line)) {
            throw service_description_exc(name, "inittab-line setting is too long");
        }
        strncpy(settings.inittab_line, inittab_setting.c_str(), sizeof(settings.inittab_line));
        #endif
    }
    else if (setting == "rlimit-nofile") {
        string nofile_setting = read_setting_value(i, end, nullptr);
        service_rlimits &nofile_limits = find_rlimits(settings.rlimits, RLIMIT_NOFILE);
        parse_rlimit(nofile_setting, name, "rlimit-nofile", nofile_limits);
    }
    else if (setting == "rlimit-core") {
        string core_setting = read_setting_value(i, end, nullptr);
        service_rlimits &nofile_limits = find_rlimits(settings.rlimits, RLIMIT_CORE);
        parse_rlimit(core_setting, name, "rlimit-core", nofile_limits);
    }
    else if (setting == "rlimit-data") {
        string data_setting = read_setting_value(i, end, nullptr);
        service_rlimits &nofile_limits = find_rlimits(settings.rlimits, RLIMIT_DATA);
        parse_rlimit(data_setting, name, "rlimit-data", nofile_limits);
    }
    else if (setting == "rlimit-addrspace") {
        #if defined(RLIMIT_AS)
            string addrspace_setting = read_setting_value(i, end, nullptr);
            service_rlimits &nofile_limits = find_rlimits(settings.rlimits, RLIMIT_AS);
            parse_rlimit(addrspace_setting, name, "rlimit-addrspace", nofile_limits);
        #endif
    }
    else {
        throw service_description_exc(name, "unknown setting: '" + setting + "'.");
    }
}

} // namespace dinit_load

using dinit_load::process_service_file;
