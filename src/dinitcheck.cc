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
#include <sys/stat.h>
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

static void report_service_description_err(const std::string &service_name, const std::string &what);

// prelim_dep: A preliminary (unresolved) service dependency
class prelim_dep
{
    public:
    std::string name;
    dependency_type dep_type;

    prelim_dep(const std::string &name_p, dependency_type dep_type_p)
        : name(name_p), dep_type(dep_type_p) { }
    prelim_dep(std::string &&name_p, dependency_type dep_type_p)
        : name(std::move(name_p)), dep_type(dep_type_p) { }
};

class service_record
{
public:
    service_record(const std::string &name_p, const std::string &chain_to_p,
            std::list<prelim_dep> dependencies_p, std::list<string> before_svcs)
                : name(name_p), dependencies(dependencies_p), before_svcs(before_svcs) {}

    std::string name;
    std::string chain_to;
    std::list<prelim_dep> dependencies;
    std::list<string> before_svcs;

    bool visited = false;  // flag used to detect cyclic dependencies
    bool cycle_free = false;
};

using service_set_t = std::map<std::string, service_record *>;

service_record *load_service(service_set_t &services, const std::string &name,
        const service_dir_pathlist &service_dirs);

// Add some missing standard library functionality...
template <typename T> bool contains(std::vector<T> vec, const T& elem)
{
    return std::find(vec.begin(), vec.end(), elem) != vec.end();
}

static bool errors_found = false;

int main(int argc, char **argv)
{
    using namespace std;

    service_dir_opt service_dir_opts;
    bool am_system_init = (getuid() == 0);

    std::vector<std::string> services_to_check;

    // Process command line
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == '-') {
                // An option...
                if (strcmp(argv[i], "--services-dir") == 0 || strcmp(argv[i], "-d") == 0) {
                    if (++i < argc) {
                        service_dir_opts.set_specified_service_dir(argv[i]);
                    }
                    else {
                        cerr << "dinitcheck: '--services-dir' (-d) requires an argument" << endl;
                        return 1;
                    }
                }
                else if (strcmp(argv[i], "--help") == 0) {
                    cout << "dinitcheck: check dinit service descriptions\n"
                            " --help                       display help\n"
                            " --services-dir <dir>, -d <dir>\n"
                            "                              set base directory for service description\n"
                            "                              files, can be specified multiple times\n"
                            " <service-name>               check service with name <service-name>\n";
                    return EXIT_SUCCESS;
                }
                else {
                    std::cerr << "dinitcheck: Unrecognized option: '" << argv[i] << "' (use '--help' for help)\n";
                    return EXIT_FAILURE;
                }
            }
            else {
                services_to_check.push_back(argv[i]);
            }
        }
    }

    service_dir_opts.build_paths(am_system_init);

    if (services_to_check.empty()) {
        services_to_check.push_back("boot");
    }

    size_t num_services_to_check = services_to_check.size();

    // Load named service(s)
    // - load the service, store dependencies as strings
    // - recurse

    std::map<std::string, service_record *> service_set;

    for (size_t i = 0; i < services_to_check.size(); ++i) {
        const std::string &name = services_to_check[i];
        std::cout << "Checking service: " << name << "...\n";
        try {
            service_record *sr = load_service(service_set, name, service_dir_opts.get_paths());
            service_set[name] = sr;
            // add dependencies to services_to_check
            for (auto &dep : sr->dependencies) {
                if (service_set.count(dep.name) == 0 && !contains(services_to_check, dep.name)) {
                    services_to_check.push_back(dep.name);
                }
            }
            // add chain_to to services_to_check
            if (!sr->chain_to.empty() && !contains(services_to_check, sr->chain_to)) {
                if (!contains(services_to_check, sr->chain_to)) {
                    services_to_check.push_back(sr->chain_to);
                }
            }
            // add before_svcs to services_to_check
            for (const std::string &before_name : sr->before_svcs) {
                if (!contains(services_to_check, before_name)) {
                    services_to_check.push_back(before_name);
                }
            }
        }
        catch (service_load_exc &exc) {
            std::cerr << "Unable to load service '" << name << "': " << exc.exc_description << "\n";
            errors_found = true;
        }
    }

    // For "before" reverse-dependencies, set up dependencies in the forwards direction (from the dependent)
    for (const auto &svc_name_record : service_set) {
        for (const std::string &before_name : svc_name_record.second->before_svcs) {
            auto before_svc_it = service_set.find(before_name);
            if (before_svc_it != service_set.end()) {
                before_svc_it->second->dependencies.emplace_back(svc_name_record.first,
                        dependency_type::BEFORE);
            }
        }
    }

    // Check for circular dependencies
    std::vector<std::tuple<service_record *, size_t>> service_chain;

    for (size_t i = 0; i < num_services_to_check; ++i) {
        service_record *root = service_set[services_to_check[i]];
        if (! root) continue;
        if (root->visited) continue;

        // invariant: service_chain is empty
        service_chain.emplace_back(root, 0);

        // Depth first traversal. If we find a link (dependency) on a service already visited (but not
        // marked as cycle-free), we know then that we've found a cycle.
        while (true) {
            auto n = service_chain.size() - 1;
            auto &last = service_chain[n];
            service_record *last_record = std::get<0>(last);
            size_t &index = std::get<1>(last);
            if (index >= last_record->dependencies.size()) {
                // Processed all dependencies, go back up:
                last_record->cycle_free = true;
                service_chain.pop_back();
                if (n == 0) break;
                size_t &prev_index = std::get<1>(service_chain[n - 1]);
                ++prev_index;
                continue;
            }
            // Down the tree:
            auto dep_it = std::next(last_record->dependencies.begin(), index);
            service_record *next_link = service_set[dep_it->name];
            if (next_link == nullptr) {
                ++index;
                continue;
            }
            if (next_link->visited) {
                if (! next_link->cycle_free) {
                    // We've found a cycle. Clear entries before the beginning of the cycle, then
                    // exit the loop.
                    auto first = std::find_if(service_chain.begin(), service_chain.end(),
                            [next_link](std::tuple<service_record *, size_t> &a) -> bool {
                       return std::get<0>(a) == next_link;
                    });
                    service_chain.erase(service_chain.begin(), first);
                    break;
                }
            }
            next_link->visited = true;
            service_chain.emplace_back(next_link, 0);
        }

        // Report only one cycle; otherwise difficult to avoid reporting duplicates or overlapping
        // cycles.
        if (!service_chain.empty()) break;
    }

    if (!service_chain.empty()) {
        errors_found = true;
        std::cerr << "Found dependency cycle:\n";
        for (auto chain_link : service_chain) {
            service_record *svc = std::get<0>(chain_link);
            size_t dep_index = std::get<1>(chain_link);
            std::cerr << "    " << svc->name << " ->";
            auto dep_it = std::next(svc->dependencies.begin(), dep_index);
            if (dep_it->dep_type == dependency_type::BEFORE) {
                std::cerr << " (via 'before')";
            }
            std::cerr << "\n";
        }
        std::cerr << "    " << std::get<0>(service_chain[0])->name << ".\n";
    }

    if (! errors_found) {
        std::cout << "No problems found.\n";
    }
    else {
        std::cout << "One or more errors/warnings issued.\n";
    }

    return errors_found ? EXIT_FAILURE : EXIT_SUCCESS;
}

static void report_service_description_err(const std::string &service_name, unsigned line_num,
        const std::string &what)
{
    std::cerr << "Service '" << service_name << "' (line " << line_num << "): " << what << "\n";
    errors_found = true;
}

static void report_service_description_err(const std::string &service_name, const char *setting_name,
        const std::string &what)
{
    std::cerr << "Service '" << service_name << "' setting '" << setting_name << "': " << what << "\n";
    errors_found = true;
}

static void report_service_description_err(const std::string &service_name, const std::string &what)
{
    std::cerr << "Service '" << service_name << "': " << what << "\n";
    errors_found = true;
}

static void report_service_description_exc(service_description_exc &exc)
{
    if (exc.line_num != (unsigned)-1) {
        report_service_description_err(exc.service_name, exc.line_num, exc.exc_description);
    }
    else {
        report_service_description_err(exc.service_name, exc.setting_name, exc.exc_description);
    }
}

static void report_error(std::system_error &exc, const std::string &service_name)
{
    std::cerr << "Service '" << service_name << "', error reading service description: " << exc.what() << "\n";
    errors_found = true;
}

static void report_dir_error(const char *service_name, const std::string &dirpath)
{
    std::cerr << "Service '" << service_name << "', error reading dependencies from directory " << dirpath
            << ": " << strerror(errno) << "\n";
    errors_found = true;
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
        const service_dir_pathlist &service_dirs)
{
    using namespace std;
    using namespace dinit_load;

    auto found = services.find(name);
    if (found != services.end()) {
        return found->second;
    }

    string service_filename;
    ifstream service_file;

    int fail_load_errno = 0;
    std::string fail_load_path;

    // Couldn't find one. Have to load it.
    for (auto &service_dir : service_dirs) {
        service_filename = service_dir.get_dir();
        if (*(service_filename.rbegin()) != '/') {
            service_filename += '/';
        }
        service_filename += name;

        service_file.open(service_filename.c_str(), ios::in);
        if (service_file) break;

        if (errno != ENOENT && fail_load_errno == 0) {
            fail_load_errno = errno;
            fail_load_path = std::move(service_filename);
        }
    }

    if (!service_file) {
        if (fail_load_errno == 0) {
            throw service_not_found(string(name));
        }
        else {
            throw service_load_error(name, std::move(fail_load_path), fail_load_errno);
        }
    }

    service_settings_wrapper<prelim_dep> settings;

    environment menv{};
    environment renv{};

    string line;
    service_file.exceptions(ios::badbit);

    try {
        process_service_file(name, service_file,
                [&](string &line, unsigned line_num, string &setting,
                        string_iterator &i, string_iterator &end) -> void {

            auto process_dep_dir_n = [&](std::list<prelim_dep> &deplist, const std::string &waitsford,
                    dependency_type dep_type) -> void {
                process_dep_dir(name.c_str(), service_filename, deplist, waitsford, dep_type);
            };

            auto load_service_n = [&](const string &dep_name) -> const string & {
                return dep_name;
            };

            try {
                process_service_line(settings, name.c_str(), line, line_num, setting, i, end,
                        load_service_n, process_dep_dir_n);
            }
            catch (service_description_exc &exc) {
                if (exc.service_name.empty()) {
                    exc.service_name = name;
                }
                report_service_description_exc(exc);
            }
        });
    }
    catch (std::system_error &sys_err)
    {
        report_error(sys_err, name);
        throw service_load_exc(name, "error while reading service description.");
    }

    auto report_err = [&](const char *msg) {
        report_service_description_err(name, msg);
    };

    bool issued_var_subst_warning = false;

    environment::env_map renvmap = renv.build(menv);

    auto resolve_var = [&](const string &name, environment::env_map const &envmap) {
        if (!issued_var_subst_warning) {
            report_service_description_err(name, "warning: variable substitution performed by dinitcheck "
                    "for file paths may not match dinitd (environment may differ)");
            issued_var_subst_warning = true;
        }
        return resolve_env_var(name, envmap);
    };

    settings.finalise(report_err, renvmap, report_err, resolve_var);

    auto check_command = [&](const char *setting_name, const char *command) {
        struct stat command_stat;
        if (stat(command, &command_stat) == -1) {
            report_service_description_err(name,
                    std::string("could not stat ") + setting_name + " executable '" + command
                    + "': " + strerror(errno));
        }
        else {
            if ((command_stat.st_mode & S_IFMT) != S_IFREG) {
                report_service_description_err(name, std::string(setting_name) + " executable '"
                        + command + "' is not a regular file.");
            }
            else if ((command_stat.st_mode & S_IXUSR) == 0) {
                report_service_description_err(name, std::string(setting_name) + " executable '" + command
                        + "' is not executable by owner.");
            }
        }
    };

    if (!settings.command.empty()) {
        int offset_start = settings.command_offsets.front().first;
        int offset_end = settings.command_offsets.front().second;
        check_command("command", settings.command.substr(offset_start, offset_end - offset_start).c_str());
    }
    if (!settings.stop_command.empty()) {
        int offset_start = settings.stop_command_offsets.front().first;
        int offset_end = settings.stop_command_offsets.front().second;
        check_command("stop command",
                settings.stop_command.substr(offset_start, offset_end - offset_start).c_str());
    }

    return new service_record(name, settings.chain_to_name, settings.depends, settings.before_svcs);
}
