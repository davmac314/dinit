#include <vector>

#include <cstring>
#include <cstdlib>

#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>

#include "options-processing.h"

const char *service_dir_opt::user_home_path = nullptr;

const char * service_dir_opt::get_user_home()
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

void service_dir_opt::build_paths(bool am_system_init)
{
    /* service directory name */
    if (service_dir == nullptr && ! am_system_init) {
        const char * user_home = get_user_home();
        if (user_home != nullptr) {
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

    bool add_all_service_dirs = false;
    if (service_dir == nullptr) {
        service_dir = "/etc/dinit.d";
        add_all_service_dirs = true;
    }

    service_dir_paths.add_dir(service_dir, service_dir_dynamic);
    if (add_all_service_dirs) {
        service_dir_paths.add_dir("/usr/local/lib/dinit.d", false);
        service_dir_paths.add_dir("/lib/dinit.d", false);
    }
}
