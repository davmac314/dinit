#ifndef DINIT_OPTIONS_PROCESSING_H
#define DINIT_OPTIONS_PROCESSING_H 1

#include <vector>

// A service directory entry, tracking the directory as a nul-terminated string, which may either
// be static or dynamically allocated (via new char[...]).
class dir_entry
{
    const char *dir;
    bool dir_dyn_allocd;  // dynamically allocated?

    public:
    dir_entry(const char *dir_p, bool dir_dyn_allocd_p) :
        dir(dir_p), dir_dyn_allocd(dir_dyn_allocd_p)
    { }

    dir_entry(dir_entry &&other)
    {
        dir = other.dir;
        dir_dyn_allocd = other.dir_dyn_allocd;
        other.dir_dyn_allocd = false;
    }

    dir_entry(const dir_entry &other) = delete;

    ~dir_entry()
    {
        if (dir_dyn_allocd) {
            delete[] dir;
        }
    }

    const char *get_dir() const
    {
        return dir;
    }
};

// TODO this is close to just being a vector; make it so?
class service_dir_pathlist
{
    std::vector<dir_entry> service_dirs; // directories containing service descriptions

public:
    service_dir_pathlist() { }

    service_dir_pathlist(const char *service_dir_p, bool dyn_allocd = false)
    {
        service_dirs.emplace_back(service_dir_p, dyn_allocd);
    }

    void add_dir(const char *service_dir_p, bool dyn_allocd = false)
    {
        service_dirs.emplace_back(service_dir_p, dyn_allocd);
    }

    size_t size() const
    {
        return service_dirs.size();
    }

    dir_entry &operator[](size_t index)
    {
        return service_dirs[index];
    }

    std::vector<dir_entry>::const_iterator begin() const
    {
        return service_dirs.begin();
    }

    std::vector<dir_entry>::const_iterator end() const
    {
        return service_dirs.end();
    }
};

class service_dir_opt
{
    const char *service_dir = nullptr;;
    bool service_dir_dynamic = false;

    static const char *user_home_path;

    service_dir_pathlist service_dir_paths;

public:
    // Get user home (and set user_home_path). (The return may become invalid after
    // changing the environment (HOME variable) or using the getpwuid() function).
    static const char * get_user_home();

    void set_specified_service_dir(const char *specified_dir)
    {
        service_dir = specified_dir;
    }

    // Build the set of service directory paths, as per configuration specified thus far. This might be a
    // single specified path, or a set of default paths.
    void build_paths(bool am_system_init);

    // Get the service directory paths as a (mutable) collection. Call only after calling build_paths().
    service_dir_pathlist &get_paths()
    {
        return service_dir_paths;
    }
};

#endif
