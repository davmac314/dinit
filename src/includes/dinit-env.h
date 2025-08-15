#ifndef DINIT_ENV_H_INCLUDED
#define DINIT_ENV_H_INCLUDED 1

#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <string>

#include <baseproc-sys.h>
#include <dinit-util.h>
#include <dinit-iostream.h>


class environment;
extern environment main_env;

// Read and set environment variables (encapsulated in an 'environment' object) from a file.
// File contains "VAR=VALUE" assignments (line by line) and "!" meta-commands.
// Parameters:
//   env_file_path - the path to the environment file to process
//   resolve_fd - directory to resolve path against; may be AT_FDCWD
//   log_warnings - whether warnings should be logged (eg about invalid embedded commands)
//   env - the environment to modify
//   throw_on_open_failure - whether to throw an exception on failure to open the specified
//                           file. If false, returns instead (without logging failure).
// Throws:
//   std::bad_alloc, std::system_error
//
// This function is available within dinit only, not utilities, but see read_env_file_inline
// below.
void read_env_file(const char *file, int resolve_fd, bool log_warnings, environment &env,
        bool throw_on_open_failure);

// Note that our sets (defined as part of environment class below) allow searching based on a name
// only (string_view) or "NAME=VALUE" assignment pair (std::string). It is important to always
// search using the correct type.

// Hash environment variable name only (not including value)
struct hash_env_name
{
    size_t operator()(const std::string &s) const
    {
        size_t eq_pos = s.find('=');
        return hash(string_view(s.data(), eq_pos));
    }

    size_t operator()(string_view s) const
    {
        return hash(s);
    }
};

// Comparison predicate for environment variables, checking name only
struct env_equal_name
{
    bool operator()(const std::string &a, const std::string &b) const noexcept
    {
        size_t a_eq_pos = a.find('=');
        size_t b_eq_pos = b.find('=');
        return string_view(a.data(), a_eq_pos) == string_view(b.data(), b_eq_pos);
    }

    // For comparison between a string and string_view, we can assume the view is just the name

    bool operator()(const std::string &a, string_view b) const noexcept
    {
        size_t a_eq_pos = a.find('=');
        return string_view(a.data(), a_eq_pos) == b;
    }

    bool operator()(string_view a, const std::string &b) const noexcept
    {
        return operator()(b, a);
    }
};

// Interface for listening to environment
class env_listener
{
    public:

    virtual void environ_event(environment *env, std::string const &name_and_val, bool overridden) noexcept = 0;
};

class environment
{
    // Whether to keep the parent environment, as a whole. Individual variables can still be
    // modified or unset.
    bool keep_parent_env = true;

    // TODO keep natural order somehow
    using env_set = dinit_unordered_set<std::string, hash_env_name, env_equal_name>;
    using env_names = dinit_unordered_set<std::string,hash_sv,dinit_equal_to>;

    // Which specific variables to keep from parent environment (if keep_parent_env is false)
    env_names import_from_parent;

    // Which specific variables to remove (if keep_parent_env is true)
    env_names undefine;

    // set of variables modified or set:
    env_set set_vars;

    std::unordered_set<env_listener *> listeners;

    string_view find_var_name(string_view var)
    {
        const char *var_ch;
        for (var_ch = var.data(); *var_ch != '='; ++var_ch) {
            if (*var_ch == '\0') break;
        }
        return {var.data(), (size_t)(var_ch - var.data())};
    }

    void notify_listeners(std::string const &var_and_val, bool overridden)
    {
        for (auto l : listeners) {
            l->environ_event(this, var_and_val, overridden);
        }
    }

public:

    environment() = default;
    environment(environment &&other) noexcept = default;
    environment &operator=(environment &&other) noexcept = default;

    // force move semantics
    environment(const environment &other) = delete;
    environment &operator=(const environment &other) = delete;

    struct env_map {
        // *non-owning* list of environment variables, i.e. list as suitable for exec, including
        // null at end of list
        std::vector<const char *> env_list;

        // map of variable name (via string_view) to its index in env_list
        std::unordered_map<string_view, unsigned, hash_sv> var_map;

        const char *lookup(string_view sv) const {
            auto it = var_map.find(sv);
            if (it != var_map.end()) {
                return env_list[it->second] + sv.size() + 1;
            }
            return nullptr;
        }
    };

    // return environment variable in form NAME=VALUE. Assumes that the real environment is the parent.
    string_view get(const std::string &name) const
    {
        auto it = set_vars.find(string_view(name));
        if (it != set_vars.end()) {
            return *it;
        }

        if (!keep_parent_env && !import_from_parent.contains(name)) {
            return {};
        }

        const char *val = bp_sys::getenv(name.c_str());
        if (val == nullptr) {
            return {};
        }
        const char *name_and_val = val - (name.length() + 1);
        size_t val_len = strlen(val);
        return {name_and_val, name.length() + 1 + val_len};
    }

    // Build a mapping excluding named variables (only called if the parent is the real environment).
    // Note that the return is non-owning, i.e. the variable values are backed by the environment object
    // and their lifetime is bounded to it.
    env_map build(const env_names &exclude) const
    {
        env_map mapping;

        if (keep_parent_env) {
            // import all from parent, excluding our own undefines + the exclude set
            if (bp_sys::environ != nullptr) {
                unsigned pos = 0;
                for (char **env = bp_sys::environ; *env != nullptr; ++env) {
                    // find '='
                    char *var_ch;
                    for (var_ch = *env; *var_ch != '='; ++var_ch) {
                        if (*var_ch == '\0') break;
                    }
                    // if this variable doesn't contain '=', ignore it
                    if (*var_ch == '\0') continue;
                    string_view name_view {*env, (size_t)(var_ch - *env)};

                    if (undefine.contains(name_view) || exclude.contains(name_view)) {
                        goto next_env_var;
                    }

                    mapping.env_list.push_back(*env);
                    mapping.var_map.insert({name_view, pos});
                    ++pos;

                    next_env_var: ;
                }
            }
        }
        else {
            // import specific items from parent
            for (const std::string &import_name : import_from_parent) {
                // POSIX allows that getenv return its result in a static, per-thread buffer. Since this is
                // ridiculous, we'll assume that all implementations do the sane thing of simply returning
                // a pointer to the value part of the NAME=VALUE string in the actual environment array:
                const char *value = getenv(import_name.c_str());
                if (value == nullptr) continue;
                // go back through the name and the '=' to the beginning of NAME=VALUE:
                const char *name_and_val = value - (import_name.length() + 1);
                mapping.var_map.insert({import_name, mapping.env_list.size()});
                mapping.env_list.push_back(name_and_val);
            }
        }

        // add our own (excluding exclude set)
        for (const std::string &set_var : set_vars) {
            size_t eq_pos = set_var.find('=');
            string_view set_var_name = string_view(set_var.data(), eq_pos);

            if (!exclude.contains(set_var_name)) {
                auto iter = mapping.var_map.find(set_var_name);
                if (iter != mapping.var_map.end()) {
                    unsigned pos = iter->second;
                    mapping.env_list[pos] = set_var.c_str();
                }
                else {
                    mapping.var_map[set_var_name] = mapping.env_list.size();
                    mapping.env_list.push_back(set_var.c_str());
                }
            }
        }
        mapping.env_list.push_back(nullptr);

        return mapping;
    }

    env_map build(const environment &parent_env) const
    {
        env_map mapping;

        // first build base: variables from the parent(s) excluding those specifically excluded
        if (keep_parent_env) {
            mapping = parent_env.build(undefine);
            // remove final null entry:
            mapping.env_list.resize(mapping.env_list.size() - 1);
        }
        else {
            // import only those specifically chosen
            for (const std::string &import_name : import_from_parent) {
                string_view name_and_val = parent_env.get(import_name);
                if (name_and_val.empty()) continue;
                mapping.var_map.insert({import_name, mapping.env_list.size()});
                mapping.env_list.push_back(name_and_val.data());
            }
        }

        // add our own (excluding exclude set)
        for (const std::string &set_var : set_vars) {
            size_t eq_pos = set_var.find('=');
            string_view set_var_name = string_view(set_var.data(), eq_pos);
            auto iter = mapping.var_map.find(set_var_name);
            if (iter != mapping.var_map.end()) {
                unsigned pos = iter->second;
                mapping.env_list[pos] = set_var.c_str();
            }
            else {
                mapping.var_map[set_var_name] = mapping.env_list.size();
                mapping.env_list.push_back(set_var.c_str());
            }
        }
        mapping.env_list.push_back(nullptr);

        return mapping;
    }

    // build a mapping, where parent is the real environment
    env_map build() const
    {
        return build(env_names());
    }

    void set_var(std::string &&var_and_val, bool notify = false)
    {
        string_view var_name = find_var_name(var_and_val);

        import_from_parent.erase(var_name);
        auto n_removed = undefine.erase(var_name);

        bool in_sysenv = false;
        if (notify && n_removed == 0) {
            // workaround to avoid an allocation; when notify is true,
            // we know for sure that the value is sanitized from before
            //
            // we don't check this when the variable was in undefine,
            // as that means we were undefining it explicitly
            auto name_size = var_name.size();
            var_and_val[name_size] = '\0';
            in_sysenv = !!bp_sys::getenv(var_and_val.c_str());
            var_and_val[name_size] = '=';
        }

        auto insert_result = set_vars.insert(std::move(var_and_val));
        // if the variable was in sys environment, it is always overridden
        bool overridden = in_sysenv;
        if (!insert_result.second) {
            *insert_result.first = var_and_val;
            overridden = true;
        }

        if (notify) {
            notify_listeners(*insert_result.first, overridden);
        }
    }

    void import_parent_var(std::string &&var_name)
    {
        undefine.erase(var_name);
        set_vars.erase(string_view(var_name));
        if (!keep_parent_env) {
            import_from_parent.insert(std::move(var_name));
        }
    }

    void undefine_var(std::string &&var_name, bool notify = false)
    {
        import_from_parent.erase(var_name);

        auto n_removed = set_vars.erase(string_view(var_name));
        bool was_set = n_removed > 0;
        if (notify && !was_set) {
            // also track if we're undefining it from system environment
            was_set = !!bp_sys::getenv(var_name.c_str());
        }
        if (keep_parent_env) {
            auto insert_result = undefine.insert(std::move(var_name));
            if (notify) {
                notify_listeners(*insert_result.first, was_set);
            }
        }
        else if (notify) {
            notify_listeners(var_name, was_set);
        }
    }

    void clear_no_inherit()
    {
        keep_parent_env = false;
        import_from_parent.clear();
        undefine.clear();
        set_vars.clear();
    }

    void add_listener(env_listener * listener)
    {
        listeners.insert(listener);
    }

    void remove_listener(env_listener * listener) noexcept
    {
        listeners.erase(listener);
    }
};

// Read and set environment variables (encapsulated in an 'environment' object) from a file.
// File contains "VAR=VALUE" assignments (line by line) and "!" meta-commands.
// Parameters:
//   env_file_path - the path to the environment file to process
//   resolve_fd - directory to resolve path against; may be AT_FDCWD
//   log_warnings - whether warnings should be logged (eg about invalid embedded commands)
//   env - the environment to modify
//   throw_on_open_failure - whether to throw an exception on failure to open the specified
//                           file. If false, returns instead (without logging failure).
//   log_inv_setting - function/functor to log invalid lines
//   log_bad_cmd - function/functor to log bad meta commands
// Throws:
//   std::bad_alloc, std::system_error
template <typename LOG_INV_SETTING, typename LOG_BAD_COMMAND>
inline void read_env_file_inline(const char *env_file_path, int resolve_fd, bool log_warnings,
        environment &env, bool throw_on_open_failure, LOG_INV_SETTING &log_inv_setting,
        LOG_BAD_COMMAND &log_bad_cmd)
{
    int env_file_fd = bp_sys::openat(resolve_fd, env_file_path, O_RDONLY);
    if (env_file_fd == -1) {
        if (throw_on_open_failure) {
            throw std::system_error(errno, std::generic_category());
        }
        return;
    }

    dio::istream env_file(env_file_fd);
    env_file.throw_exception_on(dio::buffer_fail_bit);

    auto &clocale = std::locale::classic();
    std::string line;
    int linenum = 0;

    while (!env_file.eof()) {
        env_file.get_line_until_eof(line);
        linenum++;
        auto lpos = line.begin();
        auto lend = line.end();
        while (lpos != lend && std::isspace(*lpos, clocale)) {
            ++lpos;
        }

        if (lpos == lend) continue; // empty line

        if (*lpos == '#') {
            continue;
        }
        if (*lpos == '=') {
            if (log_warnings) {
                log_inv_setting(linenum);
            }
            continue;
        }

        // "!COMMAND" form.
        if (*lpos == '!') {
            ++lpos; // lpos = first char of command
            auto epos = lpos;
            do {
                ++epos;
            } while(epos != lend && !std::isspace(*epos, clocale));

            const char *lpos_p = line.data() + (lpos - line.begin());
            string_view cmd {lpos_p, (size_t)(epos - lpos)};

            std::vector<string_view> cmd_args;
            while (epos != lend) {
                // skip whitespace
                while (std::isspace(*epos, clocale)) {
                    ++epos;
                    if (epos == lend) goto process_cmd; // no more args
                }
                // all non-ws is argument until next ws
                const char *arg_begin = line.c_str() + (epos - line.begin());
                auto arg_begin_i = epos;
                while (epos != lend && !std::isspace(*epos)) {
                    ++epos;
                }
                cmd_args.push_back(string_view {arg_begin, (size_t)(epos - arg_begin_i)});
            }

            process_cmd:

            if (cmd == "clear") {
                env.clear_no_inherit();
            }
            else if (cmd == "unset") {
                for (string_view arg : cmd_args) {
                    env.undefine_var(std::string(arg.data(), arg.length()));
                }
            }
            else if (cmd == "import") {
                for (string_view arg : cmd_args) {
                    env.import_parent_var(std::string(arg.data(), arg.length()));
                }
            }
            else if (log_warnings) {
                log_bad_cmd(linenum);
            }

            continue;
        }

        // ENV=VALUE form.
        auto name_begin = lpos++;
        // skip until '=' or whitespace:
        while (lpos != lend && *lpos != '=' && !std::isspace(*lpos, clocale)) ++lpos;
        auto name_end = lpos;
        //  skip whitespace:
        while (lpos != lend && std::isspace(*lpos, clocale)) ++lpos;
        if (lpos == lend || *lpos != '=') {
            if (log_warnings) {
                log_inv_setting(linenum);
            }
            continue;
        }

        ++lpos;
        auto val_begin = lpos;
        auto val_end = lend;

        if (val_begin != (name_end + 1) || name_begin != line.begin()) {
            // there are spaces that we need to eliminate
            std::string name_and_val;
            name_and_val.reserve((name_end - name_begin) + 1 + (val_end - val_begin));
            name_and_val = line.substr(name_begin - line.begin(), name_end - name_begin);
            name_and_val.append(1, '=');
            name_and_val.append(val_begin, val_end);
            env.set_var(std::move(name_and_val));
        }
        else {
            line.shrink_to_fit();
            env.set_var(std::move(line));
        }
    }
}

#endif /* DINIT_ENV_H_INCLUDED */
