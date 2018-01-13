#include <algorithm>
#include <string>
#include <fstream>
#include <locale>
#include <iostream>
#include <limits>

#include <cstring>

#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>

#include "proc-service.h"

using string = std::string;
using string_iterator = std::string::iterator;

// Utility function to skip white space. Returns an iterator at the
// first non-white-space position (or at end).
static string_iterator skipws(string_iterator i, string_iterator end)
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

// Read a setting name.
static string read_setting_name(string_iterator & i, string_iterator end)
{
    using std::locale;
    using std::ctype;
    using std::use_facet;
    
    const ctype<char> & facet = use_facet<ctype<char> >(locale::classic());

    string rval;
    // Allow alphabetical characters, and dash (-) in setting name
    while (i != end && (*i == '-' || facet.is(ctype<char>::alpha, *i))) {
        rval += *i;
        ++i;
    }
    return rval;
}

namespace {
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
}


// Read a setting value
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
// This function expects the string to be in an ASCII-compatible, single byte
// encoding (the "classic" locale).
//
// Params:
//    service_name - the name of the service to which the setting applies
//    i  -  reference to string iterator through the line
//    end -   iterator at end of line
//    part_positions -  list of <int,int> to which the position of each setting value
//                      part will be added as [start,end). May be null.
static string read_setting_value(string_iterator & i, string_iterator end,
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
                if (c == '\n') {
                    throw setting_exception("Line end inside quoted string");
                }
                else if (c == '\\') {
                    // A backslash escapes the following character.
                    ++i;
                    if (i != end) {
                        c = *i;
                        if (c == '\n') {
                            throw setting_exception("Line end follows backslash escape character (`\\')");
                        }
                        rval += c;
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

static int signal_name_to_number(std::string &signame)
{
    if (signame == "HUP") return SIGHUP;
    if (signame == "INT") return SIGINT;
    if (signame == "QUIT") return SIGQUIT;
    if (signame == "USR1") return SIGUSR1;
    if (signame == "USR2") return SIGUSR2;
    return -1;
}

static const char * uid_err_msg = "Specified user id contains invalid numeric characters or is outside allowed range.";

// Parse a userid parameter which may be a numeric user ID or a username. If a name, the
// userid is looked up via the system user database (getpwnam() function). In this case,
// the associated group is stored in the location specified by the group_p parameter iff
// it is not null and iff it contains the value -1.
static uid_t parse_uid_param(const std::string &param, const std::string &service_name, gid_t *group_p)
{
    // Could be a name or a numeric id. But we should assume numeric first, just in case
    // a user manages to give themselves a username that parses as a number.
    std::size_t ind = 0;
    try {
        // POSIX does not specify whether uid_t is an signed or unsigned, but regardless
        // is is probably safe to assume that valid values are positive. We'll also assert
        // that the value range fits within "unsigned long long" since it seems unlikely
        // that would ever not be the case.
        static_assert((uintmax_t)std::numeric_limits<uid_t>::max() <= (uintmax_t)std::numeric_limits<unsigned long long>::max(), "uid_t is too large");
        unsigned long long v = std::stoull(param, &ind, 0);
        if (v > static_cast<unsigned long long>(std::numeric_limits<uid_t>::max()) || ind != param.length()) {
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
            throw service_description_exc(service_name, "Specified user \"" + param + "\" does not exist in system database.");
        }
        else {
            throw service_description_exc(service_name, std::string("Error accessing user database: ") + strerror(errno));
        }
    }
    
    if (group_p && *group_p != (gid_t)-1) {
        *group_p = pwent->pw_gid;
    }
    
    return pwent->pw_uid;
}

static const char * num_err_msg = "Specified value contains invalid numeric characters or is outside allowed range.";

// Parse an unsigned numeric parameter value
static unsigned long long parse_unum_param(const std::string &param, const std::string &service_name,
        unsigned long long max = std::numeric_limits<unsigned long long>::max())
{
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

static const char * gid_err_msg = "Specified group id contains invalid numeric characters or is outside allowed range.";

static gid_t parse_gid_param(const std::string &param, const std::string &service_name)
{
    // Could be a name or a numeric id. But we should assume numeric first, just in case
    // a user manages to give themselves a username that parses as a number.
    std::size_t ind = 0;
    try {
        // POSIX does not specify whether uid_t is an signed or unsigned, but regardless
        // is is probably safe to assume that valid values are positive. We'll also assume
        // that the value range fits with "unsigned long long" since it seems unlikely
        // that would ever not be the case.
        static_assert((uintmax_t)std::numeric_limits<gid_t>::max() <= (uintmax_t)std::numeric_limits<unsigned long long>::max(), "gid_t is too large");
        unsigned long long v = std::stoull(param, &ind, 0);
        if (v > static_cast<unsigned long long>(std::numeric_limits<gid_t>::max()) || ind != param.length()) {
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
            throw service_description_exc(service_name, "Specified group \"" + param + "\" does not exist in system database.");
        }
        else {
            throw service_description_exc(service_name, std::string("Error accessing group database: ") + strerror(errno));
        }
    }
    
    return grent->gr_gid;
}

// Parse a time, specified as a decimal number of seconds (with optional fractional component after decimal
// point or decimal comma).
//
static void parse_timespec(const std::string &paramval, const std::string &servicename,
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

// Find a service record, or load it from file. If the service has
// dependencies, load those also.
//
// Might throw a ServiceLoadExc exception if a dependency cycle is found or if another
// problem occurs (I/O error, service description not found etc). Throws std::bad_alloc
// if a memory allocation failure occurs.
//
service_record * dirload_service_set::load_service(const char * name)
{
    using std::string;
    using std::ifstream;
    using std::ios;
    using std::ios_base;
    using std::locale;
    using std::isspace;
    
    using std::list;
    using std::pair;
    
    // First try and find an existing record...
    service_record * rval = find_service(string(name));
    if (rval != 0) {
        if (rval->isDummy()) {
            throw service_cyclic_dependency(name);
        }
        return rval;
    }

    // Couldn't find one. Have to load it.    
    string service_filename = service_dir;
    if (*(service_filename.rbegin()) != '/') {
        service_filename += '/';
    }
    service_filename += name;
    
    string command;
    list<pair<unsigned,unsigned>> command_offsets;
    string stop_command;
    list<pair<unsigned,unsigned>> stop_command_offsets;
    string pid_file;

    service_type_t service_type = service_type_t::PROCESS;
    std::list<prelim_dep> depends;
    string logfile;
    onstart_flags_t onstart_flags;
    int term_signal = -1;  // additional termination signal
    bool auto_restart = false;
    bool smooth_recovery = false;
    bool start_is_interruptible = false;
    string socket_path;
    int socket_perms = 0666;
    // Note: Posix allows that uid_t and gid_t may be unsigned types, but eg chown uses -1 as an
    // invalid value, so it's safe to assume that we can do the same:
    uid_t socket_uid = -1;
    gid_t socket_gid = -1;
    // Restart limit interval / count; default is 10 seconds, 3 restarts:
    timespec restart_interval = { .tv_sec = 10, .tv_nsec = 0 };
    int max_restarts = 3;
    timespec restart_delay = { .tv_sec = 0, .tv_nsec = 200000000 };
    timespec stop_timeout = { .tv_sec = 10, .tv_nsec = 0 };
    timespec start_timeout = { .tv_sec = 60, .tv_nsec = 0 };
    
    string line;
    ifstream service_file;
    service_file.exceptions(ios::badbit | ios::failbit);
    
    try {
        service_file.open(service_filename.c_str(), ios::in);
    }
    catch (std::ios_base::failure &exc) {
        throw service_not_found(name);
    }
    
    // Add a dummy service record now to prevent infinite recursion in case of cyclic dependency.
    // We replace this with the real service later (or remove it if we find a configuration error).
    rval = new service_record(this, string(name));
    add_service(rval);
    
    try {
        // getline can set failbit if it reaches end-of-file, we don't want an exception in that case:
        service_file.exceptions(ios::badbit);
        
        while (! (service_file.rdstate() & ios::eofbit)) {
            getline(service_file, line);
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
                
                if (setting == "command") {
                    command = read_setting_value(i, end, &command_offsets);
                }
                else if (setting == "socket-listen") {
                    socket_path = read_setting_value(i, end, nullptr);
                }
                else if (setting == "socket-permissions") {
                    string sock_perm_str = read_setting_value(i, end, nullptr);
                    std::size_t ind = 0;
                    try {
                        socket_perms = std::stoi(sock_perm_str, &ind, 8);
                        if (ind != sock_perm_str.length()) {
                            throw std::logic_error("");
                        }
                    }
                    catch (std::logic_error &exc) {
                        throw service_description_exc(name, "socket-permissions: Badly-formed or out-of-range numeric value");
                    }
                }
                else if (setting == "socket-uid") {
                    string sock_uid_s = read_setting_value(i, end, nullptr);
                    socket_uid = parse_uid_param(sock_uid_s, name, &socket_gid);
                }
                else if (setting == "socket-gid") {
                    string sock_gid_s = read_setting_value(i, end, nullptr);
                    socket_gid = parse_gid_param(sock_gid_s, name);
                }
                else if (setting == "stop-command") {
                    stop_command = read_setting_value(i, end, &stop_command_offsets);
                }
                else if (setting == "pid-file") {
                    pid_file = read_setting_value(i, end);
                }
                else if (setting == "depends-on") {
                    string dependency_name = read_setting_value(i, end);
                    depends.emplace_back(load_service(dependency_name.c_str()), dependency_type::REGULAR);
                }
                else if (setting == "depends-ms") {
                    string dependency_name = read_setting_value(i, end);
                    depends.emplace_back(load_service(dependency_name.c_str()), dependency_type::MILESTONE);
                }
                else if (setting == "waits-for") {
                    string dependency_name = read_setting_value(i, end);
                    depends.emplace_back(load_service(dependency_name.c_str()), dependency_type::WAITS_FOR);
                }
                else if (setting == "logfile") {
                    logfile = read_setting_value(i, end);
                }
                else if (setting == "restart") {
                    string restart = read_setting_value(i, end);
                    auto_restart = (restart == "yes" || restart == "true");
                }
                else if (setting == "smooth-recovery") {
                    string recovery = read_setting_value(i, end);
                    smooth_recovery = (recovery == "yes" || recovery == "true");
                }
                else if (setting == "type") {
                    string type_str = read_setting_value(i, end);
                    if (type_str == "scripted") {
                        service_type = service_type_t::SCRIPTED;
                    }
                    else if (type_str == "process") {
                        service_type = service_type_t::PROCESS;
                    }
                    else if (type_str == "bgprocess") {
                        service_type = service_type_t::BGPROCESS;
                    }
                    else if (type_str == "internal") {
                        service_type = service_type_t::INTERNAL;
                    }
                    else {
                        throw service_description_exc(name, "Service type must be one of: \"scripted\","
                            " \"process\", \"bgprocess\" or \"internal\"");
                    }
                }
                else if (setting == "options") {
                    std::list<std::pair<unsigned,unsigned>> indices;
                    string onstart_cmds = read_setting_value(i, end, &indices);
                    for (auto indexpair : indices) {
                        string option_txt = onstart_cmds.substr(indexpair.first, indexpair.second - indexpair.first);
                        if (option_txt == "starts-rwfs") {
                            onstart_flags.rw_ready = true;
                        }
                        else if (option_txt == "starts-log") {
                            onstart_flags.log_ready = true;
                        }
                        else if (option_txt == "no-sigterm") {
                            onstart_flags.no_sigterm = true;
                        }
                        else if (option_txt == "runs-on-console") {
                            onstart_flags.runs_on_console = true;
                            // A service that runs on the console necessarily starts on console:
                            onstart_flags.starts_on_console = true;
                        }
                        else if (option_txt == "starts-on-console") {
                            onstart_flags.starts_on_console = true;
                        }
                        else if (option_txt == "pass-cs-fd") {
                            onstart_flags.pass_cs_fd = true;
                        }
                        else if (option_txt == "start-interruptible") {
                            start_is_interruptible = true;
                        }
                        else {
                            throw service_description_exc(name, "Unknown option: " + option_txt);
                        }
                    }
                }
                else if (setting == "termsignal") {
                    string signame = read_setting_value(i, end, nullptr);
                    int signo = signal_name_to_number(signame);
                    if (signo == -1) {
                        throw service_description_exc(name, "Unknown/unsupported termination signal: " + signame);
                    }
                    else {
                        term_signal = signo;
                    }
                }
                else if (setting == "restart-limit-interval") {
                    string interval_str = read_setting_value(i, end, nullptr);
                    parse_timespec(interval_str, name, "restart-limit-interval", restart_interval);
                }
                else if (setting == "restart-delay") {
                    string rsdelay_str = read_setting_value(i, end, nullptr);
                    parse_timespec(rsdelay_str, name, "restart-delay", restart_delay);
                }
                else if (setting == "restart-limit-count") {
                    string limit_str = read_setting_value(i, end, nullptr);
                    max_restarts = parse_unum_param(limit_str, name, std::numeric_limits<int>::max());
                }
                else if (setting == "stop-timeout") {
                    string stoptimeout_str = read_setting_value(i, end, nullptr);
                    parse_timespec(stoptimeout_str, name, "stop-timeout", stop_timeout);
                }
                else if (setting == "start-timeout") {
                    string starttimeout_str = read_setting_value(i, end, nullptr);
                    parse_timespec(starttimeout_str, name, "start-timeout", start_timeout);
                }
                else {
                    throw service_description_exc(name, "Unknown setting: " + setting);
                }
            }
        }
        
        service_file.close();
        
        if (service_type == service_type_t::PROCESS || service_type == service_type_t::BGPROCESS || service_type == service_type_t::SCRIPTED) {
            if (command.length() == 0) {
                throw service_description_exc(name, "Service command not specified");
            }
        }
        
        // Now replace the dummy service record with a real record:
        for (auto iter = records.begin(); iter != records.end(); iter++) {
            if (*iter == rval) {
                // We've found the dummy record
                delete rval;
                if (service_type == service_type_t::PROCESS) {
                    auto rvalps = new process_service(this, string(name), std::move(command),
                            command_offsets, depends);
                    rvalps->set_restart_interval(restart_interval, max_restarts);
                    rvalps->set_restart_delay(restart_delay);
                    rvalps->set_stop_timeout(stop_timeout);
                    rvalps->set_start_timeout(start_timeout);
                    rvalps->set_start_interruptible(start_is_interruptible);
                    rval = rvalps;
                }
                else if (service_type == service_type_t::BGPROCESS) {
                    auto rvalps = new bgproc_service(this, string(name), std::move(command),
                            command_offsets, depends);
                    rvalps->set_pid_file(std::move(pid_file));
                    rvalps->set_restart_interval(restart_interval, max_restarts);
                    rvalps->set_restart_delay(restart_delay);
                    rvalps->set_stop_timeout(stop_timeout);
                    rvalps->set_start_timeout(start_timeout);
                    rvalps->set_start_interruptible(start_is_interruptible);
                    rval = rvalps;
                }
                else if (service_type == service_type_t::SCRIPTED) {
                    auto rvalps = new scripted_service(this, string(name), std::move(command),
                            command_offsets, depends);
                    rvalps->set_stop_command(stop_command, stop_command_offsets);
                    rvalps->set_stop_timeout(stop_timeout);
                    rvalps->set_start_timeout(start_timeout);
                    rvalps->set_start_interruptible(start_is_interruptible);
                    rval = rvalps;
                }
                else {
                    rval = new service_record(this, string(name), service_type, depends);
                }
                rval->set_log_file(logfile);
                rval->set_auto_restart(auto_restart);
                rval->set_smooth_recovery(smooth_recovery);
                rval->set_flags(onstart_flags);
                rval->set_extra_termination_signal(term_signal);
                rval->set_socket_details(std::move(socket_path), socket_perms, socket_uid, socket_gid);
                *iter = rval;
                break;
            }
        }
        
        return rval;
    }
    catch (setting_exception &setting_exc)
    {
        // Must remove the dummy service record.
        std::remove(records.begin(), records.end(), rval);
        delete rval;
        throw service_description_exc(name, std::move(setting_exc.get_info()));
    }
    catch (...) {
        // Must remove the dummy service record.
        std::remove(records.begin(), records.end(), rval);
        delete rval;
        throw;
    }
}
