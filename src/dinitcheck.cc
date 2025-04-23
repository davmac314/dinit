#include <algorithm>
#include <iostream>
#include <fstream>
#include <cstring>
#include <string>
#include <vector>
#include <list>
#include <map>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <unistd.h>
#include <pwd.h>
#include <dirent.h>

#include "dinit-util.h"
#include "dinit-client.h"
#include "service-constants.h"
#include "load-service.h"
#include "options-processing.h"

// dinitcheck:  utility to check Dinit configuration for correctness/lint

using string = std::string;
using string_iterator = std::string::iterator;

static constexpr uint16_t min_cp_version = 1;
static constexpr uint16_t max_cp_version = 5;

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
    service_record(const std::string &name_p, service_type_t service_type, const std::string &chain_to_p,
            std::list<prelim_dep> dependencies_p, const std::list<string> &before_svcs,
            const std::list<string> &after_svcs, std::string consumer_of_name, log_type_id log_type)
                : name(name_p), service_type(service_type), dependencies(dependencies_p),
                  before_svcs(before_svcs), after_svcs(after_svcs), consumer_of_name(consumer_of_name),
                  log_type(log_type)
    {
        // (constructor)
    }

    std::string name;
    service_type_t service_type;
    std::string chain_to;
    std::list<prelim_dep> dependencies;
    std::list<string> before_svcs;
    std::list<string> after_svcs;
    std::string consumer_of_name;
    log_type_id log_type;

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
static bool offline_operation = true;

// environment - populated from running dinit instance if possible
environment menv;

// Get the environment from remote dinit instance
static void get_remote_env(int csfd, cpbuffer_t &rbuffer)
{
    char buf[2] = { (char)cp_cmd::GETALLENV, 0 };
    write_all_x(csfd, buf, 2);

    wait_for_reply(rbuffer, csfd);

    if (rbuffer[0] != (char)cp_rply::ALLENV) {
        throw dinit_protocol_error();
    }

    // 1-byte packet header, then size_t data size
    constexpr size_t allenv_hdr_size = 1 + sizeof(size_t);
    rbuffer.fill_to(csfd, allenv_hdr_size);

    size_t data_size;
    rbuffer.extract(&data_size, 1, sizeof(data_size));
    rbuffer.consume(allenv_hdr_size);

    if (data_size == 0) return;

    if (rbuffer.get_length() == 0) {
        fill_some(rbuffer, csfd);
    }

    std::string env_var;

    while (data_size > 0) {
        // look for a nul terminator
        get_var:
        unsigned contig_len = rbuffer.get_contiguous_length(rbuffer.get_ptr(0));
        unsigned check_len = std::min((size_t) contig_len, data_size);
        for (unsigned i = 0; i < check_len; ++i) {
            if (rbuffer[i] == '\0') {
                // append the last portion
                env_var.append(rbuffer.get_ptr(0), rbuffer.get_ptr(0) + i);
                rbuffer.consume(i + 1);
                data_size -= (i + 1);

                menv.set_var(std::move(env_var));
                env_var.clear();

                if (data_size == 0) {
                    // that's the last one
                    return;
                }

                goto get_var;
            }
        }

        // copy what we have so far to the string, and fill some more
        env_var.append(rbuffer.get_ptr(0), rbuffer.get_ptr(0) + check_len);
        rbuffer.consume(check_len);
        data_size -= check_len;

        if (data_size == 0) {
            // This shouldn't happen, we didn't find the nul terminator at the end
            throw dinit_protocol_error();
        }

        if (rbuffer.get_length() == 0) {
            fill_some(rbuffer, csfd);
        }
    }
}

int main(int argc, char **argv)
{
    using namespace std;

    service_dir_opt service_dir_opts;
    bool user_dinit = (getuid() != 0);  // use user instance defaults/daemon instance
    std::string control_socket_str;
    const char * control_socket_path = nullptr;
    std::string env_file;

    std::vector<std::string> services_to_check;

    // Process command line
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == '-') {
                // An option...
                if (strcmp(argv[i], "--services-dir") == 0 || strcmp(argv[i], "-d") == 0) {
                    if (++i < argc && argv[i][0] != '\0') {
                        service_dir_opts.set_specified_service_dir(argv[i]);
                    }
                    else {
                        cerr << "dinitcheck: '--services-dir' (-d) requires an argument" << endl;
                        return 1;
                    }
                }
                else if (strcmp(argv[i], "--system") == 0 || strcmp(argv[i], "-s") == 0) {
                    user_dinit = false;
                }
                else if (strcmp(argv[i], "--user") == 0 || strcmp(argv[i], "-u") == 0) {
                    user_dinit = true;
                }
                else if (strcmp(argv[i], "--socket-path") == 0 || strcmp(argv[i], "-p") == 0) {
                    ++i;
                    if (i == argc || argv[i][0] == '\0') {
                        cerr << "dinitcheck: --socket-path/-p should be followed by socket path\n";
                        return 1;
                    }
                    control_socket_str = argv[i];
                }
                else if (strcmp(argv[i], "--online") == 0 || strcmp(argv[i], "-n") == 0) {
                    offline_operation = false;
                }
                else if (strcmp(argv[i], "--env-file") == 0 || strcmp(argv[i], "-e") == 0) {
                    ++i;
                    if (i == argc || argv[i][0] == '\0') {
                        cerr << "dinitcheck: --env-file/-e should be followed by environment file path\n";
                        return 1;
                    }
                    env_file = argv[i];
                }
                else if (strcmp(argv[i], "--help") == 0) {
                    cout << "dinitcheck: check dinit service descriptions\n"
                            " --help                       display help\n"
                            " --services-dir <dir>, -d <dir>\n"
                            "                              set base directory for service description\n"
                            "                              files, can be specified multiple times\n"
                            " --online, -n                 use service dirs and environment from running\n"
                            "                              dinit instance\n"
                            " --socket-path <path>, -p <path>\n"
                            "                              use specified socket to connect to daemon (online\n"
                            "                              mode)\n"
                            " --env-file, -e <file>        read environment from specified file\n"
                            " --system, -s                 use defaults for system manager mode\n"
                            " --user, -u                   use defaults for user mode\n"
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

    int socknum = -1;
    cpbuffer_t rbuffer;
    signal(SIGPIPE, SIG_IGN);

    service_dir_pathlist service_dir_paths;
    std::vector<std::string> service_dir_strs; // storage if needed for service_dir_paths

    if (offline_operation) {
        service_dir_opts.build_paths(!user_dinit);
        service_dir_paths = std::move(service_dir_opts.get_paths());
        if (env_file.empty()) {
            if (!user_dinit) {
                env_file = "/etc/dinit/environment";
            }
        }
        if (!env_file.empty()) {
            auto log_inv_env_setting = [&](int line_num) {
                std::cerr << "dinitcheck: warning: Invalid environment variable setting in environment file "
                        << env_file << " (line " << std::to_string(line_num) << ")\n";
            };
            auto log_bad_env_command = [&](int line_num) {
                std::cerr << "dinitcheck: warning: Bad command in environment file "
                        << env_file << " (line " << std::to_string(line_num) << ")\n";
            };

            try {
                read_env_file_inline(env_file.c_str(), true, menv, false, log_inv_env_setting, log_bad_env_command);
            }
            catch (std::system_error &err) {
                std::cerr << "dinitcheck: error read environment file " << env_file << ": "
                        << err.code().message() << "\n";
                return EXIT_FAILURE;
            }
        }
    }
    else {
        if (!control_socket_str.empty()) {
            control_socket_path = control_socket_str.c_str();
        }
        else {
            control_socket_path = get_default_socket_path(control_socket_str, user_dinit);
            if (control_socket_path == nullptr) {
                cerr << "dinitcheck: cannot locate user home directory (set XDG_RUNTIME_DIR, HOME, check /etc/passwd file, or "
                        "specify socket path via -p)" << endl;
                return EXIT_FAILURE;
            }
        }

        try {
            socknum = connect_to_daemon(control_socket_path);

            // Start by querying protocol version:
            check_protocol_version(min_cp_version, max_cp_version, rbuffer, socknum);

            // Read service directories
            service_dir_strs = get_service_description_dirs(socknum, rbuffer);
            for (const std::string &service_dir : service_dir_strs) {
                service_dir_paths.emplace_back(dir_entry(service_dir.c_str(), false));
            }

            menv.clear_no_inherit();
            get_remote_env(socknum, rbuffer);
        }
        catch (cp_old_client_exception &e) {
            std::cerr << "dinitcheck: too old (daemon reports newer protocol version)" << std::endl;
            return EXIT_FAILURE;
        }
        catch (cp_old_server_exception &e) {
            std::cerr << "dinitcheck: daemon too old or protocol error" << std::endl;
            return EXIT_FAILURE;
        }
        catch (cp_read_exception &e) {
            cerr << "dinitcheck: control socket read failure or protocol error" << endl;
            return EXIT_FAILURE;
        }
        catch (cp_write_exception &e) {
            cerr << "dinitcheck: control socket write error: " << std::strerror(e.errcode) << endl;
            return EXIT_FAILURE;
        }
        catch (dinit_protocol_error &e) {
            cerr << "dinitcheck: protocol error" << endl;
            return EXIT_FAILURE;
        }
        catch (general_error &ge) {
            std::cerr << "dinitcheck";
            if (ge.get_action() != nullptr) {
                std::cerr << ": " << ge.get_action();
                std::string &arg = ge.get_arg();
                if (!arg.empty()) {
                    std::cerr << " " << arg;
                }
            }
            if (ge.get_err() != 0) {
                std::cerr << ": " << strerror(ge.get_err());
            }
            std::cerr << '\n';
            return EXIT_FAILURE;
        }
    }

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
            service_record *sr = load_service(service_set, name, service_dir_paths);
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
            // add before_svcs and after_svcs to services_to_check
            for (const std::string &before_name : sr->before_svcs) {
                if (!contains(services_to_check, before_name)) {
                    services_to_check.push_back(before_name);
                }
            }
            for (const std::string &after_name : sr->after_svcs) {
                if (!contains(services_to_check, after_name)) {
                    services_to_check.push_back(after_name);
                }
            }
            // add consumed service (if any) to services to check
            if (!sr->consumer_of_name.empty()) {
                services_to_check.push_back(sr->consumer_of_name);
            }
        }
        catch (service_load_exc &exc) {
            std::cerr << "Unable to load service '" << name << "': " << exc.exc_description << "\n";
            errors_found = true;
        }
    }

    std::cout << "Performing secondary checks...\n";

    for (const auto &svc_name_record : service_set) {
        if (!svc_name_record.second->consumer_of_name.empty()) {
            auto consumer_of_it = service_set.find(svc_name_record.second->consumer_of_name);
            if (consumer_of_it != service_set.end()) {
                if (consumer_of_it->second->log_type != log_type_id::PIPE) {
                    std::cerr << "Service '" << svc_name_record.first << "': specified as consumer of service '"
                            << consumer_of_it->first << "' which has log-type that is not 'pipe'.\n";
                    errors_found = true;
                }
                else if (!value(consumer_of_it->second->service_type).is_in(service_type_t::PROCESS,
                        service_type_t::BGPROCESS, service_type_t::SCRIPTED)) {
                    std::cerr << "Service '" << svc_name_record.first << "': specified as consumer of service '"
                            << consumer_of_it->first << "' which is not a process-based service.\n";
                    errors_found = true;
                }
            }
            else {
                std::cerr << "Warning: Service '" << svc_name_record.first << "' specified as consumer of service '"
                        << consumer_of_it->first << "' which was not found.\n";
            }
        }

        // "before" ordering links are like reverse-dependencies: set up dependencies in the forwards direction
        // (from the dependent). Similarly for "after" links set up a dependency. These dependencies allow cycle
        // checking.
        for (const std::string &before_name : svc_name_record.second->before_svcs) {
            auto before_svc_it = service_set.find(before_name);
            if (before_svc_it != service_set.end()) {
                before_svc_it->second->dependencies.emplace_back(svc_name_record.first,
                        dependency_type::BEFORE);
            }
        }
        for (const std::string &after_name : svc_name_record.second->after_svcs) {
        	auto after_svc_it = service_set.find(after_name);
        	if (after_svc_it != service_set.end()) {
        		svc_name_record.second->dependencies.emplace_back(after_svc_it->first,
        				dependency_type::AFTER);
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
            if (dep_it->dep_type == dependency_type::AFTER) {
            	std::cerr << " (via 'after')";
            }
            std::cerr << "\n";
        }
        std::cerr << "    " << std::get<0>(service_chain[0])->name << ".\n";
    }

    std::cerr << "Secondary checks complete.\n";

    if (! errors_found) {
        std::cout << "No problems found.\n";
    }
    else {
        std::cout << "One or more errors/warnings issued.\n";
    }

    return errors_found ? EXIT_FAILURE : EXIT_SUCCESS;
}

static void report_service_description_err(const std::string &service_name, const char *setting_name,
        const std::string &what)
{
    std::cerr << "Service '" << service_name << "' setting '" << setting_name << "': " << what << "\n";
    errors_found = true;
}

static void report_service_description_err(const std::string &service_name, file_pos &input_pos,
        const std::string &what)
{
    std::cerr << "Service '" << service_name << "' (" << input_pos.get_file_name() << ":"
            << input_pos.get_line_num() << "): " << what << "\n";
    errors_found = true;
}

static void report_service_description_err(const std::string &service_name, const std::string &what)
{
    std::cerr << "Service '" << service_name << "': " << what << "\n";
    errors_found = true;
}

static void report_service_description_exc(service_description_exc &exc)
{
    if (exc.input_pos.get_line_num() != (unsigned)-1) {
        report_service_description_err(exc.service_name, exc.input_pos, exc.exc_description);
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

static void report_general_warning(string_view msg)
{
    std::cerr << "dinitcheck: Warning: " << msg.data() << "\n";
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
    dirent *dent = readdir(depdir);
    while (dent != nullptr) {
        char *name = dent->d_name;
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
    using ::string_view;

    auto found = services.find(name);
    if (found != services.end()) {
        return found->second;
    }

    string_view base_name = name;

    string service_arg;
    auto at_pos = name.find('@');
    if (at_pos != string::npos) {
    	service_arg = name.substr(at_pos + 1);
    	base_name = string_view(name.data(), at_pos);
    }

    string service_wdir;
    string service_filename;
    ifstream service_file;
    int dirfd;

    int fail_load_errno = 0;
    std::string fail_load_path;

    // Couldn't find one. Have to load it.
    for (auto &service_dir : service_dirs) {
        service_filename = service_dir.get_dir();
        service_wdir = service_filename;
        if (*(service_filename.rbegin()) != '/') {
            service_filename += '/';
        }
        service_filename += base_name;

        service_file.open(service_filename.c_str(), ios::in);
        if (service_file) break;

        if (errno != ENOENT && fail_load_errno == 0) {
            fail_load_errno = errno;
            fail_load_path = std::move(service_filename);
        }
    }

    if (!service_file) {
        if (fail_load_errno == 0) {
            throw service_not_found(name);
        }
        else {
            throw service_load_error(name, std::move(fail_load_path), fail_load_errno);
        }
    }

    bool issued_var_subst_warning = false;

    environment::env_map renvmap = menv.build();

    auto resolve_var = [&](const string &name) {
        if (offline_operation && !issued_var_subst_warning) {
            report_general_warning("Variable substitution performed by dinitcheck "
                    "for file paths may not match dinit daemon (environment may differ); "
                    "use --online to avoid this warning");
            issued_var_subst_warning = true;
        }
        return resolve_env_var(name, renvmap);
    };

    service_settings_wrapper<prelim_dep> settings;

    string line;
    service_file.exceptions(ios::badbit);

    file_input_stack input_stack;
    input_stack.push(std::move(service_filename), std::move(service_file));

    try {
        process_service_file(name, input_stack,
                [&](string &line, file_pos_ref input_pos, string &setting, setting_op_t op,
                        string_iterator &i, string_iterator &end) -> void {

                    auto process_dep_dir_n = [&](std::list<prelim_dep> &deplist,
                    		const std::string &waitsford,
                            dependency_type dep_type) -> void {
                        const string &service_filename = input_stack.current_file_name();
                        process_dep_dir(name.c_str(), service_filename, deplist, waitsford, dep_type);
                    };

                    auto load_service_n = [&](const string &dep_name) -> const string & {
                        return dep_name;
                    };

                    try {
                        process_service_line(settings, name.c_str(), service_arg.c_str(), line,
                        		input_pos, setting, op, i, end, load_service_n, process_dep_dir_n,
								resolve_var);
                    }
                    catch (service_description_exc &exc) {
                        if (exc.service_name.empty()) {
                            exc.service_name = name;
                        }
                        report_service_description_exc(exc);
                    }
                },
                service_arg.c_str(), resolve_var);
    }
    catch (std::system_error &sys_err)
    {
        report_error(sys_err, name);
        throw service_load_exc(name, input_stack.current_file_name() + ": error while reading service description");
    }

    auto report_err = [&](const char *msg) {
        report_service_description_err(name, msg);
    };

    environment srv_env{};

    // Fill user vars before reading env file
    if (settings.export_passwd_vars) {
        try {
            fill_environment_userinfo(settings.run_as_uid, name, srv_env);
        }
        catch (service_load_exc &load_exc) {
            report_service_description_err(name, load_exc.exc_description);
        }
    }

    // Set service name in environment if desired
    if (settings.export_service_name) {
        std::string envname = "DINIT_SERVICE=";
        envname += name;
        srv_env.set_var(std::move(envname));
    }

    if (!settings.env_file.empty()) {
        try {
            std::string fullpath = combine_paths(service_wdir, settings.env_file.c_str());

            auto log_inv_env_setting = [&](int line_num) {
                report_service_description_err(name,
                        std::string("Invalid environment variable setting in environment file " + fullpath
                                + " (line ") + std::to_string(line_num) + ")");
            };
            auto log_bad_env_command = [&](int line_num) {
                report_service_description_err(name,
                        std::string("Bad command in environment file ") + fullpath + " (line " + std::to_string(line_num) + ")");
            };

            read_env_file_inline(fullpath.c_str(), false, srv_env, true, log_inv_env_setting, log_bad_env_command);
        } catch (const std::system_error &se) {
            report_service_description_err(name, std::string("could not load environment file: ") + se.what());
        }
    }

    renvmap = srv_env.build(menv);

    settings.finalise(report_err, nullptr, report_err, resolve_var);

    if (!settings.working_dir.empty()) {
        service_wdir = settings.working_dir;
    }
    int oflags = O_DIRECTORY;
#ifdef O_PATH
    oflags |= O_PATH;
#else
    oflags |= O_RDONLY;
#endif
    dirfd = open(service_wdir.c_str(), oflags);
    if (dirfd < 0) {
        report_service_description_err(name,
                std::string("could not open service working directory: ") + strerror(errno));
        dirfd = AT_FDCWD;
    }

    auto check_command = [&](const char *setting_name, const char *command) {
        struct stat command_stat;
        if (command[0] != '/') {
            report_service_description_err(name,
                    std::string("executable '") + command + "' is not an absolute path");
        }
        else if (fstatat(dirfd, command, &command_stat, 0) == -1) {
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

    if (settings.log_type == log_type_id::LOGFILE && !settings.logfile.empty()) {
        string logfile_dir = parent_path(settings.logfile);
        if (!logfile_dir.empty()) {
            struct stat logfile_dir_stat;
            if (fstatat(dirfd, logfile_dir.c_str(), &logfile_dir_stat, 0) == -1) {
                report_service_description_err(name,
                        std::string("could not access logfile directory '") + logfile_dir + "': " + strerror(errno));
            }
            else {
                if ((logfile_dir_stat.st_mode & S_IFDIR) == 0) {
                    report_service_description_err(name, std::string("logfile directory '")
                            + logfile_dir + "' exists but is not a directory.");
                }
            }
        }
    }

    if (dirfd != AT_FDCWD) {
        close(dirfd);
    }

    return new service_record(name, settings.service_type, settings.chain_to_name, settings.depends,
            settings.before_svcs, settings.after_svcs, settings.consumer_of_name, settings.log_type);
}
