// Service settings definitions/declarations.

#ifndef DINIT_SETTINGS_H
#define DINIT_SETTINGS_H 1

#include <mconfig.h>
#include <static-string.h>

namespace dinit_load {
namespace setting_str {

// These are the authoritative definitions of the setting names. Setting names should not appear
// as literals elsewhere. These definitions can be used to concatenate message strings (for error
// messages etc). Standalone setting names should instead be taken from the all_settings array,
// to prevent duplicated strings in the compiled executable binaries.
constexpr auto str_type = cts::literal("type");
constexpr auto str_command = cts::literal("command");
constexpr auto str_working_dir = cts::literal("working-dir");
constexpr auto str_env_file = cts::literal("env-file");
constexpr auto str_socket_listen = cts::literal("socket-listen");
constexpr auto str_socket_permissions = cts::literal("socket-permissions");
constexpr auto str_socket_uid = cts::literal("socket-uid");
constexpr auto str_socket_gid = cts::literal("socket-gid");
constexpr auto str_stop_command = cts::literal("stop-command");
constexpr auto str_pid_file = cts::literal("pid-file");
constexpr auto str_depends_on = cts::literal("depends-on");
constexpr auto str_depends_ms = cts::literal("depends-ms");
constexpr auto str_waits_for = cts::literal("waits-for");
constexpr auto str_waits_for_d = cts::literal("waits-for.d");
constexpr auto str_depends_on_d = cts::literal("depends-on.d");
constexpr auto str_depends_ms_d = cts::literal("depends-ms.d");
constexpr auto str_after = cts::literal("after");
constexpr auto str_before = cts::literal("before");
constexpr auto str_logfile = cts::literal("logfile");
constexpr auto str_logfile_permissions = cts::literal("logfile-permissions");
constexpr auto str_logfile_uid = cts::literal("logfile-uid");
constexpr auto str_logfile_gid = cts::literal("logfile-gid");
constexpr auto str_log_type = cts::literal("log-type");
constexpr auto str_log_buffer_size = cts::literal("log-buffer-size");
constexpr auto str_consumer_of = cts::literal("consumer-of");
constexpr auto str_restart = cts::literal("restart");
constexpr auto str_smooth_recovery = cts::literal("smooth-recovery");
constexpr auto str_options = cts::literal("options");
constexpr auto str_load_options = cts::literal("load-options");
constexpr auto str_term_signal = cts::literal("term-signal");
constexpr auto str_termsignal = cts::literal("termsignal"); // (deprecated)
constexpr auto str_restart_limit_interval = cts::literal("restart-limit-interval");
constexpr auto str_restart_delay = cts::literal("restart-delay");
constexpr auto str_restart_limit_count = cts::literal("restart-limit-count");
constexpr auto str_stop_timeout = cts::literal("stop-timeout");
constexpr auto str_start_timeout = cts::literal("start-timeout");
constexpr auto str_run_as = cts::literal("run-as");
constexpr auto str_chain_to = cts::literal("chain-to");
constexpr auto str_ready_notification = cts::literal("ready-notification");
constexpr auto str_inittab_id = cts::literal("inittab-id");
constexpr auto str_inittab_line = cts::literal("inittab-line");
constexpr auto str_rlimit_nofile = cts::literal("rlimit-nofile");
constexpr auto str_rlimit_core = cts::literal("rlimit-core");
constexpr auto str_rlimit_data = cts::literal("rlimit-data");
constexpr auto str_rlimit_addrspace = cts::literal("rlimit-addrspace");
constexpr auto str_nice = cts::literal("nice");

#if SUPPORT_CGROUPS
constexpr auto str_run_in_cgroup = cts::literal("run-in-cgroup");
#endif

#if SUPPORT_CAPABILITIES
constexpr auto str_capabilities = cts::literal("capabilities");
constexpr auto str_securebits = cts::literal("securebits");
#endif

#if SUPPORT_IOPRIO
constexpr auto str_ioprio = cts::literal("ioprio");
#endif

#if SUPPORT_OOM_ADJ
constexpr auto str_oom_score_adj = cts::literal("oom-score-adj");
#endif

} // namespace dinit_load::setting_str

// The setting ids - these correspond to the index of the setting within the all_settings array.
enum class setting_id_t {
    LAST = -1, // used to indicate end of settings
    TYPE, COMMAND, WORKING_DIR, ENV_FILE, SOCKET_LISTEN, SOCKET_PERMISSIONS, SOCKET_UID,
    SOCKET_GID, STOP_COMMAND, PID_FILE, DEPENDS_ON, DEPENDS_MS, WAITS_FOR, WAITS_FOR_D,
    DEPENDS_ON_D, DEPENDS_MS_D, AFTER, BEFORE, LOGFILE, LOGFILE_PERMISSIONS, LOGFILE_UID,
    LOGFILE_GID, LOG_TYPE, LOG_BUFFER_SIZE, CONSUMER_OF, RESTART, SMOOTH_RECOVERY, OPTIONS,
    LOAD_OPTIONS, TERM_SIGNAL, TERMSIGNAL /* deprecated */, RESTART_LIMIT_INTERVAL, RESTART_DELAY,
    RESTART_LIMIT_COUNT, STOP_TIMEOUT, START_TIMEOUT, RUN_AS, CHAIN_TO, READY_NOTIFICATION,
    INITTAB_ID, INITTAB_LINE, NICE,
    // Prefixed with SETTING_ to avoid name collision with system macros:
    SETTING_RLIMIT_NOFILE, SETTING_RLIMIT_CORE, SETTING_RLIMIT_DATA, SETTING_RLIMIT_ADDRSPACE,
    // Possibly unsupported depending on platform/build options:
#if SUPPORT_CGROUPS
    RUN_IN_CGROUP,
#endif
#if SUPPORT_CAPABILITIES
    CAPABILITIES,
    SECUREBITS,
#endif
#if SUPPORT_IOPRIO
    IOPRIO,
#endif
#if SUPPORT_OOM_ADJ
    OOM_SCORE_ADJ,
#endif
};

struct setting_details {
    const char *setting_str; // (may be null for blank entry)
    setting_id_t setting_id;
    bool supp_colon : 1; // supports ':' assignment
    bool supp_assign : 1; // supports '=' assignment
    bool supp_plus_assign : 1; // supports '+=' assignment operator
    // Note: if '=' not supported but ':' is, '=' maps to ':' for backwards compatibility
};

extern setting_details all_settings[];

}

#endif
