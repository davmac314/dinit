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
    service_record(std::string name, std::list<prelim_dep> dependencies_p) : dependencies(dependencies_p) {}

    std::string name;
    bool finished_loading = false;  // flag used to detect cyclic dependencies
    std::list<prelim_dep> dependencies;
};

using service_set_t = std::map<std::string, service_record *>;

service_record *load_service(service_set_t &services, const std::string &name,
        const service_dir_pathlist &service_dirs);

// Add some missing standard library functionality...
template <typename T> bool contains(std::vector<T> vec, const T& elem)
{
    return std::find(vec.begin(), vec.end(), elem) != vec.end();
}

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
                }
                // TODO handle other options, err if unrecognized
            }
        }
    }

    service_dir_opts.build_paths(am_system_init);

    // Temporary, for testing:
    services_to_check.push_back("boot");

    // Load named service(s)
    std::map<std::string, service_record *> service_set;

    // - load the service, store dependencies as strings
    // - recurse

    // TODO additional: check chain-to, other lint

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
        }
        catch (service_load_exc &exc) {
            std::cerr << "Unable to load service '" << name << "': " << exc.exc_description << "\n";
        }
    }

    // TODO check for circular dependencies

    return 0;
}

static void report_service_description_exc(service_description_exc &exc)
{
    std::cerr << "Service '" << exc.service_name << "': " << exc.exc_description << "\n";
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

// TODO: this is pretty much copy-paste from load_service.cc. Need to factor out common structure.
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

    service_settings_wrapper<prelim_dep> settings;

    string line;
    service_file.exceptions(ios::badbit);

    try {
        process_service_file(name, service_file,
                [&](string &line, string &setting, string_iterator &i, string_iterator &end) -> void {

            auto process_dep_dir_n = [&](std::list<prelim_dep> &deplist, const std::string &waitsford,
                    dependency_type dep_type) -> void {
                process_dep_dir(name.c_str(), service_filename, deplist, waitsford, dep_type);
            };

            auto load_service_n = [&](const string &dep_name) -> const string & {
                return dep_name;
            };

            try {
                process_service_line(settings, name.c_str(), line, setting, i, end, load_service_n, process_dep_dir_n);
            }
            catch (service_description_exc &exc) {
                report_service_description_exc(exc);
            }
        });
    }
    catch (std::system_error &sys_err)
    {
        report_error(sys_err, name);
        return nullptr;
    }

    return new service_record(name, settings.depends);
}
