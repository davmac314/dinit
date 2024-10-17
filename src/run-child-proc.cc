#include <cstdlib>
#include <cstring>
#include <cstdio>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

#include "service.h"
#include "proc-service.h"
#include "mconfig.h"

#ifdef SUPPORT_CGROUPS
extern std::string cgroups_path;
extern bool have_cgroups_path;
#endif

#if USE_INITGROUPS
#include <grp.h>
#endif

extern sigset_t orig_signal_mask;

// Move an fd, if necessary, to another fd. The original destination fd will be closed.
// if fd is specified as -1, returns -1 immediately. Returns 0 on success.
static int move_fd(int fd, int dest)
{
    if (fd == -1) return -1;
    if (fd == dest) return 0;

    if (dup2(fd, dest) == -1) {
        return -1;
    }

    close(fd);
    return 0;
}

// Move a file descriptor to another, freeing up the original descriptor so that it can be used
// for some reserved purpose.
static int move_reserved_fd(int *fd, int min_fd)
{
    int new_fd = fcntl(*fd, F_DUPFD_CLOEXEC, min_fd);
    if (new_fd != -1) {
        close(*fd);
        *fd = new_fd;
    }
    return new_fd;
}

void base_process_service::run_child_proc(run_proc_params params) noexcept
{
    // Child process. Must not risk throwing any uncaught exception from here until exit().
    const char * const *args = params.args;
    const char *working_dir = params.working_dir;
    const char *logfile = params.logfile;
    bool on_console = params.on_console;
    int wpipefd = params.wpipefd;
    int csfd = params.csfd;
    int notify_fd = params.notify_fd;
    int force_notify_fd = params.force_notify_fd;
    const char *notify_var = params.notify_var;
    uid_t uid = params.uid;
    gid_t gid = params.gid;
    const std::vector<service_rlimits> &rlimits = params.rlimits;
    int output_fd = params.output_fd;

    // If the console already has a session leader, presumably it is us. On the other hand
    // if it has no session leader, and we don't create one, then control inputs such as
    // ^C will have no effect. (We check here, before we potentially re-assign STDIN).
    bool do_set_ctty = on_console && (tcgetsid(0) == -1);

    // For the moment, we'll block all signals, since apparently even dup() can be interrupted
    // (thanks, POSIX...).
    sigset_t sigall_set;
    sigfillset(&sigall_set);
    sigprocmask(SIG_SETMASK, &sigall_set, nullptr);

    constexpr int bufsz = 11 + ((CHAR_BIT * sizeof(pid_t) + 2) / 3) + 1;
    // "LISTEN_PID=" - 11 characters; the expression above gives a conservative estimate
    // on the maxiumum number of bytes required for LISTEN=nnn, including nul terminator,
    // where nnn is a pid_t in decimal (i.e. one decimal digit is worth just over 3 bits).
    char nbuf[bufsz];

    // "DINIT_CS_FD=" - 12 bytes. (we -1 from sizeof(int) in account of sign bit).
    constexpr int csenvbufsz = 12 + ((CHAR_BIT * sizeof(int) - 1 + 2) / 3) + 1;
    char csenvbuf[csenvbufsz];

    environment::env_map proc_env_map;

    run_proc_err err;
    err.stage = exec_stage::ARRANGE_FDS;

    // We need to shuffle various file descriptors around to get them in the right places.

    int minfd = (socket_fd == -1) ? 3 : 4;

    // If input_fd is set, deal with it now (move it to STDIN) so we can throw away that file descriptor
    if (params.input_fd != -1) {
        if (move_fd(params.input_fd, STDIN_FILENO) != 0) goto failure_out;
    }

    if (force_notify_fd != -1) {
        // Move wpipefd/csfd/socket_fd to another fd if necessary:
        if (wpipefd == force_notify_fd) {
            if (move_reserved_fd(&wpipefd, minfd) == -1) {
                goto failure_out;
            }
        }
        if (csfd == force_notify_fd) {
            if (move_reserved_fd(&csfd, minfd) == -1) {
                goto failure_out;
            }
        }
        if (socket_fd == force_notify_fd) {
            // Note that we might move this again later
            if (move_reserved_fd(&socket_fd, 0) == -1) {
                goto failure_out;
            }
        }

        // allocate the forced notification fd:
        if (notify_fd != force_notify_fd) {
            if (dup2(notify_fd, force_notify_fd) == -1) {
                goto failure_out;
            }
            close(notify_fd);
            notify_fd = force_notify_fd;
        }
    }

    // Make sure we have the fds for stdin/out/err (and pre-opened socket) available:
    if (wpipefd < minfd) {
        wpipefd = fcntl(wpipefd, F_DUPFD_CLOEXEC, minfd);
        if (wpipefd == -1) goto failure_out;
    }

    if (csfd != -1 && csfd < minfd) {
        csfd = fcntl(csfd, F_DUPFD, minfd);
        if (csfd == -1) goto failure_out;
    }

    if (notify_fd < minfd && notify_fd != force_notify_fd) {
        notify_fd = fcntl(notify_fd, F_DUPFD, minfd);
        if (notify_fd == -1) goto failure_out;
    }

    try {
        // Set up notify-fd variable:
        if (notify_var != nullptr && *notify_var != 0) {
            err.stage = exec_stage::SET_NOTIFYFD_VAR;
            // We need to do an allocation: the variable name length, '=', and space for the value,
            // and nul terminator:
            if (!strchr(notify_var, '=')) {
                int notify_var_len = strlen(notify_var);
                int req_sz = notify_var_len + ((CHAR_BIT * sizeof(int) - 1 + 2) / 3) + 1;
                char * var_str = (char *) malloc(req_sz);
                if (var_str == nullptr) goto failure_out;
                snprintf(var_str, req_sz, "%s=%d", notify_var, notify_fd);
                service_env.set_var(var_str);
            } else {
                service_env.set_var(notify_var);
            }
        }

        // Set up Systemd-style socket activation:
        if (socket_fd != -1) {
            err.stage = exec_stage::SETUP_ACTIVATION_SOCKET;

            // If we passing a pre-opened socket, it has to be fd number 3. (Thanks, Systemd).
            if (dup2(socket_fd, 3) == -1) goto failure_out;
            if (socket_fd != 3) close(socket_fd);

            service_env.set_var("LISTEN_FDS=1");
            snprintf(nbuf, bufsz, "LISTEN_PID=%jd", static_cast<intmax_t>(getpid()));
            service_env.set_var(nbuf);
        }

        if (csfd != -1) {
            err.stage = exec_stage::SETUP_CONTROL_SOCKET;
            snprintf(csenvbuf, csenvbufsz, "DINIT_CS_FD=%d", csfd);
            service_env.set_var(csenvbuf);
        }

        // We'll re-use READ_ENV_FILE stage here; it's accurate enough.
        err.stage = exec_stage::READ_ENV_FILE;
        proc_env_map = service_env.build(main_env);
    }
    catch (std::system_error &sys_err) {
        errno = sys_err.code().value();
        goto failure_out;
    }
    catch (std::bad_alloc &) {
        errno = ENOMEM;
        goto failure_out;
    }

    if (working_dir != nullptr && *working_dir != 0) {
        err.stage = exec_stage::CHDIR;
        if (chdir(working_dir) == -1) {
            goto failure_out;
        }
    }

    if (!on_console) {
        // Re-set stdin (possibly), stdout, stderr
        int begin = (params.input_fd == -1) ? 0 : 1;
        for (int i = begin; i < 3; i++) {
            if (i != force_notify_fd) close(i);
        }

        err.stage = exec_stage::SETUP_STDINOUTERR;
        // Either: notify_fd == 0, i.e. the notification fd is STDIN (bad form, but we'll allow it)
        //         and in that case it's already open
        //     or: params.input_fd != 1, i.e. our STDIN is already open
        //     or: we most open STDIN ourself (from /dev/null)
        if (notify_fd != 0 && params.input_fd == -1 && move_fd(open("/dev/null", O_RDONLY), 0) != 0) {
            goto failure_out;
        }

        // stdin = 0. That's what we should have; proceed with opening stdout and stderr. We have to
        // take care not to clobber the notify_fd.
        if (output_fd == -1) {
            output_fd = open(logfile, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
            if (output_fd == -1) {
                // On failure to open a log file, change the error stage to give more precise
                // information:
                if (this->log_type == log_type_id::LOGFILE) err.stage = exec_stage::OPEN_LOGFILE;
                goto failure_out;
            }
            // Set permission of logfile if present
            // if log type is NONE, we don't want to change ownership/permissions of /dev/null!
            if (this->log_type == log_type_id::LOGFILE) {
                if (fchown(output_fd, logfile_uid, logfile_gid) == -1) goto failure_out;
                if (fchmod(output_fd, logfile_perms) == -1) goto failure_out;
            }
        }
        if (notify_fd != 1) {
            if (move_fd(output_fd, 1) != 0) {
                goto failure_out;
            }
            if (notify_fd != 2 && dup2(1, 2) != 2) {
                goto failure_out;
            }
        }
        else if (move_fd(output_fd, 2) != 0) {
            goto failure_out;
        }

        // We have the option of creating a session and process group, or just a new process
        // group. If we just create a new process group, the child process cannot make itself
        // a session leader if it wants to do that (eg getty/login will generally want this).
        // If we do neither, and we are running with a controlling terminal, a ^C or similar
        // will also affect the child process (which probably isn't so bad, though since we
        // will handle the shutdown ourselves it's not necessary). Creating a new session
        // (and a new process group as part of that) seems like a safe bet, and has the
        // advantage of letting us signal the process as part of a process group.
        setsid();
    }
    else {
        // "run on console" - run as a foreground job on the terminal/console device

        // if do_set_ctty is false, we are the session leader; we are probably running
        // as a user process. Don't create a new session leader in that case, and run
        // as part of the parent session. Otherwise, the new session cannot claim the
        // terminal as a controlling terminal (it is already claimed), meaning that it
        // will not see control signals from ^C etc.

        if (do_set_ctty) {
            // Become session leader
            setsid();
            ioctl(0, TIOCSCTTY, 0);
        }
        setpgid(0,0);
        if (params.in_foreground) {
            tcsetpgrp(0, getpgrp());
        }
    }

    // Resource limits
    err.stage = exec_stage::SET_RLIMITS;
    for (auto &limit : rlimits) {
        rlimit setlimits;
        if (!limit.hard_set || !limit.soft_set) {
            // if either hard or soft limit is not set, use current:
            if (getrlimit(limit.resource_id, &setlimits) != 0) goto failure_out;
        }
        if (limit.hard_set) setlimits.rlim_max = limit.limits.rlim_max;
        if (limit.soft_set) setlimits.rlim_cur = limit.limits.rlim_cur;
        if (setrlimit(limit.resource_id, &setlimits) != 0) goto failure_out;
    }

    #if SUPPORT_CGROUPS
    if (params.run_in_cgroup != nullptr && *params.run_in_cgroup != 0) {
        err.stage = exec_stage::ENTER_CGROUP;

        int sys_fs_cgroup_fd = open("/sys/fs/cgroup", O_RDONLY | O_DIRECTORY | O_PATH);
        if (sys_fs_cgroup_fd == -1) goto failure_out;

        const char *run_cgroup_path = params.run_in_cgroup;
        if (run_cgroup_path[0] != '/') {
            // A relative cgroup path must be resolved against our own path (cgroups_path)
            if (!have_cgroups_path) {
                errno = ENOENT;
                goto failure_out;
            }
            if (!cgroups_path.empty()) {
                int cgrp_root_path = openat(sys_fs_cgroup_fd, cgroups_path.c_str(), O_RDONLY | O_DIRECTORY | O_PATH);
                if (cgrp_root_path == -1) goto failure_out;
                close(sys_fs_cgroup_fd);
                sys_fs_cgroup_fd = cgrp_root_path;
            }
        }
        else {
            ++run_cgroup_path; // skip leading slash
        }

        int cgroup_dir_fd = openat(sys_fs_cgroup_fd, run_cgroup_path, O_RDONLY | O_DIRECTORY | O_PATH);
        if (cgroup_dir_fd == -1) goto failure_out;
        close(sys_fs_cgroup_fd);

        int cgroup_procs_fd = openat(cgroup_dir_fd, "cgroup.procs", O_WRONLY);
        if (cgroup_procs_fd == -1) goto failure_out;
        close(cgroup_dir_fd);

        // We need to write our own pid into the cgroup.procs file
        char pidbuf[std::numeric_limits<pid_t>::digits10 + 3];
        // +1 for most significant digit, +1 for '\n', +1 for nul terminator
        int num_chars;
        if (sizeof(pid_t) <= sizeof(unsigned)) {
            num_chars = sprintf(pidbuf, "%u\n", (unsigned)getpid());
        }
        else if (sizeof(pid_t) <= sizeof(unsigned long)) {
            num_chars = sprintf(pidbuf, "%lu\n", (unsigned long)getpid());
        }
        else {
            static_assert(sizeof(pid_t) <= sizeof(unsigned long long), "pid_t is too big");
            num_chars = sprintf(pidbuf, "%llu\n", (unsigned long long)getpid());
        }

        if (write(cgroup_procs_fd, pidbuf, num_chars) == -1) goto failure_out;
        close(cgroup_procs_fd);
    }
    #endif

    if (uid != uid_t(-1)) {
        err.stage = exec_stage::SET_UIDGID;
        // We must set group first (i.e. before we drop privileges)
#if USE_INITGROUPS
        // Initialize supplementary groups unless disabled; non-POSIX API
        if (gid != gid_t(-1)) {
            // Specific group; use that, with no supplementary groups.
            // Note: for compatibility with FreeBSD, clear the groups list first before setting the
            // effective gid, because on FreeBSD setgroups also sets the effective gid.
            if (setgroups(0, nullptr)) goto failure_out;
            if (setregid(gid, gid) != 0) goto failure_out;
        }
        else {
            // No specific group; use groups associated with user.
            errno = 0;
            auto *pw = getpwuid(uid);
            if (pw) {
                if (setregid(pw->pw_gid, pw->pw_gid) != 0) goto failure_out;
                if (initgroups(pw->pw_name, pw->pw_gid) != 0) goto failure_out;
            }
            else {
                // null result with no errno indicates missing passwd entry; use ENOENT for want of a more
                // specific error code.
                if (errno == 0) errno = ENOENT;
                goto failure_out;
            }
        }
#else
        // No support for supplementary groups; just set the specified group.
        if (gid != gid_t(-1)) {
            if (setregid(gid, gid) != 0) goto failure_out;
        }
#endif
        if (setreuid(uid, uid) != 0) goto failure_out;
    }

    // Restore signal mask. If running on the console, we'll keep various control signals that can
    // be invoked from the terminal masked, with the exception of SIGHUP and possibly SIGINT.
    {
        sigset_t sigwait_set = orig_signal_mask;
        sigdelset(&sigwait_set, SIGCHLD);
        sigdelset(&sigwait_set, SIGTERM);
        if (on_console && params.in_foreground) {
            if (params.unmask_sigint) {
                sigdelset(&sigwait_set, SIGINT);
            }
            else {
                sigaddset(&sigwait_set, SIGINT);
            }
            sigaddset(&sigwait_set, SIGQUIT);
            sigaddset(&sigwait_set, SIGTSTP);
            sigaddset(&sigwait_set, SIGTTIN);
            sigaddset(&sigwait_set, SIGTTOU);
        }
        sigprocmask(SIG_SETMASK, &sigwait_set, nullptr);
    }

    err.stage = exec_stage::DO_EXEC;
    // (on linux we could use execvpe, but it's not POSIX and not in eg FreeBSD).
    bp_sys::environ = const_cast<char **>(proc_env_map.env_list.data());
    execvp(args[0], const_cast<char **>(args));

    // If we got here, the exec failed:
    failure_out:
    err.st_errno = errno;
    write(wpipefd, &err, sizeof(err));
    _exit(0);
}
