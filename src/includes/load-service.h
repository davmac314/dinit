#ifndef LOAD_SERVICE_H_INCLUDED
#define LOAD_SERVICE_H_INCLUDED 1

#include <iostream>
#include <list>
#include <limits>
#include <utility>
#include <vector>
#include <iterator>

#include <csignal>
#include <cstring>
#include <cstdlib>
#include <climits>

#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <grp.h>
#include <pwd.h>

#include <dinit-env.h>
#include <dinit-utmp.h>
#include <dinit-util.h>
#include <file-input-stack.h>
#include <service-constants.h>
#include <mconfig.h>

#if SUPPORT_CAPABILITIES
#include <sys/capability.h>
#endif

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
    bool no_new_privs : 1;      // set PR_SET_NO_NEW_PRIVS

    service_flags_t() noexcept : rw_ready(false), log_ready(false),
            runs_on_console(false), starts_on_console(false), shares_console(false), unmask_intr(false),
            pass_cs_fd(false), start_interruptible(false), skippable(false), signal_process_only(false),
            always_chain(false), kill_all_on_stop(false), no_new_privs(false)
    {
    }
};

#if SUPPORT_CAPABILITIES
struct secure_bits_t
{
    bool keep_caps : 1;
    bool keep_caps_locked : 1;
    bool no_setuid_fixup : 1;
    bool no_setuid_fixup_locked : 1;
    bool noroot : 1;
    bool noroot_locked : 1;

    secure_bits_t() noexcept : keep_caps(false), keep_caps_locked(false),
            no_setuid_fixup(false), no_setuid_fixup_locked(false),
            noroot(false), noroot_locked(false)
    {
    }

    unsigned int get() const noexcept {
        unsigned int r = 0;
        // as referenced in uapi
        if (noroot) r |= 1 << 0;
        if (noroot_locked) r |= 1 << 1;
        if (no_setuid_fixup) r |= 1 << 2;
        if (no_setuid_fixup_locked) r |= 1 << 3;
        if (keep_caps) r |= 1 << 4;
        if (keep_caps_locked) r |= 1 << 5;
        return r;
    }
};
#endif

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

// Can't load service due to system error (eg error reading service file)
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

// Non-owning file position reference
class file_pos_ref
{
    unsigned line_num;
    const std::string &file_name;

    public:
    file_pos_ref(file_input_stack &stack)
        : line_num(stack.current_line()), file_name(stack.current_file_name())
    {
    }

    file_pos_ref(const std::string &file_name, unsigned line_num) : line_num(line_num), file_name(file_name)
    {
    }

    unsigned get_line_num() const
    {
        return line_num;
    }

    const std::string &get_file_name() const
    {
        return file_name;
    }

    // get reference to (mutable) line number
    unsigned &get_line_num_ref()
    {
        return line_num;
    }
};

// File position
class file_pos
{
    unsigned line_num = -1;
    std::string file_name;

public:
    file_pos(const file_pos_ref &fpr)
        : line_num(fpr.get_line_num()), file_name(fpr.get_file_name())
    {}

    file_pos(const std::string &file_name, unsigned line_num = -1)
        : line_num(line_num), file_name(file_name)
    {}

    file_pos() = default;

    unsigned get_line_num() { return line_num; }
    const std::string &get_file_name() { return file_name; }
};

// Error in a service description
// At least one out of line number and setting name will be available. Note that these are
// constructed without supplying a service name, but it will be filled in later.
class service_description_exc : public service_load_exc
{
    public:
    file_pos input_pos;
    const char * const setting_name = nullptr;

    service_description_exc(const file_pos_ref &input_pos_p, std::string &&exc_info)
            : service_load_exc(std::move(exc_info)), input_pos(input_pos_p)
    {
    }

    service_description_exc(const std::string &file_name, unsigned line_num, std::string &&exc_info)
            : service_load_exc(std::move(exc_info)), input_pos(file_name, line_num)
    {
    }

    service_description_exc(const char *setting_name, std::string &&exc_info)
            : service_load_exc(std::move(exc_info)), setting_name(setting_name)
    {
    }

    service_description_exc(const std::string &serviceName, std::string &&exc_info,
            const file_pos_ref &input_pos_p)
        : service_load_exc(serviceName, std::move(exc_info)), input_pos(input_pos_p)
    {
    }

    service_description_exc(const std::string &serviceName, std::string &&exc_info,
            const char *setting_name)
        : service_load_exc(serviceName, std::move(exc_info)), setting_name(setting_name)
    {
    }

    service_description_exc(const std::string &serviceName, std::string &&exc_info,
            const char *setting_name, const file_pos_ref &input_pos_p)
        : service_load_exc(serviceName, std::move(exc_info)), input_pos(input_pos_p),
          setting_name(setting_name)
    {
    }
};

namespace dinit_load {

using string = std::string;
using string_iterator = std::string::iterator;

// Operators in setting lines.
enum class setting_op_t {
    ASSIGN /* = */, COLON /* : */, PLUSASSIGN /* += */
};

// The setting ids
enum class setting_id_t {
    LAST = -1, // used to indicate end of settings
    TYPE, COMMAND, WORKING_DIR, ENV_FILE, SOCKET_LISTEN, SOCKET_PERMISSIONS, SOCKET_UID,
    SOCKET_GID, STOP_COMMAND, PID_FILE, DEPENDS_ON, DEPENDS_MS, WAITS_FOR, WAITS_FOR_D,
    DEPENDS_ON_D, DEPENDS_MS_D, AFTER, BEFORE, LOGFILE, LOGFILE_PERMISSIONS, LOGFILE_UID,
    LOGFILE_GID, LOG_TYPE, LOG_BUFFER_SIZE, CONSUMER_OF, RESTART, SMOOTH_RECOVERY, OPTIONS,
    LOAD_OPTIONS, TERM_SIGNAL, TERMSIGNAL /* deprecated */, RESTART_LIMIT_INTERVAL, RESTART_DELAY,
    RESTART_LIMIT_COUNT, STOP_TIMEOUT, START_TIMEOUT, RUN_AS, CHAIN_TO, READY_NOTIFICATION,
    INITTAB_ID, INITTAB_LINE,
    // Prefixed with SETTING_ to avoid name collision with system macros:
    SETTING_RLIMIT_NOFILE, SETTING_RLIMIT_CORE, SETTING_RLIMIT_DATA, SETTING_RLIMIT_ADDRSPACE,
    // Possibly unsupported depending on platform/build options:
#if SUPPORT_CGROUPS
    RUN_IN_CGROUP,
#endif
#if SUPPORT_CAPABILITIES
    CAPABILITIES,
    SECURE_BITS,
#endif
#ifdef __linux__
    NICE,
    IONICE,
    OOM_SCORE_ADJ,
#endif
};

struct setting_details {
    const char *setting_str; // (may be null for blank entry)
    setting_id_t setting_id;
    bool supp_colon : 1; // supports ':' assignment
    bool supp_assign : 1; // supports '=' assignment
    bool supp_plus_assign : 1; // supports '+=' assignment operator
    // Note: if '=' not supported but ':' is, '=' maps to ':' for backwards compatibility
};

extern setting_details all_settings[];

// skip whitespace and embedded comments.
inline string_iterator skipcomment(string_iterator i, string_iterator end, unsigned & count) noexcept
{
    using std::locale;
    using std::isspace;

    bool comment = false;
    while (i != end) {
        if (*i == '#') {
            comment = true;
        }
        else if (*i == '\n') {
            ++count;
            comment = false;
        }

        if (!comment && !isspace(*i, locale::classic())) break;
        ++i;
    }

    return i;
}

// Utility function to skip white space. Returns an iterator at the
// first non-white-space position (or at end).
inline string_iterator skipws(string_iterator i, string_iterator end) noexcept
{
    using std::locale;
    using std::isspace;

    while (i != end) {
        if (!isspace(*i, locale::classic())) {
            break;
        }
        ++i;
    }
    return i;
}

// skipws using "char *" instead of iterator
inline const char *skipws(const char *i, const char *end) noexcept
{
    using std::locale;
    using std::isspace;

    while (i != end) {
        if (!isspace(*i, locale::classic())) {
            break;
        }
        ++i;
    }
    return i;
}

// skipws, but newlines increment an integer reference.
inline string_iterator skipwsln(string_iterator i, string_iterator end, unsigned & count) noexcept
{
    using std::locale;
    using std::isspace;

    while (i != end) {
        if (*i == '\n') ++count;
        if (!isspace(*i, locale::classic())) {
            break;
        }
        ++i;
    }

    return i;
}

inline const char *findws(const char *i, const char *end) noexcept
{
    using std::locale;
    using std::isspace;

    while (i != end) {
        if (isspace(*i, locale::classic())) {
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
inline string read_config_name(string_iterator & i, string_iterator end, bool env = false, bool *num = nullptr) noexcept
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

    // For environment lookups, integers are valid names (particularly for argument)
    if (env && facet.is(ctype<char>::digit, *i)) {
        while (facet.is(ctype<char>::digit, *i)) {
            rval += *i;
            ++i;
        }
        if (num) {
            *num = true;
        }
        return rval;
    }

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

// Read a setting value, with an assignment or plus-assignment (append).
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
// Parameters:
//    setting_val - the (current) setting value, to be modified by the call
//    operation   - the operation- assign or append
//    input_pos   - the file and line number on which the setting appears (starts), used for error
//                  reporting
//    i           - reference to string iterator through the line (updated to end of setting on
//                  return)
//    end         - iterator at end of line (not including newline character if any)
//    part_positions -  list of pair<unsigned,unsigned> to which the position of each setting value
//                  part will be added as [start,end). May be null.
inline void read_setting_value(std::string &setting_val, setting_op_t operation,
        file_pos_ref input_pos, string_iterator &i, string_iterator end,
        std::list<std::pair<unsigned,unsigned>> *part_positions = nullptr,
        char delimiter = ' ')
{
    using std::locale;
    using std::isspace;

    unsigned &line_num = input_pos.get_line_num_ref();

    i = skipwsln(i, end, line_num);

    if (operation == setting_op_t::PLUSASSIGN) {
        // Ensure that values are correctly delimited. This is usually only for debugging
        // niceness as for commands where this is mostly used the offsets actually delimit
        // the components, but e.g. for capabilities (comma-separated) it matters more.
        if (!setting_val.empty()) {
            setting_val += delimiter;
        }
    }
    else {
        setting_val.clear();
        if (part_positions != nullptr) {
            part_positions->clear();
        }
    }

    bool new_part = true;
    int part_start;

    while (i != end) {
        char c = *i;
        if (c == '\"') {
            if (new_part) {
                part_start = setting_val.length();
                new_part = false;
            }
            // quoted string
            ++i;
            while (i != end) {
                c = *i;
                if (c == '\"') break;
                else if (c == '\\') {
                    // A backslash escapes the following character.
                    c = *(++i);
                    setting_val += c;
                }
                else {
                    setting_val += c;
                }
                ++i;
            }
            if (i == end) {
                // String wasn't terminated
                throw service_description_exc(input_pos, "unterminated quoted string");
            }
        }
        else if (c == '\\') {
            if (new_part) {
                part_start = setting_val.length();
                new_part = false;
            }
            // A backslash escapes the next character
            setting_val += *(++i);
        }
        else if (isspace(c, locale::classic())) {
            if (!new_part && part_positions != nullptr) {
                part_positions->emplace_back(part_start, setting_val.length());
                new_part = true;
            }
            i = skipcomment(i, end, line_num);
            if (i == end) break;
            setting_val += ' ';  // collapse ws to a single space
            continue;
        }
        else if (c == '#') {
            // Possibly intended a comment; we require leading whitespace to reduce occurrence of accidental
            // comments in setting values.
            throw service_description_exc(input_pos, "hashmark (`#') comment must be separated from setting value by whitespace");
        }
        else {
            if (new_part) {
                part_start = setting_val.length();
                new_part = false;
            }
            setting_val += c;
        }
        ++i;
    }

    // Got to end:
    if (!new_part && part_positions != nullptr) {
        part_positions->emplace_back(part_start, setting_val.length());
    }
}

// Read a setting value, with an assignment or plus-assignment (append).
//
// See read_setting_value(std::string &, ...)
inline void read_setting_value(ha_string &setting_val, setting_op_t operation,
        file_pos_ref input_pos, string_iterator &i, string_iterator end,
        std::list<std::pair<unsigned,unsigned>> *part_positions = nullptr, char delimiter = ' ') {
    std::string sval = std::string(setting_val.c_str(), setting_val.length());
    read_setting_value(sval, operation, input_pos, i, end, part_positions, delimiter);
    setting_val = sval;
}

// Read a setting value.
//
// See read_setting_value(std::string &, ...)
inline string read_setting_value(file_pos_ref input_pos, string_iterator &i, string_iterator end,
        std::list<std::pair<unsigned,unsigned>> *part_positions = nullptr)
{
    string rval;
    read_setting_value(rval, setting_op_t::ASSIGN, input_pos, i, end, part_positions);
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
inline uid_t parse_uid_param(file_pos_ref input_pos, const std::string &param,
        const std::string &service_name, const char *setting_name, gid_t *group_p)
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
            throw service_description_exc(service_name, std::string(setting_name) + ": " + uid_err_msg, input_pos);
        }
        return v;
    }
    catch (std::out_of_range &exc) {
        throw service_description_exc(service_name, uid_err_msg, input_pos);
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
                    + "\" does not exist in system database.", input_pos);
        }
        else {
            throw service_description_exc(service_name, std::string("error accessing user database: ")
                    + strerror(errno), input_pos);
        }
    }

    if (group_p) {
        *group_p = pwent->pw_gid;
    }

    return pwent->pw_uid;
}

inline gid_t parse_gid_param(file_pos_ref input_pos, const std::string &param,
        const char *setting_name, const std::string &service_name)
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
            throw service_description_exc(service_name, std::string(setting_name) + ": " + gid_err_msg,
                    input_pos);
        }
        return v;
    }
    catch (std::out_of_range &exc) {
        throw service_description_exc(service_name, std::string(setting_name) + ": " + gid_err_msg,
                input_pos);
    }
    catch (std::invalid_argument &exc) {
        // Ok, so it doesn't look like a number: proceed...
    }

    errno = 0;
    struct group * grent = getgrnam(param.c_str());
    if (grent == nullptr) {
        // Maybe an error, maybe just no entry.
        if (errno == 0) {
            throw service_description_exc(service_name, std::string(setting_name) + ": specified group \""
                    + param + "\" does not exist in system database.", input_pos);
        }
        else {
            throw service_description_exc(service_name, std::string("error accessing group database: ")
                    + strerror(errno), input_pos);
        }
    }

    return grent->gr_gid;
}

// Parse a permissions mask parameter value, specified as an octal (such as 0600)
inline int parse_perms(file_pos_ref input_pos, string &paramval, const std::string &servicename,
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
        throw service_description_exc(servicename, paramname,
                "badly-formed or out-of-range numeric value", input_pos);
    }
}

// Parse a time, specified as a decimal number of seconds (with optional fractional component after decimal
// point or decimal comma).
inline void parse_timespec(file_pos_ref input_pos, const std::string &paramval,
        const std::string &servicename, const char * paramname, timespec &ts)
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
            throw service_description_exc(servicename, std::string("bad value for ") + paramname, input_pos);
        }
        // check for overflow
        if (isec >= max_secs) {
           throw service_description_exc(servicename, std::string("too-large value for ") + paramname, input_pos);
        }
        isec *= 10;
        isec += ch - '0';
    }
    decltype(insec) insec_m = 100000000; // 10^8
    for ( ; i < len; i++) {
        char ch = paramval[i];
        if (ch < '0' || ch > '9') {
            throw service_description_exc(servicename, std::string("bad value for ") + paramname, input_pos);
        }
        insec += (ch - '0') * insec_m;
        insec_m /= 10;
    }
    ts.tv_sec = isec;
    ts.tv_nsec = insec;
}

// Parse an unsigned numeric parameter value
inline unsigned long long parse_unum_param(file_pos_ref input_pos, const std::string &param,
        const std::string &service_name, unsigned long long max = std::numeric_limits<unsigned long long>::max())
{
    const char * num_err_msg = "specified value contains invalid numeric characters or is outside "
            "allowed range.";

    std::size_t ind = 0;
    try {
        unsigned long long v = std::stoull(param, &ind, 0);
        if (v > max || ind != param.length()) {
            throw service_description_exc(service_name, num_err_msg, input_pos);
        }
        return v;
    }
    catch (std::out_of_range &exc) {
        throw service_description_exc(service_name, num_err_msg, input_pos);
    }
    catch (std::invalid_argument &exc) {
        throw service_description_exc(service_name, num_err_msg, input_pos);
    }
}

// Parse a signed numeric parameter value
inline long long parse_snum_param(file_pos_ref input_pos, const std::string &param,
        const std::string &service_name, long long min = std::numeric_limits<long long>::min(),
        long long max = std::numeric_limits<long long>::max())
{
    const char * num_err_msg = "specified value contains invalid numeric characters or is outside "
            "allowed range.";

    std::size_t ind = 0;
    try {
        long long v = std::stoll(param, &ind, 0);
        if (v < min || v > max || ind != (param.length() - (v < 0))) {
            throw service_description_exc(service_name, num_err_msg, input_pos);
        }
        return v;
    }
    catch (std::out_of_range &exc) {
        throw service_description_exc(service_name, num_err_msg, input_pos);
    }
    catch (std::invalid_argument &exc) {
        throw service_description_exc(service_name, num_err_msg, input_pos);
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
inline void parse_rlimit(const std::string &line, file_pos_ref input_pos,
        const std::string &service_name, const char *param_name, service_rlimits &rlimit)
{
    // Examples:
    // 4:5 - soft:hard limits both set
    // 4:-   soft set, hard set to unlimited
    // 4:    soft set, hard limit unchanged
    // 4     soft and hard limit set to same limit

    if (line.empty()) {
        throw service_description_exc(service_name, std::string(param_name) + ": bad value.", input_pos);
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
                throw service_description_exc(service_name, std::string(param_name) + ": bad value.", input_pos);
            }
        }

        index++;
        if (*index == 0) return;

        rlimit.hard_set = true;

        if (*index == '-') {
            rlimit.limits.rlim_max = RLIM_INFINITY;
            if (index[1] != 0) {
                throw service_description_exc(service_name, std::string(param_name) + ": bad value.", input_pos);
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
        throw service_description_exc(service_name, std::string(param_name) + ": bad value.", input_pos);
    }
    catch (std::out_of_range &exc) {
        throw service_description_exc(service_name, std::string(param_name) + ": too-large value.", input_pos);
    }
}

inline string read_include_path(string const &svcname, string const &meta_cmd, file_pos_ref input_pos,
        string_iterator &i, string_iterator end, const char *argval);

// Process an opened service file, line by line.
//    name - the service name
//    service_input - the service file input stream (stack)
//    process_line_func - a function to process settings, of the form:
//             void(string &line, file_pos_ref position, string &setting, string_iterator i,
//                     string_iterator end)
//           Called with:
//               line - the complete line, excluding newline character
//               position - the input position (file_pos_ref)
//               setting - the setting name, from the beginning of the line
//               op - the operation (setting_op_t)
//               i - iterator at the beginning of the setting value
//               end - iterator marking the end of the line
//
// May throw service load exceptions or I/O exceptions if enabled on stream.
template <typename T>
void process_service_file(string name, file_input_stack &service_input, T process_line_func, const char *argval = nullptr)
{
    string line;

    while (service_input.getline(line)) {

        unsigned line_num = service_input.current_line();

        if (line.empty()) continue;
        while (line.back() == '\\') {
            // Check for double-backslash, i.e. backslash escaped with backslash. We actually need
            // to check for any even number.
            bool is_odd_number = true;
            auto r = std::next(line.rbegin());
            auto re = line.rend();
            while (r != re) {
                if (*r != '\\') break;
                is_odd_number = !is_odd_number;
                ++r;
            }

            if (!is_odd_number) break;

            string nextline;
            string::iterator j;
            string::iterator endnext;

            line.back() = '\n';
            if (!service_input.getline_same_file(nextline)) {
                throw service_description_exc(service_input,
                        "end-of-file follows backslash escape character (`\\')");
            }

            j = nextline.begin();
            endnext = nextline.end();
            j = skipws(j, endnext);
            if (j == nextline.begin()) {
                throw service_description_exc(service_input,
                        "line following line-continuation backslash (`\\') "
                        "does not begin with whitespace character");
            }
            line.append(nextline);
        }

        string::iterator i = line.begin();
        string::iterator end = line.end();

        i = skipwsln(i, end, line_num);
        if (i != end) {
            if (*i == '#') continue; // comment without setting

            if (*i == '@') {
                // meta command
                ++i;
                string meta_cmd = read_config_name(i, end);
                bool is_include_opt = (meta_cmd == "include-opt");
                if (is_include_opt || meta_cmd == "include") {
                    // @include-opt or @include
                    file_pos_ref input_pos { service_input.current_file_name(), line_num };
                    std::string include_name = read_include_path(name, meta_cmd, input_pos, i, end, argval);

                    std::ifstream file(include_name);
                    file.exceptions(std::ios::badbit);
                    if (file) {
                        service_input.push(include_name, std::move(file));
                    }
                    else {
                        if (!is_include_opt || errno != ENOENT) {
                            throw service_load_exc(name, include_name + ": cannot open: " + strerror(errno));
                        }
                    }
                }
                else {
                    file_pos_ref input_pos { service_input.current_file_name(), line_num };
                    throw service_description_exc(name, "bad meta command", input_pos);
                }

                continue;
            }

            string setting = read_config_name(i, end);
            i = skipwsln(i, end, line_num);

            setting_op_t setting_op;

            bool bad_line = false;
            if (setting.empty()) {
                bad_line = true;
            }
            else if (*i == '=') {
                setting_op = setting_op_t::ASSIGN;
            }
            else if (*i == ':') {
                setting_op = setting_op_t::COLON;
            }
            else if (*i == '+') {
                ++i;
                if (i != end && *i == '=') {
                    // "+=" is allowed
                    setting_op = setting_op_t::PLUSASSIGN;
                }
                else {
                    bad_line = true;
                }
            }
            else {
                bad_line = true;
            }

            if (bad_line) {
                throw service_description_exc(name, "badly formed line.", service_input);
            }

            i = skipwsln(++i, end, line_num);

            file_pos_ref fpr { service_input.current_file_name(), line_num };
            process_line_func(line, fpr, setting, setting_op, i, end);
        }
    }
}

// A dummy lint-reporting function.
inline void dummy_lint(const char *) {}

// Resolve variables from an environment
inline const char *resolve_env_var(const string &name, const environment::env_map &envmap){
    return envmap.lookup(name);
};

// Substitute variable references in a value with their values. Specified offsets must give
// the location of separate arguments after word splitting and are adjusted appropriately.
// If you simply wish to substitute all variables in the given string, pass an offsets list
// containing one pair with the string's bounds (0, size). '$$' resolves to a single '$'.
//
// throws: service_description_exc if a $-substitution is ill-formed
//         std::length_error if string length limit exceeded or command line is too long
//         std::bad_alloc on allocation failure
template <typename T>
static void value_var_subst(const char *setting_name, std::string &line,
        std::list<std::pair<unsigned,unsigned>> &offsets, T &var_resolve,
        environment::env_map const *envmap, const char *argval)
{
    auto dindx = line.find('$');
    if (dindx == string::npos) {
        return;
    }

    if (line.length() > (size_t)std::numeric_limits<int>::max()) {
        // (avoid potential for overflow later)
        throw std::length_error("string too long");
    }

    auto i = offsets.begin();
    unsigned xpos = 0; // position to copy from in original line
    std::string r_line;
    int offadj = 0;

    while (i != offsets.end()) {

        i->first += offadj; // don't adjust end yet

        // inhibit_collapse is set if we process anything which may be empty but shouldn't collapse
        // to "no argument"
        bool inhibit_collapse = false;
        bool do_collapse = false;

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
                auto spos = dindx + 1;
                bool wsplit = line[spos] == '/';
                if (wsplit) ++spos;
                bool brace = line[spos] == '{';
                if (brace) ++spos;
                auto j = std::next(line.begin(), spos);
                // may be a service argument
                bool is_arg = false;
                // read environment variable name
                string name = read_config_name(j, token_end, true, &is_arg);
                if (name.empty()) {
                    throw service_description_exc(setting_name, "invalid/missing variable name after '$'");
                }
                else if (is_arg && name != "1") {
                    // only one arg is supported
                    throw service_description_exc(setting_name, "only one service argument may be present");
                }
                char altmode = '\0';
                bool colon = false;
                auto altbeg = j, altend = j;
                if (brace) {
                    /* ${foo+val}, ${foo-val}, ${foo:+val}, ${foo:-val} */
                    if (*j == ':') {
                        colon = true;
                        ++j;
                        if (*j != '+' && *j != '-') {
                            throw service_description_exc(setting_name, "invalid syntax in variable substitution");
                        }
                    }
                    if (*j == '+' || *j == '-') {
                        altmode = *j;
                        altbeg = ++j;
                        while (j != token_end && *j != '}') {
                            ++j;
                        }
                        altend = j;
                    }
                    if (*j != '}') {
                        throw service_description_exc(setting_name, "unmatched '{' in variable substitution");
                    }
                    ++j;
                }
                size_t line_len_before = r_line.size();
                string_view resolved_vw;
                auto *resolved = is_arg ? argval : (envmap ? var_resolve(name, *envmap) : nullptr);
                if (resolved) {
                    resolved_vw = resolved;
                }
                /* apply shell-like substitutions */
                if (altmode == '-') {
                    if (!resolved || (colon && !*resolved)) {
                        resolved_vw = {line.c_str() + (altbeg - line.begin()), (size_t)(altend - altbeg)};
                    }
                }
                else if (altmode == '+') {
                    if (resolved && (!colon || *resolved)) {
                        resolved_vw = {line.c_str() + (altbeg - line.begin()), (size_t)(altend - altbeg)};
                    }
                }
                else if (is_arg && !argval) {
                    // $1 and ${1} is special in that it must be set or it is an error
                    // however, we want the more complex syntaxes for conditional substitution
                    throw service_description_exc(setting_name, "missing value in argument substitution");
                }

                xpos = j - line.begin();
                int name_len = xpos - dindx;
                offadj -= name_len;

                if (!wsplit) {
                    inhibit_collapse = true;
                    do_collapse = false;
                    if (!resolved_vw.empty()) {
                        r_line.append(resolved_vw.data(), resolved_vw.length());
                    }
                }
                else {
                    // Must perform word splitting. Find first whitespace:
                    auto r_vw_beg = resolved_vw.data();
                    auto r_vw_end = r_vw_beg + resolved_vw.length();
                    const char *wsp = findws(r_vw_beg, r_vw_end);

                    // If we have whitespace, append up to that whitespace and then split:
                    while (wsp != r_vw_end) {
                        if (wsp != r_vw_beg) {
                            r_line.append(r_vw_beg, wsp - r_vw_beg);
                        }

                        auto orig_i_second = i->second;

                        size_t line_len_after = r_line.size();
                        if (i->first == line_len_after && !inhibit_collapse) {
                            // whitespace at the start of the word; just trim it
                            goto next_section;
                        }

                        // Break here:
                        i->second = r_line.length();

                        r_line += ' ';
                        ++line_len_after;

                        if (line_len_after > (size_t)std::numeric_limits<int>::max()) {
                            // (avoid potential overflow)
                            throw service_description_exc(setting_name, "value too long (after substitution)");
                        }

                        // Create new argument from split:
                        i = offsets.insert(std::next(i), {r_line.length(), orig_i_second});
                        offadj += (int)line_len_after - (int)line_len_before;
                        line_len_before = r_line.size();

                        // Find the next break, if any:
                        next_section:
                        r_vw_beg = skipws(wsp, r_vw_end);
                        wsp = findws(r_vw_beg, r_vw_end);
                        inhibit_collapse = false;
                    }

                    if (r_vw_beg != r_vw_end) {
                        r_line.append(r_vw_beg, r_vw_end - r_vw_beg);
                    }

                    do_collapse = !inhibit_collapse;
                }

                size_t line_len_after = r_line.size();

                if (line_len_after > (size_t)std::numeric_limits<int>::max()) {
                    // (avoid potential overflow)
                    throw service_description_exc(setting_name, "value too long (after substitution)");
                }

                offadj += (int)line_len_after - (int)line_len_before;
            }

            dindx = line.find('$', xpos);
        }

        i->second += offadj;

        if (do_collapse && i->first == i->second) {
            i = offsets.erase(i);
        }
        else {
            ++i;
        }

        while (i != offsets.end() && i->second < dindx) {
            i->first += offadj;
            i->second += offadj;
            ++i;
        }
    }

    r_line.append(line, xpos); // copy final unmatched part
    line = std::move(r_line);
}

// Reads a value while performing minimal argument expansion in it.
inline string read_value_with_arg(const char *setting_name, file_pos_ref input_pos, string_iterator &i,
        string_iterator end, const char *argval)
{
    string rval;
    read_setting_value(rval, setting_op_t::ASSIGN, input_pos, i, end, nullptr);

    std::list<std::pair<unsigned,unsigned>> offsets;
    offsets.emplace_back(0, rval.size());
    value_var_subst(setting_name, rval, offsets, resolve_env_var, nullptr, argval);
    return rval;
}

// Reads an include path while performing minimal argument expansion in it.
inline string read_include_path(string const &svcname, string const &meta_cmd, file_pos_ref input_pos,
        string_iterator &i, string_iterator end, const char *argval)
{
    string rval;
    std::list<std::pair<unsigned,unsigned>> parts;

    read_setting_value(rval, setting_op_t::ASSIGN, input_pos, i, end, &parts);
    if (parts.size() != 1) {
        throw service_description_exc(svcname, "'@" + meta_cmd + "' requires a single argument", input_pos);
    }

    std::list<std::pair<unsigned,unsigned>> offsets;
    offsets.emplace_back(0, rval.size());
    value_var_subst(meta_cmd.c_str(), rval, offsets, resolve_env_var, nullptr, argval);
    return rval;
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
    auto_restart_mode auto_restart = auto_restart_mode::DEFAULT_AUTO_RESTART;
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

    #if SUPPORT_CAPABILITIES
    string capabilities;
    secure_bits_t secbits;
    #endif

    #ifdef __linux__
    int nice = INT_MIN;
    int ionice = INT_MIN;
    int oom_adj = INT_MIN;
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
    //
    // Template parameters:
    //     propagate_sde - whether to propagate service description errors (if false they are reported via report_err)
    //     (remaining template parameters should be inferred)
    // Parameters:
    //     report_error - functor to report any errors
    //     envmap - environment variables
    //     report_line - functor to report lint (default: don't report)
    //     var_subst - functor to resolve environment variable values
    template <bool propagate_sde = false, typename T, typename U = decltype(dummy_lint), typename V = decltype(resolve_env_var),
            bool do_report_lint = !std::is_same<U, decltype(dummy_lint)>::value>
    void finalise(T &report_error, environment::env_map const &envmap, const char *argval, U &report_lint = dummy_lint, V &var_subst = resolve_env_var)
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
            #if SUPPORT_CAPABILITIES
            if (!capabilities.empty()) {
                report_lint("'capabilities' specified, but ignored for the specified (or default) service type.");
            }
            if (secbits.get()) {
                report_lint("'secure-bits' specified, but ignored for the specified (or default) service type.");
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
            #if SUPPORT_CAPABILITIES
            if (onstart_flags.no_new_privs) {
                report_lint("option 'no_new_privs' was specified, but ignored for the specified (or default) service type.");
            }
            #endif
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
            auto do_resolve = [&](const char *setting_name, string &setting_value) {
                try {
                    list<pair<unsigned,unsigned>> str_offsets;
                    str_offsets.emplace_back(0, setting_value.size());
                    value_var_subst(setting_name, setting_value, str_offsets, var_subst, &envmap, argval);
                }
                catch (service_description_exc &exc) {
                    if (propagate_sde) throw;
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
//     process_dep_dir_t : type of process_dep_dir function/lambda (see below)
//
// parameters:
//     settings     : wrapper object for service settings
//     name         : name of the service being processed
//     line         : the current line of the service description file
//     input_pos    : the current input position (for error reporting)
//     setting      : the name of the setting (from the beginning of line)
//     setting_op   : the operator specified after the setting name
//     i            : iterator at beginning of setting value (including whitespace)
//     end          : iterator at end of line
//     load_service : function to load a service
//                      arguments:  const char *service_name
//                      return: a value that can be used (with a dependency type) to construct a
//                              dependency in the 'depends' vector within the 'settings' object
//     process_dep_dir : function to process a dependency directory
//                      arguments: decltype(settings.depends) &dependencies
//                                 const string &waitsford - directory as specified in parameter
//                                 dependency_type dep_type - type of dependency to add
// throws: service_description_exc, std::bad_alloc, std::length_error (string too long; unlikely)
template <typename settings_wrapper,
    typename load_service_t,
    typename process_dep_dir_t>
void process_service_line(settings_wrapper &settings, const char *name, const char *arg, string &line,
        file_pos_ref input_pos, string &setting, setting_op_t setting_op, string::iterator &i,
        string::iterator &end, load_service_t load_service,
        process_dep_dir_t process_dep_dir)
{
    // find the setting:
    setting_details *details = all_settings;
    while (setting != details->setting_str) {
        details++;
        if (details->setting_id == setting_id_t::LAST) {
            throw service_description_exc(name, "unknown setting: '" + setting + "'.", input_pos);
        }
    }

    if (setting_op == setting_op_t::PLUSASSIGN && !details->supp_plus_assign) {
        throw service_description_exc(name, "cannot use '+=' with setting '" + setting + "'", input_pos);
    }

    switch (details->setting_id) {
        case setting_id_t::COMMAND:
            read_setting_value(settings.command, setting_op, input_pos, i, end, &settings.command_offsets);
            break;
        case setting_id_t::WORKING_DIR:
            settings.working_dir = read_setting_value(input_pos, i, end, nullptr);
            break;
        case setting_id_t::ENV_FILE:
            settings.env_file = read_value_with_arg(setting.c_str(), input_pos, i, end, arg);
            break;
        #if SUPPORT_CGROUPS
        case setting_id_t::RUN_IN_CGROUP:
            settings.run_in_cgroup = read_setting_value(input_pos, i, end, nullptr);
            break;
        #endif
        #if SUPPORT_CAPABILITIES
        case setting_id_t::CAPABILITIES:
            read_setting_value(settings.capabilities, setting_op, input_pos, i, end, nullptr, ',');
            break;
        case setting_id_t::SECURE_BITS:
        {
            std::list<std::pair<unsigned,unsigned>> indices;
            string onstart_cmds = read_setting_value(input_pos, i, end, &indices);
            for (auto indexpair : indices) {
                string secbit_txt = onstart_cmds.substr(indexpair.first,
                        indexpair.second - indexpair.first);
                if (secbit_txt == "keep-caps") {
                    settings.secbits.keep_caps = true;
                }
                else if (secbit_txt == "keep-caps-locked") {
                    settings.secbits.keep_caps_locked = true;
                }
                else if (secbit_txt == "no-setuid-fixup") {
                    settings.secbits.no_setuid_fixup = true;
                }
                else if (secbit_txt == "no-setuid-fixup-locked") {
                    settings.secbits.no_setuid_fixup_locked = true;
                }
                else if (secbit_txt == "noroot") {
                    settings.secbits.noroot = true;
                }
                else if (secbit_txt == "noroot-locked") {
                    settings.secbits.noroot_locked = true;
                }
                else {
                    throw service_description_exc(name, "unknown secure bit: " + secbit_txt,
                            "secure-bits", input_pos);
                }
            }
            break;
        }
        #endif
        #ifdef __linux__
        case setting_id_t::NICE:
        {
            string nice_str = read_setting_value(input_pos, i, end);
            settings.nice = (int)parse_snum_param(input_pos, nice_str, name, -20, 19);
            break;
        }
        case setting_id_t::IONICE:
        {
            string ionice_str = read_setting_value(input_pos, i, end);
            if (ionice_str == "none") {
                settings.ionice = 0;
            }
            else if (starts_with(ionice_str, "rt:")) {
                auto nval = parse_unum_param(input_pos, ionice_str.substr(3 /* len 'rt:' */), name, 7);
                settings.ionice = (1 << 13) | nval;
            }
            else if (starts_with(ionice_str, "be:")) {
                auto nval = parse_unum_param(input_pos, ionice_str.substr(3 /* len 'be:' */), name, 7);
                settings.ionice = (2 << 13) | nval;
            }
            else if (ionice_str == "idle") {
                settings.ionice = 3 << 13;
            }
            else {
                    throw service_description_exc(name, "invalid value for ionice: " + ionice_str,
                            name, input_pos);
            }
            break;
        }
        case setting_id_t::OOM_SCORE_ADJ:
        {
            string oom_adj_str = read_setting_value(input_pos, i, end);
            settings.oom_adj = (int)parse_snum_param(input_pos, oom_adj_str, name, -1000, 1000);
            break;
        }
        #endif
        case setting_id_t::SOCKET_LISTEN:
            settings.socket_path = read_setting_value(input_pos, i, end, nullptr);
            break;
        case setting_id_t::SOCKET_PERMISSIONS:
        {
            string sock_perm_str = read_setting_value(input_pos, i, end, nullptr);
            settings.socket_perms = parse_perms(input_pos, sock_perm_str, name, "socket-permissions");
            break;
        }
        case setting_id_t::SOCKET_UID:
        {
            string sock_uid_s = read_setting_value(input_pos, i, end, nullptr);
            settings.socket_uid = parse_uid_param(input_pos, sock_uid_s, name, "socket-uid",
                    &settings.socket_uid_gid);
            break;
        }
        case setting_id_t::SOCKET_GID:
        {
            string sock_gid_s = read_setting_value(input_pos, i, end, nullptr);
            settings.socket_gid = parse_gid_param(input_pos, sock_gid_s, "socket-gid", name);
            break;
        }
        case setting_id_t::STOP_COMMAND:
            read_setting_value(settings.stop_command, setting_op, input_pos, i, end,
                    &settings.stop_command_offsets);
            break;
        case setting_id_t::PID_FILE:
            settings.pid_file = read_setting_value(input_pos, i, end);
            break;
        case setting_id_t::DEPENDS_ON:
        {
            string dependency_name = read_value_with_arg(setting.c_str(), input_pos, i, end, arg);
            settings.depends.emplace_back(load_service(dependency_name.c_str()),
                    dependency_type::REGULAR);
            break;
        }
        case setting_id_t::DEPENDS_MS:
        {
            string dependency_name = read_value_with_arg(setting.c_str(), input_pos, i, end, arg);
            settings.depends.emplace_back(load_service(dependency_name.c_str()),
                    dependency_type::MILESTONE);
            break;
        }
        case setting_id_t::WAITS_FOR:
        {
            string dependency_name = read_value_with_arg(setting.c_str(), input_pos, i, end, arg);
            settings.depends.emplace_back(load_service(dependency_name.c_str()),
                    dependency_type::WAITS_FOR);
            break;
        }
        case setting_id_t::WAITS_FOR_D:
        {
            string waitsford = read_setting_value(input_pos, i, end);
            process_dep_dir(settings.depends, waitsford, dependency_type::WAITS_FOR);
            break;
        }
        case setting_id_t::DEPENDS_ON_D:
        {
            string depends_on_d = read_setting_value(input_pos, i, end);
            process_dep_dir(settings.depends, depends_on_d, dependency_type::REGULAR);
            break;
        }
        case setting_id_t::DEPENDS_MS_D:
        {
            string depends_ms_d = read_setting_value(input_pos, i, end);
            process_dep_dir(settings.depends, depends_ms_d, dependency_type::MILESTONE);
            break;
        }
        case setting_id_t::AFTER:
        {
            string after_name = read_value_with_arg(setting.c_str(), input_pos, i, end, arg);
            settings.after_svcs.emplace_back(std::move(after_name));
            break;
        }
        case setting_id_t::BEFORE:
        {
            string before_name = read_value_with_arg(setting.c_str(), input_pos, i, end, arg);
            settings.before_svcs.emplace_back(std::move(before_name));
            break;
        }
        case setting_id_t::LOGFILE:
        {
            settings.logfile = read_setting_value(input_pos, i, end);
            if (!settings.logfile.empty() && settings.log_type == log_type_id::NONE) {
                settings.log_type = log_type_id::LOGFILE;
            }
            break;
        }
        case setting_id_t::LOGFILE_PERMISSIONS:
        {
            string log_perm_str = read_setting_value(input_pos, i, end, nullptr);
            settings.logfile_perms = parse_perms(input_pos, log_perm_str, name, "logfile-permissions");
            break;
        }
        case setting_id_t::LOGFILE_UID:
        {
            string log_uid_s = read_setting_value(input_pos, i, end, nullptr);
            settings.logfile_uid = parse_uid_param(input_pos, log_uid_s, name, "logfile-uid",
                    &settings.logfile_uid_gid);
            break;
        }
        case setting_id_t::LOGFILE_GID:
        {
            string log_gid_s = read_setting_value(input_pos, i, end, nullptr);
            settings.logfile_gid = parse_gid_param(input_pos, log_gid_s, name, "logfile-gid");
            break;
        }
        case setting_id_t::LOG_TYPE:
        {
            string log_type_str = read_setting_value(input_pos, i, end);
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
                throw service_description_exc(name, "log type must be one of: \"file\", \"buffer\", \"pipe\","
                        " or \"none\"", "log-type", input_pos);
            }
            break;
        }
        case setting_id_t::LOG_BUFFER_SIZE:
        {
            string log_buffer_size_str = read_setting_value(input_pos, i, end);
            unsigned bufsize = (unsigned)parse_unum_param(input_pos, log_buffer_size_str, name,
                    std::numeric_limits<unsigned>::max() / 2);
            settings.max_log_buffer_sz = bufsize;
            break;
        }
        case setting_id_t::CONSUMER_OF:
        {
            string consumed_svc_name = read_setting_value(input_pos, i, end);
            if (consumed_svc_name == name) {
                throw service_description_exc(name, "service cannot be its own consumer", "consumer-of",
                        input_pos);
            }
            settings.consumer_of_name = consumed_svc_name;
            break;
        }
        case setting_id_t::RESTART:
        {
            string restart = read_setting_value(input_pos, i, end);
            if (restart == "yes" || restart == "true") {
                settings.auto_restart = auto_restart_mode::ALWAYS;
            }
            else if (restart == "on-failure") {
                settings.auto_restart = auto_restart_mode::ON_FAILURE;
            }
            else if (restart == "no" || restart == "false") {
                settings.auto_restart = auto_restart_mode::NEVER;
            }
            else {
                throw service_description_exc(name, "restart must be one of: \"yes\", \"true\","
                        " \"no\", \"false\" or \"on-failure\"", "restart", input_pos);
            }
            break;
        }
        case setting_id_t::SMOOTH_RECOVERY:
        {
            string recovery = read_setting_value(input_pos, i, end);
            if (recovery == "yes" || recovery == "true") {
                settings.smooth_recovery = true;
            }
            else if (recovery == "no" || recovery == "false") {
                settings.smooth_recovery = false;
            }
            else {
                throw service_description_exc(name, "smooth-recovery must be one of: \"yes\","
                        " \"true\", \"no\" or \"false\"", "smooth-recovery", input_pos);
            }
            break;
        }
        case setting_id_t::TYPE:
        {
            string type_str = read_setting_value(input_pos, i, end);
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
                    " \"process\", \"bgprocess\", \"internal\" or \"triggered\"",
                    "type", input_pos);
            }
            break;
        }
        case setting_id_t::OPTIONS:
        {
            std::list<std::pair<unsigned,unsigned>> indices;
            string onstart_cmds = read_setting_value(input_pos, i, end, &indices);
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
#if SUPPORT_CAPABILITIES
                else if (option_txt == "no-new-privs") {
                    settings.onstart_flags.no_new_privs = true;
                }
#endif
                else {
                    throw service_description_exc(name, "unknown option: " + option_txt,
                            "options", input_pos);
                }
            }
            break;
        }
        case setting_id_t::LOAD_OPTIONS:
        {
            std::list<std::pair<unsigned,unsigned>> indices;
            string load_opts = read_setting_value(input_pos, i, end, &indices);
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
                    throw service_description_exc(name, "unknown load option: " + option_txt,
                            "load-options", input_pos);
                }
            }
            break;
        }
        case setting_id_t::TERM_SIGNAL:
        case setting_id_t::TERMSIGNAL:
        {
            // Note: "termsignal" supported for legacy reasons.
            string signame = read_setting_value(input_pos, i, end, nullptr);
            int signo = signal_name_to_number(signame);
            if (signo == -1) {
                throw service_description_exc(name, "unknown/unsupported termination signal: "
                        + signame, details->setting_str, input_pos);
            }
            else {
                settings.term_signal = signo;
            }
            break;
        }
        case setting_id_t::RESTART_LIMIT_INTERVAL:
        {
            string interval_str = read_setting_value(input_pos, i, end, nullptr);
            parse_timespec(input_pos, interval_str, name, "restart-limit-interval", settings.restart_interval);
            break;
        }
        case setting_id_t::RESTART_DELAY:
        {
            string rsdelay_str = read_setting_value(input_pos, i, end, nullptr);
            parse_timespec(input_pos, rsdelay_str, name, "restart-delay", settings.restart_delay);
            break;
        }
        case setting_id_t::RESTART_LIMIT_COUNT: {
            string limit_str = read_setting_value(input_pos, i, end, nullptr);
            settings.max_restarts = parse_unum_param(input_pos, limit_str, name, std::numeric_limits<int>::max());
            break;
        }
        case setting_id_t::STOP_TIMEOUT:
        {
            string stoptimeout_str = read_setting_value(input_pos, i, end, nullptr);
            parse_timespec(input_pos, stoptimeout_str, name, "stop-timeout", settings.stop_timeout);
            break;
        }
        case setting_id_t::START_TIMEOUT:
        {
            string starttimeout_str = read_setting_value(input_pos, i, end, nullptr);
            parse_timespec(input_pos, starttimeout_str, name, "start-timeout", settings.start_timeout);
            break;
        }
        case setting_id_t::RUN_AS:
        {
            string run_as_str = read_setting_value(input_pos, i, end, nullptr);
            settings.run_as_uid = parse_uid_param(input_pos, run_as_str, name, "run-as", &settings.run_as_uid_gid);
            break;
        }
        case setting_id_t::CHAIN_TO:
            settings.chain_to_name = read_setting_value(input_pos, i, end, nullptr);
            break;
        case setting_id_t::READY_NOTIFICATION:
        {
            string notify_setting = read_setting_value(input_pos, i, end, nullptr);
            if (starts_with(notify_setting, "pipefd:")) {
                settings.readiness_fd = parse_unum_param(input_pos, notify_setting.substr(7 /* len 'pipefd:' */),
                        name, std::numeric_limits<int>::max());
            }
            else if (starts_with(notify_setting, "pipevar:")) {
                settings.readiness_var = notify_setting.substr(8 /* len 'pipevar:' */);
                if (settings.readiness_var.empty()) {
                    throw service_description_exc(name, "invalid pipevar variable name",
                            "ready-notification", input_pos);
                }
            }
            else {
                throw service_description_exc(name, "unrecognised setting: " + notify_setting,
                        "ready-notification", input_pos);
            }
            break;
        }
        case setting_id_t::INITTAB_ID:
        {
            string inittab_setting = read_setting_value(input_pos, i, end, nullptr);
            #if USE_UTMPX
                if (inittab_setting.length() > sizeof(settings.inittab_id)) {
                    throw service_description_exc(name, "inittab-id setting is too long", input_pos);
                }
                strncpy(settings.inittab_id, inittab_setting.c_str(), sizeof(settings.inittab_id));
            #endif
            break;
        }
        case setting_id_t::INITTAB_LINE:
        {
            string inittab_setting = read_setting_value(input_pos, i, end, nullptr);
            #if USE_UTMPX
                if (inittab_setting.length() > sizeof(settings.inittab_line)) {
                    throw service_description_exc(name, "inittab-line setting is too long", input_pos);
                }
                strncpy(settings.inittab_line, inittab_setting.c_str(), sizeof(settings.inittab_line));
            #endif
            break;
        }
        case setting_id_t::SETTING_RLIMIT_NOFILE:
        {
            string nofile_setting = read_setting_value(input_pos, i, end, nullptr);
            service_rlimits &nofile_limits = find_rlimits(settings.rlimits, RLIMIT_NOFILE);
            parse_rlimit(nofile_setting, input_pos, name, "rlimit-nofile", nofile_limits);
            break;
        }
        case setting_id_t::SETTING_RLIMIT_CORE:
        {
            string core_setting = read_setting_value(input_pos, i, end, nullptr);
            service_rlimits &core_limits = find_rlimits(settings.rlimits, RLIMIT_CORE);
            parse_rlimit(core_setting, input_pos, name, "rlimit-core", core_limits);
            break;
        }
        case setting_id_t::SETTING_RLIMIT_DATA:
        {
            string data_setting = read_setting_value(input_pos, i, end, nullptr);
            service_rlimits &data_limits = find_rlimits(settings.rlimits, RLIMIT_DATA);
            parse_rlimit(data_setting, input_pos, name, "rlimit-data", data_limits);
            break;
        }
        case setting_id_t::SETTING_RLIMIT_ADDRSPACE:
        {
            #if defined(RLIMIT_AS)
                string addrspace_setting = read_setting_value(input_pos, i, end, nullptr);
                service_rlimits &as_limits = find_rlimits(settings.rlimits, RLIMIT_AS);
                parse_rlimit(addrspace_setting, input_pos, name, "rlimit-addrspace", as_limits);
            #endif
            break;
        }
        case setting_id_t::LAST:
#ifdef __GNUC__
            __builtin_unreachable();
#else
            break;
#endif
    }
}

} // namespace dinit_load

using dinit_load::process_service_file;

#endif /* LOAD_SERVICE_H_INCLUDED */
