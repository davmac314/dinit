#include <vector>

#include <cstring>
#include <cstdlib>

#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>

#include "options-processing.h"

const char *service_dir_opt::user_home_path = nullptr;

const char *service_dir_opt::get_user_home()
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
    if (service_dirs.empty()) {
        bool home_service_dir_set = false;

        /* service directory name */
        if (!am_system_init) {
            const char * xdg_config_home = getenv("XDG_CONFIG_HOME");
            if (xdg_config_home != nullptr && strlen(xdg_config_home) != 0) {
                size_t xdg_config_home_len = strlen(xdg_config_home);
                size_t dinit_d_len = strlen("/dinit.d");
                size_t full_len = xdg_config_home_len + dinit_d_len + 1;
                char *service_dir_w = new char[full_len];
                std::memcpy(service_dir_w, xdg_config_home, xdg_config_home_len);
                std::memcpy(service_dir_w + xdg_config_home_len, "/dinit.d", dinit_d_len);
                service_dir_w[full_len - 1] = 0;

                service_dir_paths.emplace_back(service_dir_w, /*dyn_allocd=*/true);
                home_service_dir_set = true;
            }

            const char * user_home = get_user_home();
            if (user_home != nullptr) {
                size_t user_home_len = strlen(user_home);
                size_t dinit_d_len = strlen("/.config/dinit.d");
                size_t full_len = user_home_len + dinit_d_len + 1;
                char *service_dir_w = new char[full_len];
                std::memcpy(service_dir_w, user_home, user_home_len);
                std::memcpy(service_dir_w + user_home_len, "/.config/dinit.d", dinit_d_len);
                service_dir_w[full_len - 1] = 0;

                bool user_home_path_already_added = false;
                for (auto &path : service_dir_paths) {
                    if (strcmp(path.get_dir(), service_dir_w) == 0) {
                        user_home_path_already_added = true;
                        break;
                    }
                }

                if (!user_home_path_already_added) {
                    service_dir_paths.emplace_back(service_dir_w, /*dyn_allocd=*/true);
                    home_service_dir_set = true;
                } else {
                    delete[] service_dir_w;
                }
            }
        }

        if (!home_service_dir_set) {
            service_dir_paths.emplace_back("/etc/dinit.d", /*dyn_allocd=*/false);
            service_dir_paths.emplace_back("/run/dinit.d", false);
            service_dir_paths.emplace_back("/usr/local/lib/dinit.d", false);
            service_dir_paths.emplace_back("/lib/dinit.d", false);
        }
    }
    else {
        for (const char *dir : service_dirs) {
            service_dir_paths.emplace_back(dir, /*dyn_allocd=*/false);
        }
    }
}
