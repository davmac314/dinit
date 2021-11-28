#ifndef DINIT_OPTIONS_PROCESSING_H
#define DINIT_OPTIONS_PROCESSING_H 1

#include <vector>

#include "service-dir.h"

class service_dir_opt
{
    std::vector<const char *> service_dirs;
    static const char *user_home_path;

    service_dir_pathlist service_dir_paths;

public:
    // Get user home (and set user_home_path). (The return may become invalid after
    // changing the environment (HOME variable) or using the getpwuid() function).
    static const char * get_user_home();

    void set_specified_service_dir(const char *specified_dir)
    {
        service_dirs.push_back(specified_dir);
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
