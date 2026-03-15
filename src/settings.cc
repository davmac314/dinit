#include <dinit-settings.h>
#include <mconfig.h>

// Settings name strings and setting attributes.

namespace dinit_load {

using namespace dinit_load::setting_str;

setting_details all_settings[] = {
        // string, identifier, ":", "=", "+="
        {str_type,                  setting_id_t::TYPE,                     false,  true,   false},
        {str_command,               setting_id_t::COMMAND,                  false,  true,   true },
        {str_working_dir,           setting_id_t::WORKING_DIR,              false,  true,   false},
        {str_env_file,              setting_id_t::ENV_FILE,                 false,  true,   false},
        {str_socket_listen,         setting_id_t::SOCKET_LISTEN,            false,  true,   false},
        {str_socket_permissions,    setting_id_t::SOCKET_PERMISSIONS,       false,  true,   false},
        {str_socket_uid,            setting_id_t::SOCKET_UID,               false,  true,   false},
        {str_socket_gid,            setting_id_t::SOCKET_GID,               false,  true,   false},
        {str_stop_command,          setting_id_t::STOP_COMMAND,             false,  true,   true },
        {str_pid_file,              setting_id_t::PID_FILE,                 false,  true,   false},
        {str_depends_on,            setting_id_t::DEPENDS_ON,               true,   false,  false},
        {str_depends_ms,            setting_id_t::DEPENDS_MS,               true,   false,  false},
        {str_waits_for,             setting_id_t::WAITS_FOR,                true,   false,  false},
        {str_waits_for_d,           setting_id_t::WAITS_FOR_D,              true,   false,  false},
        {str_depends_on_d,          setting_id_t::DEPENDS_ON_D,             true,   false,  false},
        {str_depends_ms_d,          setting_id_t::DEPENDS_MS_D,             true,   false,  false},
        {str_after,                 setting_id_t::AFTER,                    true,   false,  false},
        {str_before,                setting_id_t::BEFORE,                   true,   false,  false},

        {str_logfile,               setting_id_t::LOGFILE,                  false,  true,   false},
        {str_logfile_permissions,   setting_id_t::LOGFILE_PERMISSIONS,      false,  true,   false},
        {str_logfile_uid,           setting_id_t::LOGFILE_UID,              false,  true,   false},
        {str_logfile_gid,           setting_id_t::LOGFILE_GID,              false,  true,   false},
        {str_log_type,              setting_id_t::LOG_TYPE,                 false,  true,   false},
        {str_log_buffer_size,       setting_id_t::LOG_BUFFER_SIZE,          false,  true,   false},

        {str_consumer_of,           setting_id_t::CONSUMER_OF,              false,  true,   false},
        {str_restart,               setting_id_t::RESTART,                  false,  true,   false},
        {str_smooth_recovery,       setting_id_t::SMOOTH_RECOVERY,          false,  true,   false},
        {str_options,               setting_id_t::OPTIONS,                  true,   true,   false}, // TODO +=?
        {str_load_options,          setting_id_t::LOAD_OPTIONS,             true,   true,   false}, // +=?
        {str_term_signal,           setting_id_t::TERM_SIGNAL,              false,  true,   false},
        {str_termsignal,            setting_id_t::TERMSIGNAL,               false,  true,   false}, // (deprecated)
        {str_restart_limit_interval,setting_id_t::RESTART_LIMIT_INTERVAL,   false,  true,   false},
        {str_restart_delay,         setting_id_t::RESTART_DELAY,            false,  true,   false},
        {str_restart_limit_count,   setting_id_t::RESTART_LIMIT_COUNT,      false,  true,   false},
        {str_stop_timeout,          setting_id_t::STOP_TIMEOUT,             false,  true,   false},
        {str_start_timeout,         setting_id_t::START_TIMEOUT,            false,  true,   false},
        {str_run_as,                setting_id_t::RUN_AS,                   false,  true,   false},
        {str_chain_to,              setting_id_t::CHAIN_TO,                 false,  true,   false},
        {str_ready_notification,    setting_id_t::READY_NOTIFICATION,       false,  true,   false},

        // Note: inittab-_ settings are supported even if functionality is not built in
        {str_inittab_id,            setting_id_t::INITTAB_ID,               false,  true,   false},
        {str_inittab_line,          setting_id_t::INITTAB_LINE,             false,  true,   false},

        {str_rlimit_nofile,         setting_id_t::SETTING_RLIMIT_NOFILE,    false,  true,   false},
        {str_rlimit_core,           setting_id_t::SETTING_RLIMIT_CORE,      false,  true,   false},
        {str_rlimit_data,           setting_id_t::SETTING_RLIMIT_DATA,      false,  true,   false},
        {str_rlimit_addrspace,      setting_id_t::SETTING_RLIMIT_ADDRSPACE, false,  true,   false},

        {str_nice,                  setting_id_t::NICE,                     false,  true,   false},

#if SUPPORT_CGROUPS
        {str_run_in_cgroup,         setting_id_t::RUN_IN_CGROUP,            false,  true,   false},
#endif

#if SUPPORT_CAPABILITIES
        {str_capabilities,          setting_id_t::CAPABILITIES,             false,  true,    true},
        {str_securebits,            setting_id_t::SECUREBITS,               false,  true,    true},
#endif

#if SUPPORT_IOPRIO
        {str_ioprio,                setting_id_t::IOPRIO,                   false,  true,   false},
#endif

#if SUPPORT_OOM_ADJ
        {str_oom_score_adj,         setting_id_t::OOM_SCORE_ADJ,            false,  true,   false},
#endif

        {nullptr,                   setting_id_t::LAST,                     false,  false,  false}
};

} // dinit_load namespace
