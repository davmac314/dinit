#include <algorithm>
#include <string>
#include <fstream>
#include <locale>
#include <limits>
#include <list>

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
    using namespace dinit_load;
    if (do_sub_vars) {
        cmdline_var_subst(line, offsets, resolve_env_var);
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
        dependency_type dep_type,
        const service_record *avoid_circular)
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

service_record * dirload_service_set::load_service(const char * name, const service_record *avoid_circular)
{
    return load_reload_service(name, nullptr, avoid_circular);
}

service_record * dirload_service_set::reload_service(service_record * service)
{
    return load_reload_service(service->get_name().c_str(), service, service);
}

// Update the dependencies of the specified service atomically. May fail with bad_alloc.
static void update_depenencies(service_record *service,
        dinit_load::service_settings_wrapper<prelim_dep> &settings)
{
    std::list<service_dep> &deps = service->get_dependencies();
    auto first_preexisting = deps.begin();

    // build a set of services currently issuing acquisition
    std::unordered_set<service_record *> deps_with_acqs;
    for (auto i = deps.begin(), e = deps.end(); i != e; ++i) {
        if (i->holding_acq) {
            deps_with_acqs.insert(i->get_to());
        }
    }

    try {
        // Insert all the new dependencies before the first pre-existing dependency
        for (auto &new_dep : settings.depends) {
            bool has_acq = deps_with_acqs.count(new_dep.to);
            service->add_dep(new_dep.to, new_dep.dep_type, first_preexisting, has_acq);
        }
    }
    catch (...) {
        // remove the inserted dependencies
        for (auto i = deps.begin(); i != first_preexisting; ++i) {
            i = service->rm_dep(i);
        }

        // re-throw the exception
        throw;
    }

    // Now remove all pre-existing dependencies (no exceptions possible from here).
    for( ; first_preexisting != deps.end(); ) {
        first_preexisting = service->rm_dep(first_preexisting);
    }
}

// Update the command, and dependencies, of the specified service atomically. May fail with bad_alloc.
static void update_command_and_dependencies(base_process_service *service,
        dinit_load::service_settings_wrapper<prelim_dep> &settings)
{
    // Get the current command parts
    std::string orig_cmd; std::vector<const char *> orig_arg_parts;
    service->get_command(orig_cmd, orig_arg_parts);

    // Separate the new command parts and set
    std::vector<const char *> cmd_arg_parts = separate_args(settings.command, settings.command_offsets);
    service->set_command(std::move(settings.command), std::move(cmd_arg_parts));

    try {
        update_depenencies(service, settings);
    }
    catch (...) {
        // restore original command
        service->set_command(std::move(orig_cmd), std::move(orig_arg_parts));

        // re-throw the exception
        throw;
    }
}

service_record * dirload_service_set::load_reload_service(const char *name, service_record *reload_svc,
        const service_record *avoid_circular)
{
    // For reload, we have the following problems:
    // - ideally want to allow changing service type, at least for stopped services. That implies creating
    //   a new (replacement) service_record object, at least in cases where the type does change.
    // - dependencies may change (including addition of new dependencies which aren't yet loaded). We need
    //   to prevent cyclic dependencies forming.
    // - We want atomicity. If any new settings are not valid/alterable, or if a cyclic dependency is
    //   created, nothing should change. Ideally this would extend to unloading any dependencies which were
    //   loaded as part of the reload attempt.
    // - We need to either transfer handles referring to the old service (so that they refer to the new
    //   service), or make them invalid. Or, we alter the original service without creating a new one
    //   (which we can only do if the type doesn't change).

    // Approach:
    // - remember the initial service count, so we can remove services loaded as part of the reload
    //   operation if we want to abort it later (i.e. if service count changed from N to N+X, remove the
    //   last X services)
    // - check that the new settings are valid (if the service is running, check if the settings can be
    //   altered, though we may just defer some changes until service is restarted)
    // - check all dependencies of the newly created service record for cyclic dependencies, via depth-first
    //   traversal.
    // - If changing type:
    //   - create the service initially just as if loading a new service (but with no dummy placeholder,
    //     use the original service for that).
    //   - switch all dependents to depend on the new record. Copy necessary runtime data from the original
    //     to the new service record. Remove dependencies from the old record, and release any dependency
    //     services as appropriate (so they stop if no longer needed). Finally, remove the old service
    //     record and delete it.
    //  Otherwise:
    //   - copy the new settings to the existing service
    //   - fix dependencies
    //
    // Limitations:
    // - caller must check there are no handles (or only a single requesting handle) to the service before
    //   calling
    // - cannot change the type of a non-stopped service

    using std::string;
    using std::ifstream;
    using std::ios;
    using std::ios_base;
    using std::locale;
    using std::isspace;

    using std::list;
    using std::pair;

    using namespace dinit_load;

    if (reload_svc == nullptr) {
        // First try and find an existing record...
        service_record * rval = find_service(string(name));
        if (rval != nullptr) {
            if (rval == avoid_circular || rval->is_dummy()) {
                throw service_cyclic_dependency(name);
            }
            return rval;
        }
    }

    service_record *rval = nullptr;
    service_record *dummy = nullptr;

    ifstream service_file;
    string service_filename;

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

    string line;
    // getline can set failbit if it reaches end-of-file, we don't want an exception in that case. There's
    // no good way to handle an I/O error however, so we'll have exceptions thrown on badbit:
    service_file.exceptions(ios::badbit);

    bool create_new_record = true;

    try {
        if (reload_svc == nullptr) {
            // Add a dummy service record now to prevent infinite recursion in case of cyclic dependency.
            // We replace this with the real service later (or remove it if we find a configuration error).
            dummy = new service_record(this, string(name));
            add_service(dummy);
        }

        process_service_file(name, service_file,
                [&](string &line, string &setting, string_iterator &i, string_iterator &end) -> void {

            auto process_dep_dir_n = [&](std::list<prelim_dep> &deplist, const std::string &waitsford,
                    dependency_type dep_type) -> void {
                process_dep_dir(*this, name, service_filename, deplist, waitsford, dep_type, reload_svc);
            };

            auto load_service_n = [&](const string &dep_name) -> service_record * {
                return load_service(dep_name.c_str(), reload_svc);
            };

            process_service_line(settings, name, line, setting, i, end, load_service_n, process_dep_dir_n);
        });

        service_file.close();

        auto report_err = [&](const char *msg){
            throw service_description_exc(name, msg);
        };

        settings.finalise(report_err);
        auto service_type = settings.service_type;

        if (reload_svc != nullptr) {
            // Make sure settings are able to be changed/are compatible
            service_record *service = reload_svc;
            if (service->get_state() != service_state_t::STOPPED) {
                // Can not change type of a running service.
                if (service_type != service->get_type()) {
                    throw service_description_exc(name, "cannot change type of non-stopped service.");
                }
                // Can not alter a starting/stopping service, at least for now.
                if (service->get_state() != service_state_t::STARTED) {
                    throw service_description_exc(name,
                            "cannot alter settings for service which is currently starting/stopping.");
                }

                // Check validity of dependencies (if started, regular deps must be started)
                for (auto &new_dep : settings.depends) {
                    if (new_dep.dep_type == dependency_type::REGULAR) {
                        if (new_dep.to->get_state() != service_state_t::STARTED) {
                            throw service_description_exc(name,
                                    std::string("cannot add non-started dependency '")
                                        + new_dep.to->get_name() + "'.");
                        }
                    }
                }

                // Cannot change certain flags
                auto current_flags = service->get_flags();
                if (current_flags.starts_on_console != settings.onstart_flags.starts_on_console
                        || current_flags.shares_console != settings.onstart_flags.shares_console) {
                    throw service_description_exc(name, "cannot change starts_on_console/"
                            "shares_console flags for a running service.");
                }

                // Cannot change pid file
                if (service->get_type() == service_type_t::BGPROCESS) {
                    auto *bgp_service = static_cast<bgproc_service *>(service);
                    if (bgp_service->get_pid_file() != settings.pid_file) {
                        throw service_description_exc(name, "cannot change pid_file for running service.");
                    }
                }

                // Cannot change inittab_id/inittab_line
                #if USE_UTMPX
                    if (service->get_type() == service_type_t::PROCESS) {
                        auto *proc_service = static_cast<process_service *>(service);
                        auto *svc_utmp_id = proc_service->get_utmp_id();
                        auto *svc_utmp_ln = proc_service->get_utmp_line();
                        if (strncmp(svc_utmp_id, settings.inittab_id, proc_service->get_utmp_id_size()) != 0
                                || strncmp(svc_utmp_ln, settings.inittab_line,
                                        proc_service->get_utmp_line_size()) != 0) {
                            throw service_description_exc(name, "cannot change inittab-id or inittab-line "
                                    "settings for running service.");
                        }
                    }
                #endif

                // Already started; we must replace settings on existing service record
                create_new_record = false;
            }
        }

        // Note, we need to be very careful to handle exceptions properly and roll back any changes that
        // we've made before the exception occurred.

        if (service_type == service_type_t::PROCESS) {
            do_env_subst(settings.command, settings.command_offsets, settings.do_sub_vars);
            std::vector<const char *> stop_arg_parts = separate_args(settings.stop_command, settings.stop_command_offsets);
            process_service *rvalps;
            if (create_new_record) {
                rvalps = new process_service(this, string(name), std::move(settings.command),
                        settings.command_offsets, settings.depends);
            }
            else {
                rvalps = static_cast<process_service *>(reload_svc);
                update_command_and_dependencies(rvalps, settings);
            }
            rval = rvalps;
            // All of the following should be noexcept or must perform rollback on exception
            rvalps->set_stop_command(std::move(settings.stop_command), std::move(stop_arg_parts));
            rvalps->set_working_dir(std::move(settings.working_dir));
            rvalps->set_env_file(std::move(settings.env_file));
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
        }
        else if (service_type == service_type_t::BGPROCESS) {
            do_env_subst(settings.command, settings.command_offsets, settings.do_sub_vars);
            std::vector<const char *> stop_arg_parts = separate_args(settings.stop_command, settings.stop_command_offsets);
            bgproc_service *rvalps;
            if (create_new_record) {
                rvalps = new bgproc_service(this, string(name), std::move(settings.command),
                        settings.command_offsets, settings.depends);
            }
            else {
                rvalps = static_cast<bgproc_service *>(reload_svc);
                update_command_and_dependencies(rvalps, settings);
            }
            rval = rvalps;
            // All of the following should be noexcept or must perform rollback on exception
            rvalps->set_stop_command(std::move(settings.stop_command), std::move(stop_arg_parts));
            rvalps->set_working_dir(std::move(settings.working_dir));
            rvalps->set_env_file(std::move(settings.env_file));
            rvalps->set_rlimits(std::move(settings.rlimits));
            rvalps->set_pid_file(std::move(settings.pid_file));
            rvalps->set_restart_interval(settings.restart_interval, settings.max_restarts);
            rvalps->set_restart_delay(settings.restart_delay);
            rvalps->set_stop_timeout(settings.stop_timeout);
            rvalps->set_start_timeout(settings.start_timeout);
            rvalps->set_extra_termination_signal(settings.term_signal);
            rvalps->set_run_as_uid_gid(settings.run_as_uid, settings.run_as_gid);
            settings.onstart_flags.runs_on_console = false;
        }
        else if (service_type == service_type_t::SCRIPTED) {
            do_env_subst(settings.command, settings.command_offsets, settings.do_sub_vars);
            std::vector<const char *> stop_arg_parts = separate_args(settings.stop_command, settings.stop_command_offsets);
            scripted_service *rvalps;
            if (create_new_record) {
                rvalps = new scripted_service(this, string(name), std::move(settings.command),
                        settings.command_offsets, settings.depends);
            }
            else {
                rvalps = static_cast<scripted_service *>(reload_svc);
                update_command_and_dependencies(rvalps, settings);
            }
            rval = rvalps;
            // All of the following should be noexcept or must perform rollback on exception
            rvalps->set_stop_command(std::move(settings.stop_command), std::move(stop_arg_parts));
            rvalps->set_working_dir(std::move(settings.working_dir));
            rvalps->set_env_file(std::move(settings.env_file));
            rvalps->set_rlimits(std::move(settings.rlimits));
            rvalps->set_stop_timeout(settings.stop_timeout);
            rvalps->set_start_timeout(settings.start_timeout);
            rvalps->set_extra_termination_signal(settings.term_signal);
            rvalps->set_run_as_uid_gid(settings.run_as_uid, settings.run_as_gid);
        }
        else {
            if (create_new_record) {
                rval = new service_record(this, string(name), service_type, settings.depends);
            }
            else {
                rval = reload_svc;
                update_depenencies(rval, settings);
            }
        }

        rval->set_log_file(std::move(settings.logfile));
        rval->set_auto_restart(settings.auto_restart);
        rval->set_smooth_recovery(settings.smooth_recovery);
        rval->set_flags(settings.onstart_flags);
        rval->set_socket_details(std::move(settings.socket_path), settings.socket_perms,
                settings.socket_uid, settings.socket_gid);
        rval->set_chain_to(std::move(settings.chain_to_name));

        if (create_new_record && reload_svc != nullptr) {
            // switch dependencies on old record so that they refer to the new record

            // Add dependent-link for all dependencies. Add to the new service first, so we can rollback
            // on failure:
            int added_dep_links = 0;
            try {
                for (auto &dep : rval->get_dependencies()) {
                    dep.get_to()->get_dependents().push_back(&dep);
                    added_dep_links++;
                }
            }
            catch (...) {
                for (auto &dep : rval->get_dependencies()) {
                    if (added_dep_links-- == 0) break;
                    dep.get_to()->get_dependents().pop_back();
                }
                throw;
            }

            // Remove dependent-link for all dependencies from the original:
            reload_svc->prepare_for_unload();

            // Set links in all dependents to the original to point to the new service:
            rval->get_dependents() = std::move(reload_svc->get_dependents());
            for (auto n : rval->get_dependents()) {
                n->set_to(rval);
            }
        }

        if (dummy != nullptr) {
            auto iter = std::find(records.begin(), records.end(), dummy);
            *iter = rval;
            delete dummy;
        }

        return rval;
    }
    catch (setting_exception &setting_exc)
    {
        // Must remove the dummy service record.
        if (dummy != nullptr) {
            records.erase(std::find(records.begin(), records.end(), dummy));
            delete dummy;
        }
        if (create_new_record) delete rval;
        throw service_description_exc(name, std::move(setting_exc.get_info()));
    }
    catch (std::system_error &sys_err)
    {
        if (dummy != nullptr) {
            records.erase(std::find(records.begin(), records.end(), dummy));
            delete dummy;
        }
        if (create_new_record) delete rval;
        throw service_description_exc(name, sys_err.what());
    }
    catch (...) // (should only be std::bad_alloc / service_description_exc)
    {
        if (dummy != nullptr) {
            records.erase(std::find(records.begin(), records.end(), dummy));
            delete dummy;
        }
        if (create_new_record) delete rval;
        throw;
    }
}
