#include <cstddef>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <string>
#include <iostream>
#include <exception>

#include <sys/reboot.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "cpbuffer.h"
#include "control-cmds.h"
#include "service-constants.h"
#include "static-string.h"
#include "dinit-client.h"
#include "dinit-util.h"
#include "mconfig.h"
#include "control-datatypes.h"

#include "dasynq.h"

// shutdown:  shut down the system
// This utility communicates with the dinit daemon via a unix socket (specified in SYSCONTROLSOCKET).

static constexpr uint16_t min_cp_version = 1;
static constexpr uint16_t max_cp_version = 5;

static constexpr auto reboot_execname = cts::literal(SHUTDOWN_PREFIX) + cts::literal("reboot");
static constexpr auto soft_reboot_execname = cts::literal(SHUTDOWN_PREFIX) + cts::literal("soft-reboot");

using loop_t = dasynq::event_loop_n;
using rearm = dasynq::rearm;
using clock_type = dasynq::clock_type;
class subproc_buffer;

void do_system_shutdown(shutdown_type_t shutdown_type);
static void unmount_disks(loop_t &loop, subproc_buffer &sub_buf);
static void swap_off(loop_t &loop, subproc_buffer &sub_buf);
static loop_t::child_proc_watcher::proc_status_t run_process(const char * prog_args[],
        loop_t &loop, subproc_buffer &sub_buf);

constexpr static int subproc_bufsize = 4096;

constexpr static char output_lost_msg[] = "[Some output has not been shown due to buffer overflow]\n";

// A buffer which maintains a series of overflow markers, used for capturing and echoing
// subprocess output.
class subproc_buffer : private cpbuffer<subproc_bufsize>
{
    using base = cpbuffer<subproc_bufsize>;

    int overflow_marker = -1;
    int last_overflow = -1;  // last marker in the series
    const char *overflow_msg_ptr = nullptr;   // current position in overflow message
    dasynq::event_loop_n &loop;
    dasynq::event_loop_n::fd_watcher * out_watch;

    public:
    enum class fill_status
    {
        OK,
        ENDFILE,
        FULL
    };

    subproc_buffer(dasynq::event_loop_n &loop_p, int out_fd) : loop(loop_p)
    {
        using loop_t = dasynq::event_loop_n;
        using rearm = dasynq::rearm;

        out_watch = loop_t::fd_watcher::add_watch(loop, out_fd, dasynq::OUT_EVENTS,
                [&](loop_t &eloop, int fd, int flags) -> rearm {

            auto fstatus = flush(STDOUT_FILENO);
            if (fstatus == subproc_buffer::fill_status::ENDFILE) {
                return rearm::DISARM;
            }

            return rearm::REARM;
        });
    }

    ~subproc_buffer()
    {
        out_watch->deregister(loop);
    }

    // Fill buffer by reading from file descriptor. Note caller must set overflow marker
    // if the buffer becomes full and more data is available.
    fill_status fill(int fd)
    {
        int rem = get_free();

        if (rem == 0) {
            return fill_status::FULL;
        }

        int read = base::fill(fd, rem);
        if (read <= 0) {
            if (read == -1 && errno == EAGAIN) {
                return fill_status::OK;
            }
            return fill_status::ENDFILE;
        }

        out_watch->set_enabled(loop, true);
        return fill_status::OK;
    }

    // Append a message. If the message will not fit in the buffer, discard it and mark overflow.
    void append(const char *msg)
    {
        out_watch->set_enabled(loop, true);
        unsigned len = strlen(msg);
        if (subproc_bufsize - get_length() >= len) {
            base::append(msg, len);
        }
        else {
            mark_overflow();
        }
    }

    // Append the given buffer, which must fit in the remaining space in this buffer.
    void append(const char *buf, int len)
    {
        out_watch->set_enabled(loop, true);
        base::append(buf, len);
    }

    int get_free()
    {
        return base::get_free();
    }

    // Write buffer contents out to file descriptor. The descriptor is assumed to be non-blocking.
    // returns ENDFILE if there is no more content to flush (buffer is now empty) or OK otherwise.
    fill_status flush(int fd)
    {
        int to_write = get_contiguous_length(get_ptr(0));

        if (overflow_marker != -1) {
            if (overflow_marker == 0) {
                // output (remainder of) overflow message
                int l = std::strlen(overflow_msg_ptr);
                int r = write(fd, overflow_msg_ptr, l);
                if (r == l) {
                    // entire message has been written; next marker is in buffer
                    int16_t overflow_marker16;
                    extract(reinterpret_cast<char *>(&overflow_marker16), 0, sizeof(overflow_marker16));
                    overflow_marker = overflow_marker16;
                    consume(sizeof(overflow_marker16));

                    // no more overflow markers?
                    if (overflow_marker == -1) {
                        last_overflow = -1;
                    }
                    return get_length() == 0 ? fill_status::ENDFILE : fill_status::OK;
                }
                if (r > 0) {
                    overflow_msg_ptr += r;
                }
                return fill_status::OK;
            }

            to_write = std::min(to_write, overflow_marker);
        }

        int r = write(fd, get_ptr(0), to_write);
        if (r > 0) {
            consume(r);
            if (overflow_marker != -1) {
                overflow_marker -= r;
                last_overflow -= r;
                if (overflow_marker == 0) {
                    overflow_msg_ptr = output_lost_msg;
                }
            }
        }

        return get_length() == 0 ? fill_status::ENDFILE : fill_status::OK;
    }

    // Mark overflow occurred. Call this only when the buffer is full.
    // The marker is put after the most recent newline in the buffer, if possible, so that whole
    // lines are retained in the buffer. In some cases marking overflow will not add a new overflow
    // marker but simply trim the buffer to an existing marker.
    void mark_overflow()
    {
        // Try to find the last newline in the buffer
        int begin = 0;
        if (last_overflow != -1) {
            begin = last_overflow + sizeof(int16_t);
        }
        int end = get_length() - 1 - sizeof(int16_t); // -1, then -2 for storage of marker

        int i;
        for (i = end; i >= begin; i--) {
            if ((*this)[i] == '\n') break;
        }

        if (last_overflow != -1 && i < begin) {
            // No new line after existing marker: trim all beyond that marker, don't
            // create a new marker:
            trim_to(last_overflow + sizeof(uint16_t));
            return;
        }

        if (i < begin) {
            // No newline in the whole buffer... we'll put the overflow marker at the end,
            // on the assumption that it is better to output a partial line than it is to
            // discard the entire buffer:
            last_overflow = get_length() - sizeof(int16_t);
            overflow_marker = last_overflow;
            int16_t overflow16 = -1;
            char * overflow16_ptr = reinterpret_cast<char *>(&overflow16);
            *get_ptr(last_overflow + 0) = overflow16_ptr[0];
            *get_ptr(last_overflow + 1) = overflow16_ptr[1];
            return;
        }

        // We found a newline, put the overflow marker just after it:
        int new_overflow = i + 1;
        if (last_overflow != -1) {
            int16_t new_overflow16 = new_overflow;
            char * new_overflow16_ptr = reinterpret_cast<char *>(&new_overflow16);
            *get_ptr(last_overflow + 0) = new_overflow16_ptr[0];
            *get_ptr(last_overflow + 1) = new_overflow16_ptr[1];
        }
        last_overflow = new_overflow;
        if (overflow_marker == -1) {
            overflow_marker = last_overflow;
        }

        int16_t overflow16 = -1;
        char * overflow16_ptr = reinterpret_cast<char *>(&overflow16);
        *get_ptr(last_overflow + 0) = overflow16_ptr[0];
        *get_ptr(last_overflow + 0) = overflow16_ptr[1];
        trim_to(last_overflow + sizeof(int16_t));
    }
};

static bool
reboot_cmd_unsupported(const shutdown_type_t type)
{
    // weed out unsupported values
    switch (type) {
#if !defined(RB_HALT_SYSTEM) && !defined(RB_HALT)
    case shutdown_type_t::HALT: return true;
#endif
#ifndef RB_POWER_OFF
    case shutdown_type_t::POWEROFF: return true;
#endif
#ifndef RB_KEXEC
    case shutdown_type_t::KEXEC: return true;
#endif
    default: return false;
    }
}


int main(int argc, char **argv)
{
    using namespace std;
    
    bool show_help = false;
    bool sys_shutdown = false;
    bool use_passed_cfd = false;
    
    auto shutdown_type = shutdown_type_t::POWEROFF;

    const char *execname = base_name(argv[0]);
    if (strcmp(execname, reboot_execname) == 0) {
        shutdown_type = shutdown_type_t::REBOOT;
    }
    else if (strcmp(execname, soft_reboot_execname) == 0) {
        shutdown_type = shutdown_type_t::SOFTREBOOT;
    }
        
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "--help") == 0) {
                show_help = true;
                break;
            }
            
            if (strcmp(argv[i], "--system") == 0) {
                sys_shutdown = true;
            }
            else if (strcmp(argv[i], "-r") == 0) {
                shutdown_type = shutdown_type_t::REBOOT;
            }
            else if (strcmp(argv[i], "-h") == 0) {
                shutdown_type = shutdown_type_t::HALT;
            }
            else if (strcmp(argv[i], "-p") == 0) {
                shutdown_type = shutdown_type_t::POWEROFF;
            }
            else if (strcmp(argv[i], "-s") == 0) {
                shutdown_type = shutdown_type_t::SOFTREBOOT;
            }
            else if (strcmp(argv[i], "-k") == 0) {
                shutdown_type = shutdown_type_t::KEXEC;
            }
            else if (strcmp(argv[i], "--use-passed-cfd") == 0) {
                use_passed_cfd = true;
            }
            else {
                cerr << "Unrecognized command-line parameter: " << argv[i] << endl;
                return 1;
            }
        }
        else {
            // time argument? TODO
            show_help = true;
        }
    }

    if (reboot_cmd_unsupported(shutdown_type)) {
        cerr << "Unsupported shutdown type\n";
        return 1;
    }

    if (show_help) {
        cout << execname << " :   shutdown the system\n"
                "  --help           : show this help\n"
                "  -r               : reboot\n"
                "  -s               : soft-reboot (restart dinit with same boot-time arguments)\n"
#if defined(RB_HALT_SYSTEM) || defined(RB_HALT)
                "  -h               : halt system\n"
#endif
#ifdef RB_POWER_OFF
                "  -p               : power down (default)\n"
#endif
#ifdef RB_KEXEC
                "  -k               : stop dinit and reboot directly into kernel loaded with kexec\n"
#endif
                "  --use-passed-cfd : use the socket file descriptor identified by the DINIT_CS_FD\n"
                "                     environment variable to communicate with the init daemon.\n"
                "  --system         : perform shutdown immediately, instead of issuing shutdown\n"
                "                     command to the init program. Not recommended for use\n"
                "                     by users.\n";
        return 1;
    }
    
    if (sys_shutdown) {
        do_system_shutdown(shutdown_type);
        return 1; // likely to cause panic; the above shouldn't return
    }

    signal(SIGPIPE, SIG_IGN);
    
    int socknum = -1;
    
    if (use_passed_cfd) {
        socknum = get_passed_cfd();
        if (socknum == -1) {
            use_passed_cfd = false;
        }
    }
    
    if (!use_passed_cfd) {
        socknum = socket(AF_UNIX, SOCK_STREAM, 0);
        if (socknum == -1) {
            perror("socket");
            return 1;
        }
        
        const char *naddr = SYSCONTROLSOCKET;
        
        struct sockaddr_un name;
        name.sun_family = AF_UNIX;
        strcpy(name.sun_path, naddr);
        int sunlen = offsetof(struct sockaddr_un, sun_path) + strlen(naddr) + 1; // family, (string), nul
        
        int connr = connect(socknum, (struct sockaddr *) &name, sunlen);
        if (connr == -1) {
            perror("connect");
            return 1;
        }
    }

    try {
        cpbuffer_t rbuffer;
    
        check_protocol_version(min_cp_version, max_cp_version, rbuffer, socknum);

        // Build buffer;
        constexpr int bufsize = 2;
        char buf[bufsize];

        buf[0] = (dinit_cptypes::cp_cmd_t)cp_cmd::SHUTDOWN;
        buf[1] = static_cast<char>(shutdown_type);

        cout << "Issuing shutdown command..." << endl;

        write_all_x(socknum, buf, bufsize);

        // Wait for ACK/NACK
    
        wait_for_reply(rbuffer, socknum);
        
        if (rbuffer[0] != (dinit_cptypes::cp_rply_t)cp_rply::ACK) {
            cerr << "shutdown: control socket protocol error" << endl;
            return 1;
        }
    }
    catch (cp_old_client_exception &e) {
        std::cerr << "shutdown: too old (server reports newer protocol version)" << std::endl;
        return 1;
    }
    catch (cp_old_server_exception &e) {
        std::cerr << "shutdown: server too old or protocol error" << std::endl;
        return 1;
    }
    catch (cp_read_exception &e) {
        cerr << "shutdown: control socket read failure or protocol error" << endl;
        return 1;
    }
    catch (cp_write_exception &e) {
        cerr << "shutdown: control socket write error: " << std::strerror(e.errcode) << endl;
        return 1;
    }

    while (true) {
        pause();
    }
    
    return 0;
}

// Actually shut down the system.
void do_system_shutdown(shutdown_type_t shutdown_type)
{
    using namespace std;
    
    // Mask all signals to prevent death of our parent etc from terminating us
    sigset_t allsigs;
    sigfillset(&allsigs);
    sigprocmask(SIG_SETMASK, &allsigs, nullptr);
    
    int reboot_type = RB_AUTOBOOT; // reboot
#if defined(RB_POWER_OFF)
    if (shutdown_type == shutdown_type_t::POWEROFF) reboot_type = RB_POWER_OFF;
#endif
#if defined(RB_HALT_SYSTEM)
    if (shutdown_type == shutdown_type_t::HALT) reboot_type = RB_HALT_SYSTEM;
#elif defined(RB_HALT)
    if (shutdown_type == shutdown_type_t::HALT) reboot_type = RB_HALT;
#endif
#if defined(RB_KEXEC)
    if (shutdown_type == shutdown_type_t::KEXEC) reboot_type = RB_KEXEC;
#endif
    
    // Write to console rather than any terminal, since we lose the terminal it seems:
    int consfd = open("/dev/console", O_WRONLY);
    if (consfd != STDOUT_FILENO && consfd != -1) {
        dup2(consfd, STDOUT_FILENO);
    }
    
    loop_t loop;
    subproc_buffer sub_buf {loop, STDOUT_FILENO};

    sub_buf.append("Sending TERM/KILL to all processes...\n");
    
    // Send TERM/KILL to all (remaining) processes
    kill(-1, SIGTERM);

    // 1 second delay (while outputting from sub_buf):
    bool timeout_reached = false;
    dasynq::time_val timeout {1, 0};
    dasynq::time_val interval {0,0};
    loop_t::timer::add_timer(loop, clock_type::MONOTONIC, true /* relative */,
            timeout.get_timespec(), interval.get_timespec(),
            [&](loop_t &eloop, int expiry_count) -> rearm {

        timeout_reached = true;
        return rearm::REMOVE;
    });

    do {
      loop.run();
    } while (! timeout_reached);

    kill(-1, SIGKILL);

    // Attempt to execute shutdown hook at three possible locations:
    const char * const hook_paths[] = {
            "/etc/dinit/shutdown-hook",
            "/lib/dinit/shutdown-hook"
    };
    
    bool do_unmount_ourself = true;
    const int execmask = S_IXOTH | S_IXGRP | S_IXUSR;
    struct stat statbuf;

    for (size_t i = 0; i < sizeof(hook_paths) / sizeof(hook_paths[0]); ++i) {
        int stat_r = lstat(hook_paths[i], &statbuf);
        if (stat_r == 0 && (statbuf.st_mode & execmask) != 0) {
            sub_buf.append("Executing shutdown hook...\n");
            const char *prog_args[] = { hook_paths[i], nullptr };
            try {
                auto r = run_process(prog_args, loop, sub_buf);
                if (r.did_exit() && r.get_exit_status() == 0) {
                    do_unmount_ourself = false;
                }
            }
            catch (std::exception &e) {
                sub_buf.append("Couldn't fork for shutdown-hook: ");
                sub_buf.append(e.what());
                sub_buf.append("\n");
            }
            break;
        }
    }

    // perform shutdown
    if (do_unmount_ourself) {
        sub_buf.append("Turning off swap...\n");
        swap_off(loop, sub_buf);
        sub_buf.append("Unmounting disks...\n");
        unmount_disks(loop, sub_buf);
    }

    sync();
    
    sub_buf.append("Issuing shutdown via kernel...\n");
    loop.poll();  // give message a chance to get to console
#ifdef __NetBSD__
    reboot(reboot_type, NULL);
#else
    if (reboot(reboot_type)) {
        // we're in trouble now
        sub_buf.append("reboot(2): ");
        sub_buf.append(strerror(errno));
        sub_buf.append(
                "\nThis may happen if you try to kexec without loading an image first, or if\n"
                "\nsomehow a reboot type unsupported by the kernel is attempted\n"
        );
        loop.poll();
    }
#endif
}

// Watcher for subprocess output.
class subproc_out_watch : public loop_t::fd_watcher_impl<subproc_out_watch>
{
    subproc_buffer &sub_buf;
    bool in_overflow = false;

    rearm read_overflow(int fd)
    {
        char buf[128];
        int r = read(fd, buf, 128);
        if (r == 0 || (r == -1 && errno != EAGAIN)) {
            return rearm::NOOP; // leave disarmed
        }
        if (r == -1) {
            return rearm::REARM;
        }

        // How much space is available?
        int fr = sub_buf.get_free();
        for (int b = r - std::min(r, fr); b < r; b++) {
            if (buf[b] == '\n') {
                // Copy the (partial) line into sub_buf and leave overflow mode
                sub_buf.append(buf + b, r - b);
                in_overflow = false;
            }
        }
        return rearm::REARM;
    }

    public:
    subproc_out_watch(subproc_buffer &sub_buf_p) : sub_buf(sub_buf_p) {}

    rearm fd_event(loop_t &, int fd, int flags)
    {
        // if current status is reading overflow, read and discard until newline
        if (in_overflow) {
            return read_overflow(fd);
        }

        auto r = sub_buf.fill(fd);
        if (r == subproc_buffer::fill_status::FULL) {
            sub_buf.mark_overflow();
            in_overflow = true;
            return read_overflow(fd);
        }
        else if (r == subproc_buffer::fill_status::ENDFILE) {
            return rearm::NOOP;
        }

        return rearm::REARM;  // re-enable watcher
    }
};

// Run process, put its output through the subprocess buffer
//   may throw: std::system_error, std::bad_alloc
static loop_t::child_proc_watcher::proc_status_t run_process(const char * prog_args[],
        loop_t &loop, subproc_buffer &sub_buf)
{
    class sp_watcher_t : public loop_t::child_proc_watcher_impl<sp_watcher_t>
    {
        public:
        bool terminated = false;
        proc_status_t exit_status;

        rearm status_change(loop_t &, pid_t child, proc_status_t status)
        {
            terminated = true;
            exit_status = status;
            return rearm::REMOVE;
        }
    };

    sp_watcher_t sp_watcher;

    // Create output pipe
    bool have_pipe = true;
    int pipefds[2];
    if (dasynq::pipe2(pipefds, O_NONBLOCK) == -1) {
        sub_buf.append("Warning: ");
        sub_buf.append(prog_args[0]);
        sub_buf.append(": could not create pipe for subprocess output\n");
        have_pipe = false;
        // Note, we proceed and let the sub-process run with our stdout/stderr.
    }

    subproc_out_watch owatch {sub_buf};

    if (have_pipe) {
        try {
            owatch.add_watch(loop, pipefds[0], dasynq::IN_EVENTS);
        }
        catch (...) {
            // failed to create the watcher for the subprocess output; again, let it run with
            // our stdout/stderr
            sub_buf.append("Warning: could not create output watch for subprocess\n");
            close(pipefds[0]);
            close(pipefds[1]);
            have_pipe = false;
        }
    }

    // If we've buffered any messages/output, give them a chance to go out now:
    loop.poll();

    pid_t ch_pid = sp_watcher.fork(loop);
    if (ch_pid == 0) {
        // child
        // Dup output pipe to stdout, stderr
        if (have_pipe) {
            dup2(pipefds[1], STDOUT_FILENO);
            dup2(pipefds[1], STDERR_FILENO);
            close(pipefds[0]);
            close(pipefds[1]);
        }
        execv(prog_args[0], const_cast<char **>(prog_args));
        puts("Failed to execute subprocess: ");
        perror(prog_args[0]);
        _exit(1);
    }
    if (have_pipe) {
        close(pipefds[1]);
    }

    do {
        loop.run();
    } while (!sp_watcher.terminated);

    if (have_pipe) {
        owatch.deregister(loop);
        close(pipefds[0]);
    }

    return sp_watcher.exit_status;
}

static void unmount_disks(loop_t &loop, subproc_buffer &sub_buf)
{
    try {
#ifdef __NetBSD__
        const char * unmount_args[] = { "/sbin/umount", "-a", nullptr };
#else
        const char * unmount_args[] = { "/bin/umount", "-a", "-r", nullptr };
#endif
        run_process(unmount_args, loop, sub_buf);
    }
    catch (std::exception &e) {
        sub_buf.append("Couldn't fork for umount: ");
        sub_buf.append(e.what());
        sub_buf.append("\n");
    }
}

static void swap_off(loop_t &loop, subproc_buffer &sub_buf)
{
    try {
#ifdef __NetBSD__
        const char * swapoff_args[] = { "/sbin/swapctl", "-U", nullptr };
#else
        const char * swapoff_args[] = { "/sbin/swapoff", "-a", nullptr };
#endif
        run_process(swapoff_args, loop, sub_buf);
    }
    catch (std::exception &e) {
        sub_buf.append("Couldn't fork for swapoff: ");
        sub_buf.append(e.what());
        sub_buf.append("\n");
    }
}
