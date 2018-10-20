#include <algorithm>
#include <string>
#include <fstream>
#include <locale>
#include <iostream>
#include <limits>

#include <cstring>
#include <cstdlib>

#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <dirent.h>

#include "proc-service.h"
#include "dinit-log.h"
#include "dinit-util.h"

using string = std::string;
using string_iterator = std::string::iterator;

// Convert a signal name to the corresponding signal number
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
        static_assert((uintmax_t)std::numeric_limits<uid_t>::max()
                <= (uintmax_t)std::numeric_limits<unsigned long long>::max(), "uid_t is too large");
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

static const char * num_err_msg = "Specified value contains invalid numeric characters or is outside "
        "allowed range.";

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

static const char * gid_err_msg = "Specified group id contains invalid numeric characters or is "
        "outside allowed range.";

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

// Perform environment variable substitution on a command line, if specified.
//   line -  the string storing the command and arguments
//   offsets - the [start,end) pair of offsets of the command and each argument within the string
//
static void do_env_subst(std::string &line, std::list<std::pair<unsigned,unsigned>> &offsets,
        bool do_sub_vars)
{
    if (do_sub_vars) {
        auto i = offsets.begin();
        std::string r_line = line.substr(i->first, i->second - i->first); // copy command part
        for (++i; i != offsets.end(); ++i) {
            auto &offset_pair = *i;
            if (line[offset_pair.first] == '$') {
                // Do subsitution for this part:
                auto env_name = line.substr(offset_pair.first + 1,
                        offset_pair.second - offset_pair.first - 1);
                char *env_val = getenv(env_name.c_str());
                if (env_val != nullptr) {
                    auto val_len = strlen(env_val);
                    r_line += " ";
                    offset_pair.first = r_line.length();
                    offset_pair.second = offset_pair.first + val_len;
                    r_line += env_val;
                }
                else {
                    // specified enironment variable not set: treat as an empty string
                    offset_pair.first = r_line.length();
                    offset_pair.second = offset_pair.first;
                }
            }
            else {
                // No subsitution for this part:
                r_line += " ";
                auto new_offs = r_line.length();
                auto len = offset_pair.second - offset_pair.first;
                r_line += line.substr(offset_pair.first, len);
                offset_pair.first = new_offs;
                offset_pair.second = new_offs + len;
            }
        }
        line = std::move(r_line);
    }
}

// Process a dependency directory - filenames contained within correspond to service names which
// are loaded and added as a dependency of the given type. Expected use is with a directory
// containing symbolic links to other service descriptions, but this isn't required.
// Failure to read the directory contents, or to find a service listed within, is not considered
// a fatal error.
static void process_dep_dir(dirload_service_set &sset,
        const char *servicename,
        const string &service_filename,
        std::list<prelim_dep> &deplist, const std::string &depdirpath,
        dependency_type dep_type)
{
    std::string depdir_fname = combine_paths(parent_path(service_filename), depdirpath.c_str());

    DIR *depdir = opendir(depdir_fname.c_str());
    if (depdir == nullptr) {
        log(loglevel_t::WARN, "Could not open dependency directory '", depdir_fname,
                "' for ", servicename, " service.");
        return;
    }

    errno = 0;
    dirent * dent = readdir(depdir);
    while (dent != nullptr) {
        char * name =  dent->d_name;
        if (name[0] != '.') {
            try {
                service_record * sr = sset.load_service(name);
                deplist.emplace_back(sr, dep_type);
            }
            catch (service_not_found &) {
                log(loglevel_t::WARN, "Ignoring unresolved dependency '", name,
                        "' in dependency directory '", depdirpath,
                        "' for ", servicename, " service.");
            }
        }
        dent = readdir(depdir);
    }

    if (errno != 0) {
        log(loglevel_t::WARN, "Error reading dependency directory '", depdirpath,
                "' for ", servicename, " service.");
    }
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
    
    using namespace dinit_load;

    // First try and find an existing record...
    service_record * rval = find_service(string(name));
    if (rval != 0) {
        if (rval->is_dummy()) {
            throw service_cyclic_dependency(name);
        }
        return rval;
    }

    ifstream service_file;
    string service_filename;

    // Couldn't find one. Have to load it.
    for (auto &service_dir : service_dirs) {
        service_filename = service_dir.get_dir();
        if (*(service_filename.rbegin()) != '/') {
            service_filename += '/';
        }
        service_filename += name;

        service_file.open(service_filename.c_str(), ios::in);
        if (service_file) break;
    }
    
    if (! service_file) {
        throw service_not_found(string(name));
    }

    string command;
    list<pair<unsigned,unsigned>> command_offsets;
    string stop_command;
    list<pair<unsigned,unsigned>> stop_command_offsets;
    string working_dir;
    string pid_file;

    bool do_sub_vars = false;

    service_type_t service_type = service_type_t::PROCESS;
    std::list<prelim_dep> depends;
    string logfile;
    service_flags_t onstart_flags;
    int term_signal = -1;  // additional termination signal
    bool auto_restart = false;
    bool smooth_recovery = false;
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
    
    uid_t run_as_uid = -1;
    gid_t run_as_gid = -1;

    string line;
    service_file.exceptions(ios::badbit | ios::failbit);
    
    // Add a dummy service record now to prevent infinite recursion in case of cyclic dependency.
    // We replace this with the real service later (or remove it if we find a configuration error).
    rval = new service_record(this, string(name));
    add_service(rval);
    
    try {
        // getline can set failbit if it reaches end-of-file, we don't want an exception in that case:
        service_file.exceptions(ios::badbit);
        
        process_service_file(name, service_file,
                [&](string &line, string &setting, string_iterator &i, string_iterator &end) -> void {
            if (setting == "command") {
                command = read_setting_value(i, end, &command_offsets);
            }
            else if (setting == "working-dir") {
                working_dir = read_setting_value(i, end, nullptr);
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
                    throw service_description_exc(name, "socket-permissions: Badly-formed or "
                            "out-of-range numeric value");
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
            else if (setting == "waits-for.d") {
                string waitsford = read_setting_value(i, end);
                process_dep_dir(*this, name, service_filename, depends, waitsford,
                        dependency_type::WAITS_FOR);
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
                    string option_txt = onstart_cmds.substr(indexpair.first,
                            indexpair.second - indexpair.first);
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
                        onstart_flags.start_interruptible = true;
                    }
                    else if (option_txt == "skippable") {
                        onstart_flags.skippable = true;
                    }
                    else if (option_txt == "signal-process-only") {
                        onstart_flags.signal_process_only = true;
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
                        do_sub_vars = true;
                    }
                    else if (option_txt == "no-sub-vars") {
                        do_sub_vars = false;
                    }
                    else {
                        throw service_description_exc(name, "Unknown load option: " + option_txt);
                    }
                }
            }
            else if (setting == "termsignal") {
                string signame = read_setting_value(i, end, nullptr);
                int signo = signal_name_to_number(signame);
                if (signo == -1) {
                    throw service_description_exc(name, "Unknown/unsupported termination signal: "
                            + signame);
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
            else if (setting == "run-as") {
                string run_as_str = read_setting_value(i, end, nullptr);
                run_as_uid = parse_uid_param(run_as_str, name, &run_as_gid);
            }
            else {
                throw service_description_exc(name, "Unknown setting: " + setting);
            }
        });

        service_file.close();
        
        if (service_type == service_type_t::PROCESS || service_type == service_type_t::BGPROCESS
                || service_type == service_type_t::SCRIPTED) {
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
                    do_env_subst(command, command_offsets, do_sub_vars);
                    auto rvalps = new process_service(this, string(name), std::move(command),
                            command_offsets, depends);
                    rvalps->set_workding_dir(working_dir);
                    rvalps->set_restart_interval(restart_interval, max_restarts);
                    rvalps->set_restart_delay(restart_delay);
                    rvalps->set_stop_timeout(stop_timeout);
                    rvalps->set_start_timeout(start_timeout);
                    rvalps->set_extra_termination_signal(term_signal);
                    rvalps->set_run_as_uid_gid(run_as_uid, run_as_gid);
                    rvalps->set_workding_dir(working_dir);
                    // process service start / run on console must be the same:
                    onstart_flags.starts_on_console = onstart_flags.runs_on_console;
                    rval = rvalps;
                }
                else if (service_type == service_type_t::BGPROCESS) {
                    do_env_subst(command, command_offsets, do_sub_vars);
                    auto rvalps = new bgproc_service(this, string(name), std::move(command),
                            command_offsets, depends);
                    rvalps->set_workding_dir(working_dir);
                    rvalps->set_pid_file(std::move(pid_file));
                    rvalps->set_restart_interval(restart_interval, max_restarts);
                    rvalps->set_restart_delay(restart_delay);
                    rvalps->set_stop_timeout(stop_timeout);
                    rvalps->set_start_timeout(start_timeout);
                    rvalps->set_extra_termination_signal(term_signal);
                    rvalps->set_run_as_uid_gid(run_as_uid, run_as_gid);
                    onstart_flags.runs_on_console = false;
                    rval = rvalps;
                }
                else if (service_type == service_type_t::SCRIPTED) {
                    do_env_subst(command, command_offsets, do_sub_vars);
                    auto rvalps = new scripted_service(this, string(name), std::move(command),
                            command_offsets, depends);
                    rvalps->set_stop_command(stop_command, stop_command_offsets);
                    rvalps->set_workding_dir(working_dir);
                    rvalps->set_stop_timeout(stop_timeout);
                    rvalps->set_start_timeout(start_timeout);
                    rvalps->set_extra_termination_signal(term_signal);
                    rvalps->set_run_as_uid_gid(run_as_uid, run_as_gid);
                    rval = rvalps;
                }
                else {
                    rval = new service_record(this, string(name), service_type, depends);
                }
                rval->set_log_file(logfile);
                rval->set_auto_restart(auto_restart);
                rval->set_smooth_recovery(smooth_recovery);
                rval->set_flags(onstart_flags);
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
        records.erase(std::find(records.begin(), records.end(), rval));
        delete rval;
        throw service_description_exc(name, std::move(setting_exc.get_info()));
    }
    catch (...) {
        // Must remove the dummy service record.
        records.erase(std::find(records.begin(), records.end(), rval));
        delete rval;
        throw;
    }
}
