#include <algorithm>
#include <string>
#include <fstream>
#include <locale>
#include <limits>
#include <list>
#include <utility>
#include <iterator>

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

// Perform environment variable substitution on a command line or other setting.
//   line -  the string storing the command and arguments
//   offsets - the [start,end) pair of offsets of the command and each argument within the string
//
// throws:  std::bad_alloc, std::length_error, service_description_exc
static void do_env_subst(const char *setting_name, ha_string &line,
        std::list<std::pair<unsigned,unsigned>> &offsets,
        environment::env_map const &envmap, const char *arg)
{
    using namespace dinit_load;
    std::string line_s = std::string(line.c_str(), line.length());
    value_var_subst(setting_name, line_s, offsets, resolve_env_var, &envmap, arg);
    line = line_s;
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
        errno = 0; // errno may have changed in the meantime
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

using service_dep_list = decltype(std::declval<dinit_load::service_settings_wrapper<prelim_dep>>().depends);

// Check for dependency cycles for the specified service (orig) with the given set of dependencies. Report
// any cycles as occurring in _report_svc_name_.
static void check_cycle(service_dep_list &deps, service_record *orig, const std::string &report_svc_name)
{
    linked_uo_set<service_record *> pending;
    for (auto &new_dep : deps) {
        if (new_dep.to == orig) {
            throw service_cyclic_dependency(report_svc_name);
        }
        pending.add_back(new_dep.to);
    }

    for (auto i = pending.begin(); i != pending.end(); ++i) {
        auto &dep_list = (*i)->get_dependencies();
        for (auto &dep : dep_list) {
            if (dep.get_to() == orig) {
                throw service_cyclic_dependency(report_svc_name);
            }
            pending.add_back(dep.get_to());
        }
    }
}

// Check for dependency cycles in "before" dependencies, _orig_ is where cycles will be identified.
static void check_cycle(service_dep_list &deps, service_record *orig)
{
    check_cycle(deps, orig, orig->get_name());
}

// Update the dependencies of the specified service atomically.
// May fail with bad_alloc, service_cyclic_dependency.
static void update_depenencies(service_record *service,
        dinit_load::service_settings_wrapper<prelim_dep> &settings,
        std::list<service_dep> &before_deps)
{
    check_cycle(settings.depends, service);

    std::list<service_dep> &deps = service->get_dependencies();
    auto first_preexisting = deps.begin();

    auto &depts = service->get_dependents();
    auto first_pre_dept = depts.begin();

    try {
        // Insert all new dependents (from "before" relationships) before the first pre-existing dependent
        for (auto new_dept_i = before_deps.begin(); new_dept_i != before_deps.end(); ) {
            auto &new_dept = *new_dept_i;
            depts.insert(depts.begin(), &new_dept);
            // splice the dependency into the dependent:
            auto next_dept_i = std::next(new_dept_i);
            auto &from_deps = new_dept.get_from()->get_dependencies();
            from_deps.splice(from_deps.begin(), before_deps, new_dept_i);
            new_dept_i = next_dept_i;
        }

        // Insert all the new dependencies before the first pre-existing dependency
        for (auto i = settings.depends.begin(); i != settings.depends.end(); ) {
            auto &new_dep = *i;
            service->add_dep(new_dep.to, new_dep.dep_type, first_preexisting);
            i = settings.depends.erase(i);
        }
    }
    catch (...) {
        // remove "before" dependencies from dependents
        for (auto i = depts.begin(); i != first_pre_dept; ) {
            auto next_i = std::next(i);
            (*i)->get_from()->rm_dep(**i);
            i = next_i;
        }

        // remove the inserted dependencies
        for (auto i = deps.begin(); i != first_preexisting; ) {
            i = service->rm_dep(i);
        }

        // re-throw the exception
        throw;
    }

    // Now remove all pre-existing dependencies, except for "before" dependencies (which come from the
    // linked service and so must be retained)
    for( ; first_preexisting != deps.end(); ) {
        if (first_preexisting->dep_type != dependency_type::BEFORE) {
            first_preexisting = service->rm_dep(first_preexisting);
        }
        else {
            ++first_preexisting;
        }
    }

    // Also remove pre-existing "before" dependents (because they come from this service)
    for( ; first_pre_dept != depts.end(); ) {
        auto next_pre_dept = std::next(first_pre_dept);
        if ((*first_pre_dept)->dep_type == dependency_type::BEFORE) {
            (*first_pre_dept)->get_from()->rm_dep(**first_pre_dept);
        }
        first_pre_dept = next_pre_dept;
    }
}

// Update the command, and dependencies, of the specified service atomically.
// May fail with bad_alloc, service_cyclic_dependency.
static void update_command_and_dependencies(base_process_service *service,
        dinit_load::service_settings_wrapper<prelim_dep> &settings,
        std::list<service_dep> &before_deps)
{
    // Get the current command parts
    ha_string orig_cmd; std::vector<const char *> orig_arg_parts;
    service->get_command(orig_cmd, orig_arg_parts);

    // Separate the new command parts and set
    std::vector<const char *> cmd_arg_parts = separate_args(settings.command, settings.command_offsets);
    service->set_command(std::move(settings.command), std::move(cmd_arg_parts));

    try {
        update_depenencies(service, settings, before_deps);
    }
    catch (...) {
        // restore original command
        service->set_command(std::move(orig_cmd), std::move(orig_arg_parts));

        // re-throw the exception
        throw;
    }
}

// Check that the provided settings are compatible / ok to be applied to the specified (already loaded)
// service.
// Returns: true if a new service record must be created, false otherwise
static bool check_settings_for_reload(service_record *service,
        dinit_load::service_settings_wrapper<prelim_dep> &settings)
{
    using namespace dinit_load;

    const std::string &name = service->get_name();

    // To begin, assume a new record is needed
    bool create_new_record = true;

    if (service->get_state() != service_state_t::STOPPED) {
        // Can not change type of a running service.
        if (settings.service_type != service->get_type()) {
            throw service_load_exc(name, "cannot change type of non-stopped service.");
        }
        // Can not alter a starting/stopping service, at least for now.
        if (service->get_state() != service_state_t::STARTED) {
            throw service_load_exc(name,
                    "cannot alter settings for service which is currently starting/stopping.");
        }

        // Check validity of dependencies (if started, regular deps must be started)
        for (auto &new_dep : settings.depends) {
            if (new_dep.dep_type == dependency_type::REGULAR) {
                if (new_dep.to->get_state() != service_state_t::STARTED) {
                    throw service_load_exc(name,
                            std::string("cannot add non-started dependency '")
                                + new_dep.to->get_name() + "'.");
                }
            }
        }

        // Cannot change certain flags
        auto current_flags = service->get_flags();
        if (current_flags.starts_on_console != settings.onstart_flags.starts_on_console
                || current_flags.shares_console != settings.onstart_flags.shares_console) {
            throw service_load_exc(name, "cannot change starts_on_console/"
                    "shares_console flags for a running service.");
        }

        // Cannot change pid file
        if (service->get_type() == service_type_t::BGPROCESS) {
            auto *bgp_service = static_cast<bgproc_service *>(service);
            if (bgp_service->get_pid_file() != settings.pid_file) {
                throw service_load_exc(name, "cannot change pid_file for running service.");
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
                    throw service_load_exc(name, "cannot change inittab-id or inittab-line "
                            "settings for running service.");
                }
            }
        #endif

        // Cannot change log type
        if (value(service->get_type()).is_in(service_type_t::PROCESS, service_type_t::BGPROCESS,
                service_type_t::SCRIPTED)) {
            base_process_service *bps = static_cast<base_process_service *>(service);
            if (bps->get_log_mode() != settings.log_type) {
                throw service_load_exc(name, "cannot change log-type for running service.");
            }
        }

        // Already started; we must replace settings on existing service record
        create_new_record = false;
    }
    else if (settings.service_type == service->get_type()) {
        // No need to create a new record if the type hasn't changed
        create_new_record = false;
    }

    return create_new_record;
}

service_record * dirload_service_set::load_reload_service(const char *fullname, service_record *reload_svc,
        const service_record *avoid_circular)
{
    // Load a new service, or reload an already-loaded service.

    // For reload, we have the following problems:
    // - ideally want to allow changing service type, at least for stopped services. That implies creating
    //   a new (replacement) service_record object, at least in cases where the type does change.
    // - dependencies may change (including addition of new dependencies which aren't yet loaded). We need
    //   to prevent cyclic dependencies forming.
    // - We want atomicity. If any new settings are not valid/alterable, or if a cyclic dependency is
    //   created, nothing should change.
    // - We need to either transfer handles referring to the old service (so that they refer to the new
    //   service), or make them invalid. Or, we alter the original service without creating a new one
    //   (which we can only do if the type doesn't change).

    // Approach:
    // - determine whether we need a new service record or can alter the existing one
    //   (loading a new service always creates a new record; reload only creates a new record if the service
    //   type changes, and otherwise just changes the existing record in-place).
    // - if loading a new service, a dummy record is created to enable easy cyclic dependency detection.
    //   (In other cases cycles must be checked by walking the service graph).
    //   The dummy is replaced with the real service once loading is complete (or is removed if it fails).
    // - process settings from the service file (into a service_settings_wrapper).
    // - check that the new settings are valid (for reload, if the service is running, check if the settings
    //   can be altered).
    // - create the new record and install the new settings in it (or the existing record if not creating a
    //   new record). If doing a reload, check for cycles at this point (there is no dummy record in this
    //   case, so the quick cycle detection is not active).
    // - (if doing a reload, with a new record) move the dependents on the original record to the new record.
    //
    // "Before" dependencies require special handling, as a "before = " specified in a service actually creates
    // a dependency in the specified service on this service. Hence they always require explicit cycle checks
    // (the quick cycle detection method using a dummy service cannot be used). For reloads this is done early,
    // for new services it is done late (after the dummy has been removed).
    //
    // This is all an intricate dance. If failure occurs at any stage, we must restore the previous state.

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

    const auto *argp = strchr(fullname, '@');
    if (!argp) argp = fullname + strlen(fullname);

    auto name = string(fullname, argp);

    auto *argval = *argp ? argp + 1 : nullptr;

    if (reload_svc == nullptr) {
        // First try and find an existing record...
        service_record *existing = find_service(string(fullname), true);
        if (existing != nullptr) {
            if (existing == avoid_circular || existing->check_is_loading()) {
                throw service_cyclic_dependency(name);
            }
            if (existing->get_type() != service_type_t::PLACEHOLDER) {
                return existing;
            }

            // If we found a placeholder, we proceed as for a reload:
            reload_svc = existing;
        }
    }

    service_record *rval = nullptr;
    service_record *dummy = nullptr;

    ifstream service_file;
    string service_filename;

    int fail_load_errno = 0;
    std::string fail_load_path;
    const char *service_dsc_dir = nullptr;

    // Couldn't find one. Have to load it.
    for (auto &service_dir : service_dirs) {
        service_dsc_dir = service_dir.get_dir();
        service_filename = service_dsc_dir;
        if (service_filename.back() != '/') {
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
            throw service_not_found(name);
        }
        else {
            throw service_load_error(name, std::move(fail_load_path), fail_load_errno);
        }
    }

    service_settings_wrapper<prelim_dep> settings;
    service_record *consumer_of_svc = nullptr;

    string line;
    // getline can set failbit if it reaches end-of-file, we don't want an exception in that case. There's
    // no good way to handle an I/O error however, so we'll have exceptions thrown on badbit:
    service_file.exceptions(ios::badbit);

    bool create_new_record = true;

    // any "before" "dependencies" that were loaded
    std::list<service_dep> before_deps;

    auto exception_cleanup = [&]() {
        // Must remove the dummy service record.
        if (dummy != nullptr) {
            remove_service(dummy);
            delete dummy;
        }
        if (create_new_record && rval != nullptr) {
            rval->prepare_for_unload();
            delete rval;
        }
        for (service_dep &before_dep : before_deps) {
            service_record *before_svc = before_dep.get_from();
            if (before_svc->get_type() == service_type_t::PLACEHOLDER) {
                if (before_svc->is_unrefd()) {
                    remove_service(before_svc);
                    delete before_svc;
                }
            }
        }
        // Remove any "after" placeholders that were created while loading but not successfully added as
        // dependencies on the new service (rval). (This requires that settings.depends has been cleared
        // of any dependencies that were successfully added).
        for (prelim_dep &dep : settings.depends) {
            if (dep.to->get_type() == service_type_t::PLACEHOLDER) {
                if (dep.dep_type == dependency_type::AFTER && dep.to->is_unrefd()) {
                    remove_service(dep.to);
                }
            }
        }
        // Remove any placeholder consumed service.
        if (consumer_of_svc != nullptr) {
           if (consumer_of_svc->get_type() == service_type_t::PLACEHOLDER) {
               if (consumer_of_svc->is_unrefd()) {
                   remove_service(consumer_of_svc);
                   delete consumer_of_svc;
               }
           }
        }
    };

    if (reload_svc == nullptr) {
        // Add a placeholder record now to prevent infinite recursion in case of cyclic dependency.
        // We replace this with the real service later (or remove it if we find a configuration error).
        try {
            dummy = new service_record(this, string(fullname), service_record::LOADING_TAG);
            add_service(dummy);
        }
        catch (...) {
            delete dummy; // (no effect if dummy is null)
            dummy = nullptr;
            throw;
        }
    }

    file_input_stack input_stack;
    input_stack.push(service_filename, std::move(service_file));

    try {
        process_service_file(name, input_stack,
                [&](string &line, file_pos_ref fpr, string &setting, setting_op_t op,
                        string_iterator &i, string_iterator &end) -> void {

            auto process_dep_dir_n = [&](std::list<prelim_dep> &deplist, const std::string &waitsford,
                    dependency_type dep_type) -> void {
                process_dep_dir(*this, name.c_str(), service_filename, deplist, waitsford, dep_type, reload_svc);
            };

            auto load_service_n = [&](const string &dep_name) -> service_record * {
                try {
                    return load_service(dep_name.c_str(), reload_svc);
                }
                catch (service_description_exc &sle) {
                    log_service_load_failure(sle);
                    throw service_load_exc(name, "could not load dependency.");
                }
                catch (service_load_exc &sle) {
                    log(loglevel_t::ERROR, "Could not load service ", sle.service_name, ": ",
                            sle.exc_description);
                    throw service_load_exc(name, "could not load dependency.");
                }
            };

            process_service_line(settings, name.c_str(), argval, line, fpr, setting,
                    op, i, end, load_service_n, process_dep_dir_n);
        }, argval);

        auto report_err = [&](const char *msg){
            throw service_load_exc(name, msg);
        };

        environment srv_env;

        // Fill user vars before reading env file
        if (settings.export_passwd_vars) {
            fill_environment_userinfo(settings.run_as_uid, name, srv_env);
        }

        // Set service name in environment if desired
        if (settings.export_service_name) {
            std::string envname = "DINIT_SERVICE=";
            envname += name;
            srv_env.set_var(std::move(envname));
        }

        // This mapping is temporary, for load substitutions. (The environment actually *may* change
        // after load, e.g. through dinitctl setenv, either from the outside or from within services,
        // and so we need to calculate a fresh mapping on each process invocation).
        environment::env_map srv_envmap;

        if (!settings.env_file.empty()) {
            try {
                if (settings.env_file[0] == '/') {
                    // (don't allocate a string if we don't have to)
                    read_env_file(settings.env_file.c_str(), false, srv_env, true);
                }
                else {
                    std::string fullpath = combine_paths(service_dsc_dir, settings.env_file.c_str());
                    read_env_file(fullpath.c_str(), false, srv_env, true);
                }
            } catch (const std::system_error &se) {
                throw service_load_exc(name, std::string("could not load environment file: ") + se.what());
            }
        }

        srv_envmap = srv_env.build(main_env);

        settings.finalise<true>(report_err, srv_envmap, argval);
        auto service_type = settings.service_type;

        if (reload_svc != nullptr) {
            // Make sure settings are able to be changed/are compatible
            create_new_record = check_settings_for_reload(reload_svc, settings);
            // If service current has an output consumer, make sure new settings are compatible
            if (reload_svc->get_log_consumer() != nullptr) {
                if (!value(service_type).is_in(service_type_t::PROCESS, service_type_t::BGPROCESS,
                        service_type_t::SCRIPTED)) {
                    throw service_load_exc(name, "service has output consumer; service type must correspond "
                            "to output-producing service (process, bgprocess, or scripted)");
                }
                if (settings.log_type != log_type_id::PIPE) {
                    throw service_load_exc(name, "service has output consumer; log type must be set "
                            "to 'pipe'");
                }
            }
            // We also don't allow a running service to change its consumed service. This is checked
            // shortly.
        }

        bool have_consumed_svc = !settings.consumer_of_name.empty();
        if (have_consumed_svc) {
            consumer_of_svc = find_service(settings.consumer_of_name.c_str(), true);
        }

        if (reload_svc != nullptr && reload_svc->get_state() != service_state_t::STOPPED) {
            if (value(service_type).is_in(service_type_t::PROCESS, service_type_t::BGPROCESS)) {
                // Since it is not stopped, reload_svc type must be same as service_type, or
                // check_settings_for_reload would have complained. So it's also either PROCESS or
                // BGPROCESS.
                auto *current_consumed = ((process_service *)reload_svc)->get_consumed();
                if (current_consumed != consumer_of_svc) {
                    throw service_load_exc(name, "cannot change consumed service ('consumer-of') when not stopped");
                }
            }
        }

        // Note, we need to be very careful to handle exceptions properly and roll back any changes that
        // we've made before the exception occurred, including destroying any placeholder services that we
        // create, etc.

        if (have_consumed_svc) {
            if (consumer_of_svc == nullptr) {
                consumer_of_svc = new placeholder_service(this, settings.consumer_of_name);
                try {
                    add_service(consumer_of_svc);
                }
                catch (...) {
                    delete consumer_of_svc;
                    consumer_of_svc = nullptr;
                    throw;
                }
            }
            else {
                auto consumed_type = consumer_of_svc->get_type();
                if (!value(consumed_type).is_in(service_type_t::PROCESS, service_type_t::BGPROCESS,
                        service_type_t::SCRIPTED, service_type_t::PLACEHOLDER)) {
                    throw service_load_exc(name, "the 'consumer-of' setting specifies a service of a "
                            "type that does not produce output");
                }
                if (consumed_type != service_type_t::PLACEHOLDER) {
                    base_process_service *bps_consumed = static_cast<base_process_service *>(consumer_of_svc);
                    if (bps_consumed->get_log_mode() != log_type_id::PIPE) {
                        throw service_load_exc(name, "the 'consumer-of' setting specifies a service that "
                                "does not log via a pipe ('log-type = pipe')");
                    }
                }
                service_record *current_consumer = consumer_of_svc->get_log_consumer();
                if (current_consumer != nullptr && current_consumer != reload_svc) {
                    throw service_load_exc(name, "the 'consumer-of' setting specifies a service that "
                            "already has a consumer");
                }
            }
        }

        // If we have "after" constraints, load them now and treat them as regular dependencies. We need
        // to do this now, after the other dependents are loaded, because we might create a placeholder
        // instead (and we don't want to create a placeholder, have it added to the list of dependencies,
        // then load the same service as a real dependency shortly afterwards, which would replace the
        // placeholder but leave a dangling pointer to it in the list).
        for (const std::string &after_ent : settings.after_svcs) {
            service_record *after_svc;
            if (after_ent == name) throw service_cyclic_dependency(name);

            after_svc = find_service(after_ent.c_str(), true);
            if (after_svc != nullptr) {
                if (after_svc->check_is_loading()) {
                    throw service_cyclic_dependency(name);
                }
            }
            if (after_svc == nullptr) {
                after_svc = new placeholder_service(this, after_ent);
                try {
                    add_service(after_svc);
                }
                catch (...) {
                    delete after_svc;
                    throw;
                }
            }

            try {
                settings.depends.emplace_back(after_svc, dependency_type::AFTER);
            }
            catch (...) {
                if (after_svc->is_unrefd()) {
                    remove_service(after_svc);
                    delete after_svc;
                    throw;
                }
            }
        }

        // if we have "before" constraints, check them now.
        for (const std::string &before_ent : settings.before_svcs) {
            service_record *before_svc;
            if (before_ent == name) throw service_cyclic_dependency(name);

            before_svc = find_service(before_ent.c_str(), true);
            if (before_svc != nullptr) {
                check_cycle(settings.depends, before_svc, name);
            }
            else {
                bool before_svc_added = false;
                try {
                    before_svc = new placeholder_service(this, before_ent);
                    add_service(before_svc);
                    before_svc_added = true;
                }
                catch (...) {
                    if (before_svc_added) remove_service(before_svc);
                    delete before_svc;
                    throw;
                }
            }

            before_deps.emplace_back(before_svc, reload_svc, dependency_type::BEFORE);
            // (note, we may need to adjust the to-service if we create a new service record object
            // - this will be done later)
        }

        // We may have capabilities, process them now
        #if SUPPORT_CAPABILITIES
        cap_iab_t cap_iab = nullptr;
        if (!settings.capabilities.empty()) {
            cap_iab = cap_iab_from_text(settings.capabilities.c_str());
            if (!cap_iab) {
                    throw service_load_exc(name, "the 'capabilities' string has an invalid format");
            }
        }
        #endif

        if (service_type == service_type_t::PROCESS) {
            do_env_subst("command", settings.command, settings.command_offsets, srv_envmap, argval);
            do_env_subst("stop-command", settings.stop_command, settings.stop_command_offsets, srv_envmap, argval);
            std::vector<const char *> stop_arg_parts = separate_args(settings.stop_command, settings.stop_command_offsets);
            process_service *rvalps;
            if (create_new_record) {
                if (reload_svc != nullptr) {
                    check_cycle(settings.depends, reload_svc);
                }
                rvalps = new process_service(this, string(fullname), std::move(settings.command),
                        settings.command_offsets, settings.depends);
                settings.depends.clear();
            }
            else {
                rvalps = static_cast<process_service *>(reload_svc);
                update_command_and_dependencies(rvalps, settings, before_deps);
            }
            rval = rvalps;
            // All of the following should be noexcept or must perform rollback on exception
            rvalps->set_stop_command(std::move(settings.stop_command), std::move(stop_arg_parts));
            rvalps->set_working_dir(std::move(settings.working_dir));
            rvalps->set_env_file(std::move(settings.env_file));
            #if SUPPORT_CGROUPS
            rvalps->set_cgroup(std::move(settings.run_in_cgroup));
            #endif
            #if SUPPORT_CAPABILITIES
            rvalps->set_cap(cap_iab, settings.secbits.get());
            #endif
            #ifdef __linux__
            rvalps->set_nice(settings.nice);
            rvalps->set_ionice(settings.ionice);
            rvalps->set_oom_adj(settings.oom_adj);
            #endif
            rvalps->set_rlimits(std::move(settings.rlimits));
            rvalps->set_restart_interval(settings.restart_interval, settings.max_restarts);
            rvalps->set_restart_delay(settings.restart_delay);
            rvalps->set_stop_timeout(settings.stop_timeout);
            rvalps->set_start_timeout(settings.start_timeout);
            rvalps->set_extra_termination_signal(settings.term_signal);
            rvalps->set_run_as_uid_gid(settings.run_as_uid, settings.run_as_gid);
            rvalps->set_notification_fd(settings.readiness_fd);
            rvalps->set_notification_var(std::move(settings.readiness_var));
            rvalps->set_logfile_details(std::move(settings.logfile), settings.logfile_perms,
                    settings.logfile_uid, settings.logfile_gid);
            rvalps->set_log_buf_max(settings.max_log_buffer_sz);
            rvalps->set_log_mode(settings.log_type);
            #if USE_UTMPX
            rvalps->set_utmp_id(settings.inittab_id);
            rvalps->set_utmp_line(settings.inittab_line);
            #endif
        }
        else if (service_type == service_type_t::BGPROCESS) {
            do_env_subst("command", settings.command, settings.command_offsets, srv_envmap, argval);
            do_env_subst("stop-command", settings.stop_command, settings.stop_command_offsets, srv_envmap, argval);
            std::vector<const char *> stop_arg_parts = separate_args(settings.stop_command, settings.stop_command_offsets);
            bgproc_service *rvalps;
            if (create_new_record) {
                if (reload_svc != nullptr) {
                    check_cycle(settings.depends, reload_svc);
                }
                rvalps = new bgproc_service(this, string(fullname), std::move(settings.command),
                        settings.command_offsets, settings.depends);
                settings.depends.clear();
            }
            else {
                rvalps = static_cast<bgproc_service *>(reload_svc);
                update_command_and_dependencies(rvalps, settings, before_deps);
            }
            rval = rvalps;
            // All of the following should be noexcept or must perform rollback on exception
            rvalps->set_stop_command(std::move(settings.stop_command), std::move(stop_arg_parts));
            rvalps->set_working_dir(std::move(settings.working_dir));
            rvalps->set_env_file(std::move(settings.env_file));
            #if SUPPORT_CGROUPS
            rvalps->set_cgroup(std::move(settings.run_in_cgroup));
            #endif
            #if SUPPORT_CAPABILITIES
            rvalps->set_cap(cap_iab, settings.secbits.get());
            #endif
            #ifdef __linux__
            rvalps->set_nice(settings.nice);
            rvalps->set_ionice(settings.ionice);
            rvalps->set_oom_adj(settings.oom_adj);
            #endif
            rvalps->set_rlimits(std::move(settings.rlimits));
            rvalps->set_pid_file(std::move(settings.pid_file));
            rvalps->set_restart_interval(settings.restart_interval, settings.max_restarts);
            rvalps->set_restart_delay(settings.restart_delay);
            rvalps->set_stop_timeout(settings.stop_timeout);
            rvalps->set_start_timeout(settings.start_timeout);
            rvalps->set_extra_termination_signal(settings.term_signal);
            rvalps->set_run_as_uid_gid(settings.run_as_uid, settings.run_as_gid);
            rvalps->set_logfile_details(std::move(settings.logfile), settings.logfile_perms,
                    settings.logfile_uid, settings.logfile_gid);
            rvalps->set_log_buf_max(settings.max_log_buffer_sz);
            rvalps->set_log_mode(settings.log_type);
            settings.onstart_flags.runs_on_console = false;
        }
        else if (service_type == service_type_t::SCRIPTED) {
            do_env_subst("command", settings.command, settings.command_offsets, srv_envmap, argval);
            do_env_subst("stop-command", settings.stop_command, settings.stop_command_offsets, srv_envmap, argval);
            std::vector<const char *> stop_arg_parts = separate_args(settings.stop_command, settings.stop_command_offsets);
            scripted_service *rvalps;
            if (create_new_record) {
                if (reload_svc != nullptr) {
                    check_cycle(settings.depends, reload_svc);
                }
                rvalps = new scripted_service(this, string(fullname), std::move(settings.command),
                        settings.command_offsets, settings.depends);
                settings.depends.clear();
            }
            else {
                rvalps = static_cast<scripted_service *>(reload_svc);
                update_command_and_dependencies(rvalps, settings, before_deps);
            }
            rval = rvalps;
            // All of the following should be noexcept or must perform rollback on exception
            rvalps->set_stop_command(std::move(settings.stop_command), std::move(stop_arg_parts));
            rvalps->set_working_dir(std::move(settings.working_dir));
            rvalps->set_env_file(std::move(settings.env_file));
            #if SUPPORT_CGROUPS
            rvalps->set_cgroup(std::move(settings.run_in_cgroup));
            #endif
            #if SUPPORT_CAPABILITIES
            rvalps->set_cap(cap_iab, settings.secbits.get());
            #endif
            #ifdef __linux__
            rvalps->set_nice(settings.nice);
            rvalps->set_ionice(settings.ionice);
            rvalps->set_oom_adj(settings.oom_adj);
            #endif
            rvalps->set_rlimits(std::move(settings.rlimits));
            rvalps->set_stop_timeout(settings.stop_timeout);
            rvalps->set_start_timeout(settings.start_timeout);
            rvalps->set_extra_termination_signal(settings.term_signal);
            rvalps->set_run_as_uid_gid(settings.run_as_uid, settings.run_as_gid);
            rvalps->set_logfile_details(std::move(settings.logfile), settings.logfile_perms,
                    settings.logfile_uid, settings.logfile_gid);
            rvalps->set_log_buf_max(settings.max_log_buffer_sz);
            rvalps->set_log_mode(settings.log_type);
        }
        else {
            if (create_new_record) {
                if (reload_svc != nullptr) {
                    check_cycle(settings.depends, reload_svc);
                }
                if (service_type == service_type_t::INTERNAL) {
                    rval = new service_record(this, string(fullname), service_type, settings.depends);
                }
                else {
                    /* TRIGGERED */
                    rval = new triggered_service(this, string(fullname), service_type, settings.depends);
                }
                settings.depends.clear();
            }
            else {
                rval = reload_svc;
                update_depenencies(rval, settings, before_deps);
            }
        }

        rval->set_service_dsc_dir(service_dsc_dir);
        rval->set_auto_restart(settings.auto_restart);
        rval->set_smooth_recovery(settings.smooth_recovery);
        rval->set_flags(settings.onstart_flags);
        rval->set_socket_details(std::move(settings.socket_path), settings.socket_perms,
                settings.socket_uid, settings.socket_gid);
        rval->set_chain_to(std::move(settings.chain_to_name));
        rval->set_environment(std::move(srv_env));

        #if SUPPORT_CAPABILITIES
        // in case it was not taken by a service (internal etc.)
        cap_free(cap_iab);
        #endif

        if (create_new_record) {
            // switch dependencies on old record so that they refer to the new record

            // first link in all the (new) "before" dependents (one way at this stage):
            auto &dept_list = rval->get_dependents();
            unsigned added_dependents = 0;
            try {
                for (auto &dept : before_deps) {
                    dept_list.push_back(&dept);
                    ++added_dependents;
                }
            }
            catch (...) {
                // Undo, since the invalid state will cause issues when the new service is disposed of
                while (added_dependents > 0) {
                    dept_list.pop_back();
                    --added_dependents;
                }
                throw;
            }

            // --- Point of no return: mustn't fail from here ---

            // Splice in the new "before" dependencies
            auto i = before_deps.begin();
            decltype(i) j;
            while (i != before_deps.end()) {
                j = std::next(i);
                i->set_to(rval);
                auto &from_deps = i->get_from()->get_dependencies();
                from_deps.splice(from_deps.end(), before_deps, i);
                i = j;
            }

            // Which service are we replacing? (It's either the one that's being reloaded, or a dummy)
            service_record *orig_svc = (reload_svc != nullptr) ? reload_svc : dummy;

            // Complete dependency/dependent transfers.

            // Remove all "before" dependents from the original service (these were created by the
            // original service itself)
            auto &reload_depts = orig_svc->get_dependents();
            for (auto i = reload_depts.begin(); i != reload_depts.end(); ) {
                auto next_i = std::next(i);
                if ((*i)->dep_type == dependency_type::BEFORE) {
                    service_record *before_svc = (*i)->get_from();
                    before_svc->rm_dep(**i);
                    if (before_svc->get_type() == service_type_t::PLACEHOLDER && before_svc->is_unrefd()) {
                        remove_service(before_svc);
                        delete before_svc;
                    }
                }
                i = next_i;
            }

            // Transfer dependents from the original service record to the new record;
            // set links in all dependents on the original to point to the new service:
            auto first_new_before = dept_list.begin();
            dept_list.splice(first_new_before, reload_depts);
            for (auto &dept : dept_list) {
                dept->set_to(rval);
            }

            // Transfer all "before" dependencies (which are actually created by other services) to the
            // new service
            auto &dep_list_prev = orig_svc->get_dependencies();
            auto &dep_list = rval->get_dependencies();
            for (auto i = dep_list_prev.begin(); i != dep_list_prev.end(); ++i) {
                if (i->dep_type == dependency_type::BEFORE) {
                    i->set_from(rval);
                    dep_list.splice(dep_list.end(), dep_list_prev, i++);
                    continue;
                }
            }

            // Transfer any open file descriptors for (log) output to new service record
            if (value(service_type).is_in(service_type_t::PROCESS, service_type_t::BGPROCESS, service_type_t::SCRIPTED)) {
                ((base_process_service *)rval)->set_output_pipe_fds(orig_svc->transfer_output_pipe());
                auto *orig_consumer = orig_svc->get_log_consumer();
                if (orig_consumer != nullptr) {
                    orig_consumer->set_consumer_for(rval);
                }
            }

            // Remove dependent-link for all dependencies from the original:
            orig_svc->prepare_for_unload();

            // Remove consumer-for link from consumed service, if it's changing:
            if (value(orig_svc->get_type()).is_in(service_type_t::PROCESS, service_type_t::BGPROCESS)) {
                process_service *ps_orig = static_cast<process_service *>(orig_svc);
                auto *orig_consumed = ps_orig->get_consumed();
                if (orig_consumed != nullptr && orig_consumed != consumer_of_svc) {
                    orig_consumed->set_log_consumer(nullptr);
                }
            }

            // Finally, replace the old service with the new one:
            auto iter = std::find(records.begin(), records.end(), orig_svc);
            *iter = rval;
            delete orig_svc;
        }

        // Mark as consumer for output of target service (if any)
        if (value(service_type).is_in(service_type_t::PROCESS, service_type_t::BGPROCESS)) {
            process_service *psvc = (process_service *)rval;
            psvc->set_consumer_for(consumer_of_svc);
            if (consumer_of_svc != nullptr) {
                consumer_of_svc->set_log_consumer(psvc);
            }
        }

        return rval;
    }
    catch (service_description_exc &setting_exc)
    {
        exception_cleanup();
        if (setting_exc.service_name.empty()) {
            setting_exc.service_name = name;
        }
        throw;
    }
    catch (std::system_error &sys_err)
    {
        exception_cleanup();
        // don't use sys_err.what() since libstdc++ sometimes includes class names (basic_filebuf):
        std::string msg = input_stack.current_file_name() + ": " + sys_err.code().message();
        throw service_load_exc(name, std::move(msg));
    }
    catch (std::length_error &len_err) {
        // This is pretty much only theoretically possible; we'd normally expect bad_alloc instead.
        exception_cleanup();
        throw service_load_exc(name, "supported length for string/container exceeded");
    }
    catch (...) // (should only be std::bad_alloc or service_load_exc)
    {
        exception_cleanup();
        throw;
    }
}
