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
#include "dinit-utmp.h"

using string = std::string;
using string_iterator = std::string::iterator;

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

    closedir(depdir);
}

// Find a service record, or load it from file. If the service has dependencies, load those also.
//
// Throws service_load_exc (or subclass) if a dependency cycle is found or if another
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

    service_settings_wrapper<prelim_dep> settings;

    string line;
    // getline can set failbit if it reaches end-of-file, we don't want an exception in that case. There's
    // no good way to handle an I/O error however, so we'll have exceptions thrown on badbit:
    service_file.exceptions(ios::badbit);
    
    // Add a dummy service record now to prevent infinite recursion in case of cyclic dependency.
    // We replace this with the real service later (or remove it if we find a configuration error).
    rval = new service_record(this, string(name));
    add_service(rval);
    
    try {
        process_service_file(name, service_file,
                [&](string &line, string &setting, string_iterator &i, string_iterator &end) -> void {

            auto process_dep_dir_n = [&](std::list<prelim_dep> &deplist, const std::string &waitsford,
                    dependency_type dep_type) -> void {
                process_dep_dir(*this, name, service_filename, deplist, waitsford, dep_type);
            };

            auto load_service_n = [&](const string &dep_name) -> service_record * {
                return load_service(dep_name.c_str());
            };

            process_service_line(settings, name, line, setting, i, end, load_service_n, process_dep_dir_n);
        });

        service_file.close();
        
        auto service_type = settings.service_type;

        if (service_type == service_type_t::PROCESS || service_type == service_type_t::BGPROCESS
                || service_type == service_type_t::SCRIPTED) {
            if (settings.command.length() == 0) {
                throw service_description_exc(name, "Service command not specified");
            }
        }
        
        // Now replace the dummy service record with a real record:
        for (auto iter = records.begin(); iter != records.end(); iter++) {
            if (*iter == rval) {
                // We've found the dummy record
                delete rval;
                if (service_type == service_type_t::PROCESS) {
                    do_env_subst(settings.command, settings.command_offsets, settings.do_sub_vars);
                    auto rvalps = new process_service(this, string(name), std::move(settings.command),
                            settings.command_offsets, settings.depends);
                    rvalps->set_working_dir(settings.working_dir);
                    rvalps->set_env_file(settings.env_file);
                    rvalps->set_rlimits(std::move(settings.rlimits));
                    rvalps->set_restart_interval(settings.restart_interval, settings.max_restarts);
                    rvalps->set_restart_delay(settings.restart_delay);
                    rvalps->set_stop_timeout(settings.stop_timeout);
                    rvalps->set_start_timeout(settings.start_timeout);
                    rvalps->set_extra_termination_signal(settings.term_signal);
                    rvalps->set_run_as_uid_gid(settings.run_as_uid, settings.run_as_gid);
                    rvalps->set_notification_fd(settings.readiness_fd);
                    rvalps->set_notification_var(std::move(settings.readiness_var));
                    #if USE_UTMPX
                    rvalps->set_utmp_id(settings.inittab_id);
                    rvalps->set_utmp_line(settings.inittab_line);
                    #endif
                    rval = rvalps;
                }
                else if (service_type == service_type_t::BGPROCESS) {
                    do_env_subst(settings.command, settings.command_offsets, settings.do_sub_vars);
                    auto rvalps = new bgproc_service(this, string(name), std::move(settings.command),
                            settings.command_offsets, settings.depends);
                    rvalps->set_working_dir(settings.working_dir);
                    rvalps->set_env_file(settings.env_file);
                    rvalps->set_rlimits(std::move(settings.rlimits));
                    rvalps->set_pid_file(std::move(settings.pid_file));
                    rvalps->set_restart_interval(settings.restart_interval, settings.max_restarts);
                    rvalps->set_restart_delay(settings.restart_delay);
                    rvalps->set_stop_timeout(settings.stop_timeout);
                    rvalps->set_start_timeout(settings.start_timeout);
                    rvalps->set_extra_termination_signal(settings.term_signal);
                    rvalps->set_run_as_uid_gid(settings.run_as_uid, settings.run_as_gid);
                    settings.onstart_flags.runs_on_console = false;
                    rval = rvalps;
                }
                else if (service_type == service_type_t::SCRIPTED) {
                    do_env_subst(settings.command, settings.command_offsets, settings.do_sub_vars);
                    auto rvalps = new scripted_service(this, string(name), std::move(settings.command),
                            settings.command_offsets, settings.depends);
                    rvalps->set_stop_command(settings.stop_command, settings.stop_command_offsets);
                    rvalps->set_working_dir(settings.working_dir);
                    rvalps->set_env_file(settings.env_file);
                    rvalps->set_rlimits(std::move(settings.rlimits));
                    rvalps->set_stop_timeout(settings.stop_timeout);
                    rvalps->set_start_timeout(settings.start_timeout);
                    rvalps->set_extra_termination_signal(settings.term_signal);
                    rvalps->set_run_as_uid_gid(settings.run_as_uid, settings.run_as_gid);
                    rval = rvalps;
                }
                else {
                    rval = new service_record(this, string(name), service_type, settings.depends);
                }
                rval->set_log_file(settings.logfile);
                rval->set_auto_restart(settings.auto_restart);
                rval->set_smooth_recovery(settings.smooth_recovery);
                rval->set_flags(settings.onstart_flags);
                rval->set_socket_details(std::move(settings.socket_path), settings.socket_perms,
                        settings.socket_uid, settings.socket_gid);
                rval->set_chain_to(std::move(settings.chain_to_name));
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
    catch (std::system_error &sys_err)
    {
        records.erase(std::find(records.begin(), records.end(), rval));
        delete rval;
        throw service_description_exc(name, sys_err.what());
    }
    catch (...) // (should only be std::bad_alloc)
    {
        records.erase(std::find(records.begin(), records.end(), rval));
        delete rval;
        throw;
    }
}
