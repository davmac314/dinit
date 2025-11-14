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
            size_t xdg_config_home_len;
            if (xdg_config_home != nullptr && *xdg_config_home != '\0') {
                xdg_config_home_len = strlen(xdg_config_home);
                if (xdg_config_home[xdg_config_home_len - 1] == '/') {
                    // strip any trailing slash
                    --xdg_config_home_len;
                }
                size_t dinit_d_len = strlen("/dinit.d");
                size_t full_len = xdg_config_home_len + dinit_d_len + 1;
                char *service_dir_w = new char[full_len];
                std::memcpy(service_dir_w, xdg_config_home, xdg_config_home_len);
                std::memcpy(service_dir_w + xdg_config_home_len, "/dinit.d", dinit_d_len);
                service_dir_w[full_len - 1] = 0;

                service_dir_paths.emplace_back(service_dir_w, true);
                home_service_dir_set = true;
            }

            const char * user_home = get_user_home();
            if (user_home != nullptr && *user_home != '\0') {
                size_t user_home_len = strlen(user_home);
                if (xdg_config_home != nullptr && *xdg_config_home != '\0'
                        && xdg_config_home_len == (user_home_len + strlen("/.config"))
                        && strncmp(xdg_config_home, user_home, user_home_len) == 0
                        && strcmp(xdg_config_home + user_home_len, "/.config") == 0) {
                    // don't add duplicate directories:
                    goto done_user_home;
                }

                size_t dinit_d_len = strlen("/.config/dinit.d");
                size_t full_len = user_home_len + dinit_d_len + 1;
                char *service_dir_w = new char[full_len];
                std::memcpy(service_dir_w, user_home, user_home_len);
                std::memcpy(service_dir_w + user_home_len, "/.config/dinit.d", dinit_d_len);
                service_dir_w[full_len - 1] = 0;

                service_dir_paths.emplace_back(service_dir_w, true);
                home_service_dir_set = true;
            }
        }

        done_user_home:

        if (home_service_dir_set) {
            service_dir_paths.emplace_back("/etc/dinit.d/user", false);
            service_dir_paths.emplace_back("/usr/lib/dinit.d/user", false);
            service_dir_paths.emplace_back("/usr/local/lib/dinit.d/user", false);
        }
        else {
            service_dir_paths.emplace_back("/etc/dinit.d", false);
            service_dir_paths.emplace_back("/run/dinit.d", false);
            service_dir_paths.emplace_back("/usr/local/lib/dinit.d", false);
            service_dir_paths.emplace_back("/lib/dinit.d", false);
        }
    }
    else {
        for (const char *dir : service_dirs) {
            service_dir_paths.emplace_back(dir, false);
        }
    }
}
