#include <load-service.h>
#include <mconfig.h>

// Settings name strings and setting attributes.

namespace dinit_load {

setting_details all_settings[] = {
        // string, identifier, ":", "=", "+="
        {"type",                    setting_id_t::TYPE,                     false,  true,   false},
        {"command",                 setting_id_t::COMMAND,                  false,  true,   true },
        {"working-dir",             setting_id_t::WORKING_DIR,              false,  true,   false},
        {"env-file",                setting_id_t::ENV_FILE,                 false,  true,   false},
        {"socket-listen",           setting_id_t::SOCKET_LISTEN,            false,  true,   false},
        {"socket-permissions",      setting_id_t::SOCKET_PERMISSIONS,       false,  true,   false},
        {"socket-uid",              setting_id_t::SOCKET_UID,               false,  true,   false},
        {"socket-gid",              setting_id_t::SOCKET_GID,               false,  true,   false},
        {"stop-command",            setting_id_t::STOP_COMMAND,             false,  true,   true },
        {"pid-file",                setting_id_t::PID_FILE,                 false,  true,   false},
        {"depends-on",              setting_id_t::DEPENDS_ON,               true,   false,  false},
        {"depends-ms",              setting_id_t::DEPENDS_MS,               true,   false,  false},
        {"waits-for",               setting_id_t::WAITS_FOR,                true,   false,  false},
        {"waits-for.d",             setting_id_t::WAITS_FOR_D,              true,   false,  false},
        {"depends-on.d",            setting_id_t::DEPENDS_ON_D,             true,   false,  false},
        {"depends-ms.d",            setting_id_t::DEPENDS_MS_D,             true,   false,  false},
        {"after",                   setting_id_t::AFTER,                    true,   false,  false},
        {"before",                  setting_id_t::BEFORE,                   true,   false,  false},
        {"logfile",                 setting_id_t::LOGFILE,                  false,  true,   false},
        {"logfile-permissions",     setting_id_t::LOGFILE_PERMISSIONS,      false,  true,   false},
        {"logfile-uid",             setting_id_t::LOGFILE_UID,              false,  true,   false},
        {"logfile-gid",             setting_id_t::LOGFILE_GID,              false,  true,   false},
        {"log-type",                setting_id_t::LOG_TYPE,                 false,  true,   false},

        {"log-buffer-size",         setting_id_t::LOG_BUFFER_SIZE,          false,  true,   false},
        {"consumer-of",             setting_id_t::CONSUMER_OF,              false,  true,   false},
        {"restart",                 setting_id_t::RESTART,                  false,  true,   false},
        {"smooth-recovery",         setting_id_t::SMOOTH_RECOVERY,          false,  true,   false},
        {"options",                 setting_id_t::OPTIONS,                  true,   true,   false}, // TODO +=?
        {"load-options",            setting_id_t::LOAD_OPTIONS,             true,   true,   false}, // +=?
        {"term-signal",             setting_id_t::TERM_SIGNAL,              false,  true,   false},
        {"termsignal",              setting_id_t::TERMSIGNAL,               false,  true,   false}, // (deprecated)
        {"restart-limit-interval",  setting_id_t::RESTART_LIMIT_INTERVAL,   false,  true,   false},
        {"restart-delay",           setting_id_t::RESTART_DELAY,            false,  true,   false},
        {"restart-limit-count",     setting_id_t::RESTART_LIMIT_COUNT,      false,  true,   false},
        {"stop-timeout",            setting_id_t::STOP_TIMEOUT,             false,  true,   false},
        {"start-timeout",           setting_id_t::START_TIMEOUT,            false,  true,   false},
        {"run-as",                  setting_id_t::RUN_AS,                   false,  true,   false},
        {"chain-to",                setting_id_t::CHAIN_TO,                 false,  true,   false},
        {"ready-notification",      setting_id_t::READY_NOTIFICATION,       false,  true,   false},

        // Note: inittab-_ settings are supported even if functionality is not built in
        {"inittab-id",              setting_id_t::INITTAB_ID,               false,  true,   false},
        {"inittab-line",            setting_id_t::INITTAB_LINE,             false,  true,   false},

        {"rlimit-nofile",           setting_id_t::SETTING_RLIMIT_NOFILE,    false,  true,   false},
        {"rlimit-core",             setting_id_t::SETTING_RLIMIT_CORE,      false,  true,   false},
        {"rlimit-data",             setting_id_t::SETTING_RLIMIT_DATA,      false,  true,   false},
        {"rlimit-addrspace",        setting_id_t::SETTING_RLIMIT_ADDRSPACE, false,  true,   false},

#if SUPPORT_CGROUPS
        {"run-in-cgroup",           setting_id_t::RUN_IN_CGROUP,            false,  true,   false},
#endif

#if SUPPORT_CAPABILTIIES
        {"capabilities",            setting_id_t::CAPABILITIES,             false,  true,    true},
        {"secure-bits",             setting_id_t::SECURE_BITS,              false,  true,   false},
#endif

#ifdef __linux__
        {"nice",                    setting_id_t::NICE,                     false,  true,   false},
        {"ionice",                  setting_id_t::IONICE,                   false,  true,   false},
        {"oom-score-adj",           setting_id_t::OOM_SCORE_ADJ,            false,  true,   false},
#endif

        {nullptr,                   setting_id_t::LAST,                     false,  false,  false}
};

} // dinit_load namespace
