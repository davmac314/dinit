#include <algorithm>
#include <iostream>
#include <fstream>
#include <cstring>
#include <string>
#include <vector>
#include <list>
#include <map>

#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pwd.h>
#include <dirent.h>

#include "dinit-util.h"
#include "service-constants.h"
#include "load-service.h"
#include "options-processing.h"

// dinitcheck:  utility to check Dinit configuration for correctness/lint

using string = std::string;
using string_iterator = std::string::iterator;

static const char *user_home_path = nullptr;

// Get user home (and set user_home_path). (The return may become invalid after
// changing the environment (HOME variable) or using the getpwuid() function).
static const char * get_user_home()
{
    if (user_home_path == nullptr) {
        user_home_path = getenv("HOME");
        if (user_home_path == nullptr) {
            struct passwd * pwuid_p = getpwuid(getuid());
            if (pwuid_p != nullptr) {
                user_home_path = pwuid_p->pw_dir;
            }
        }
    }
    return user_home_path;
}

class prelim_dep
{
    public:
    std::string name;
    dependency_type dep_type;

    prelim_dep(std::string &name_p, dependency_type dep_type_p)
        : name(name_p), dep_type(dep_type_p) { }
    prelim_dep(std::string &&name_p, dependency_type dep_type_p)
        : name(std::move(name_p)), dep_type(dep_type_p) { }
};

class service_record
{
public:
    service_record(std::string name, std::list<prelim_dep> dependencies_p) : dependencies(dependencies_p) {}

    std::string name;
    bool finished_loading = false;  // flag used to detect cyclic dependencies
    std::list<prelim_dep> dependencies;
};

using service_set_t = std::map<std::string, service_record *>;

service_record *load_service(service_set_t &services, const std::string &name,
        const std::vector<dir_entry> &service_dirs);

// Add some missing standard library functionality...
template <typename T> bool contains(std::vector<T> vec, const T& elem)
{
    return std::find(vec.begin(), vec.end(), elem) != vec.end();
}

int main(int argc, char **argv)
{
    using namespace std;

    bool add_all_service_dirs = false;
    bool for_system = false;
    const char * service_dir = nullptr;
    bool service_dir_dynamic = false; // service_dir dynamically allocated?

    std::vector<std::string> services_to_check;

    // Figure out service dirs
    /* service directory name */
    if (service_dir == nullptr && ! for_system) {
        const char * userhome = get_user_home();
        if (userhome != nullptr) {
            const char * user_home = get_user_home();
            size_t user_home_len = strlen(user_home);
            size_t dinit_d_len = strlen("/dinit.d");
            size_t full_len = user_home_len + dinit_d_len + 1;
            char *service_dir_w = new char[full_len];
            std::memcpy(service_dir_w, user_home, user_home_len);
            std::memcpy(service_dir_w + user_home_len, "/dinit.d", dinit_d_len);
            service_dir_w[full_len - 1] = 0;

            service_dir = service_dir_w;
            service_dir_dynamic = true;
        }
    }

    if (service_dir == nullptr) {
        service_dir = "/etc/dinit.d";
        add_all_service_dirs = true;
    }

    std::vector<dir_entry> service_dirs;

    service_dirs.emplace_back(service_dir, service_dir_dynamic);
    if (add_all_service_dirs) {
        service_dirs.emplace_back("/usr/local/lib/dinit.d", false);
        service_dirs.emplace_back("/lib/dinit.d", false);
    }

    // Temporary, for testing:
    services_to_check.push_back("boot");

    // Load named service(s)
    std::map<std::string, service_record *> service_set;

    // - load the service, store dependencies as strings
    // - recurse

    // additional: check chain-to, other lint

    for (const auto &name : services_to_check) {
        try {
            service_record *sr = load_service(service_set, name, service_dirs);
            service_set[name] = sr;
            // add dependencies to services_to_check
            for (auto &dep : sr->dependencies) {
                if (service_set.count(dep.name) == 0 && !contains(services_to_check, dep.name)) {
                    services_to_check.push_back(dep.name);
                }
            }
        }
        catch (service_load_exc &exc) {
            std::cerr << "Unable to load service '" << name << "': " << exc.exc_description << "\n";
        }
    }

    // check for circular dependencies

    return 0;
}

static void report_error(dinit_load::setting_exception &exc, const std::string &service_name, const char *setting_name)
{
    std::cerr << "Service '" << service_name << "', " << setting_name << ": " << exc.get_info() << "\n";
}

static void report_error(std::system_error &exc, const std::string &service_name)
{
    std::cerr << "Service '" << service_name << "', error reading service description: " << exc.what() << "\n";
}

static void report_dir_error(const char *service_name, const std::string &dirpath)
{
    std::cerr << "Service '" << service_name << "', error reading dependencies from directory " << dirpath
            << ": " << strerror(errno) << "\n";
}

// Process a dependency directory - filenames contained within correspond to service names which
// are loaded and added as a dependency of the given type. Expected use is with a directory
// containing symbolic links to other service descriptions, but this isn't required.
// Failure to read the directory contents, or to find a service listed within, is not considered
// a fatal error.
static void process_dep_dir(const char *servicename,
        const string &service_filename,
        std::list<prelim_dep> &deplist, const std::string &depdirpath,
        dependency_type dep_type)
{
    std::string depdir_fname = combine_paths(parent_path(service_filename), depdirpath.c_str());

    DIR *depdir = opendir(depdir_fname.c_str());
    if (depdir == nullptr) {
        report_dir_error(servicename, depdirpath);
        return;
    }

    errno = 0;
    dirent * dent = readdir(depdir);
    while (dent != nullptr) {
        char * name =  dent->d_name;
        if (name[0] != '.') {
            deplist.emplace_back(name, dep_type);
        }
        dent = readdir(depdir);
    }

    if (errno != 0) {
        report_dir_error(servicename, depdirpath);
    }

    closedir(depdir);
}

service_record *load_service(service_set_t &services, const std::string &name,
        const std::vector<dir_entry> &service_dirs)
{
    using namespace std;
    using namespace dinit_load;

    auto found = services.find(name);
    if (found != services.end()) {
        return found->second;
    }

    string service_filename;
    ifstream service_file;

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
    string env_file;

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
    std::vector<service_rlimits> rlimits;

    int readiness_fd = -1;      // readiness fd in service process
    std::string readiness_var;  // environment var to hold readiness fd

    uid_t run_as_uid = -1;
    gid_t run_as_gid = -1;

    string chain_to_name;

    #if USE_UTMPX
    char inittab_id[sizeof(utmpx().ut_id)] = {0};
    char inittab_line[sizeof(utmpx().ut_line)] = {0};
    #endif

    string line;
    service_file.exceptions(ios::badbit);

    try {
        process_service_file(name, service_file,
                [&](string &line, string &setting, string_iterator &i, string_iterator &end) -> void {
            try {
                if (setting == "command") {
                    command = read_setting_value(i, end, &command_offsets);
                }
                else if (setting == "working-dir") {
                    working_dir = read_setting_value(i, end, nullptr);
                }
                else if (setting == "env-file") {
                    env_file = read_setting_value(i, end, nullptr);
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
                    depends.emplace_back(std::move(dependency_name), dependency_type::REGULAR);
                }
                else if (setting == "depends-ms") {
                    string dependency_name = read_setting_value(i, end);
                    depends.emplace_back(dependency_name, dependency_type::MILESTONE);
                }
                else if (setting == "waits-for") {
                    string dependency_name = read_setting_value(i, end);
                    depends.emplace_back(dependency_name, dependency_type::WAITS_FOR);
                }
                else if (setting == "waits-for.d") {
                    string waitsford = read_setting_value(i, end);
                    process_dep_dir(name.c_str(), service_filename, depends, waitsford,
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
                            onstart_flags.shares_console = false;
                        }
                        else if (option_txt == "starts-on-console") {
                            onstart_flags.starts_on_console = true;
                            onstart_flags.shares_console = false;
                        }
                        else if (option_txt == "shares-console") {
                            onstart_flags.shares_console = true;
                            onstart_flags.runs_on_console = false;
                            onstart_flags.starts_on_console = false;
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
                else if (setting == "term-signal" || setting == "termsignal") {
                    // Note: "termsignal" supported for legacy reasons.
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
                else if (setting == "chain-to") {
                    chain_to_name = read_setting_value(i, end, nullptr);
                }
                else if (setting == "ready-notification") {
                    string notify_setting = read_setting_value(i, end, nullptr);
                    if (starts_with(notify_setting, "pipefd:")) {
                        readiness_fd = parse_unum_param(notify_setting.substr(7 /* len 'pipefd:' */),
                                name, std::numeric_limits<int>::max());
                    }
                    else if (starts_with(notify_setting, "pipevar:")) {
                        readiness_var = notify_setting.substr(8 /* len 'pipevar:' */);
                        if (readiness_var.empty()) {
                            throw service_description_exc(name, "Invalid pipevar variable name "
                                    "in ready-notification");
                        }
                    }
                    else {
                        throw service_description_exc(name, "Unknown ready-notification setting: "
                                + notify_setting);
                    }
                }
                else if (setting == "inittab-id") {
                    string inittab_setting = read_setting_value(i, end, nullptr);
                    #if USE_UTMPX
                    if (inittab_setting.length() > sizeof(inittab_id)) {
                        throw service_description_exc(name, "inittab-id setting is too long");
                    }
                    strncpy(inittab_id, inittab_setting.c_str(), sizeof(inittab_id));
                    #endif
                }
                else if (setting == "inittab-line") {
                    string inittab_setting = read_setting_value(i, end, nullptr);
                    #if USE_UTMPX
                    if (inittab_setting.length() > sizeof(inittab_line)) {
                        throw service_description_exc(name, "inittab-line setting is too long");
                    }
                    strncpy(inittab_line, inittab_setting.c_str(), sizeof(inittab_line));
                    #endif
                }
                else if (setting == "rlimit-nofile") {
                    string nofile_setting = read_setting_value(i, end, nullptr);
                    service_rlimits &nofile_limits = find_rlimits(rlimits, RLIMIT_NOFILE);
                    parse_rlimit(line, name, "rlimit-nofile", nofile_limits);
                }
                else if (setting == "rlimit-core") {
                    string nofile_setting = read_setting_value(i, end, nullptr);
                    service_rlimits &nofile_limits = find_rlimits(rlimits, RLIMIT_CORE);
                    parse_rlimit(line, name, "rlimit-core", nofile_limits);
                }
                else if (setting == "rlimit-data") {
                    string nofile_setting = read_setting_value(i, end, nullptr);
                    service_rlimits &nofile_limits = find_rlimits(rlimits, RLIMIT_DATA);
                    parse_rlimit(line, name, "rlimit-data", nofile_limits);
                }
                else if (setting == "rlimit-addrspace") {
                    #if defined(RLIMIT_AS)
                        string nofile_setting = read_setting_value(i, end, nullptr);
                        service_rlimits &nofile_limits = find_rlimits(rlimits, RLIMIT_AS);
                        parse_rlimit(line, name, "rlimit-addrspace", nofile_limits);
                    #endif
                }
                else {
                    throw service_description_exc(name, "Unknown setting: " + setting);
                }
            }
            catch (setting_exception &exc) {
                report_error(exc, name, setting.c_str());
            }
        });
    }
    catch (std::system_error &sys_err)
    {
        report_error(sys_err, name);
        return nullptr;
    }

    return new service_record(name, depends);
}
