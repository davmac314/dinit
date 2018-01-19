#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

#include "service.h"

void service_record::run_child_proc(const char * const *args, const char *logfile, bool on_console,
        int wpipefd, int csfd, int socket_fd) noexcept
{
    // Child process. Must not allocate memory (or otherwise risk throwing any exception)
    // from here until exit().

    // If the console already has a session leader, presumably it is us. On the other hand
    // if it has no session leader, and we don't create one, then control inputs such as
    // ^C will have no effect.
    bool do_set_ctty = (tcgetsid(0) == -1);

    // Copy signal mask, but unmask signals that we masked on startup. For the moment, we'll
    // also block all signals, since apparently dup() can be interrupted (!!! really, POSIX??).
    sigset_t sigwait_set;
    sigset_t sigall_set;
    sigfillset(&sigall_set);
    sigprocmask(SIG_SETMASK, &sigall_set, &sigwait_set);
    sigdelset(&sigwait_set, SIGCHLD);
    sigdelset(&sigwait_set, SIGINT);
    sigdelset(&sigwait_set, SIGTERM);
    sigdelset(&sigwait_set, SIGQUIT);

    constexpr int bufsz = ((CHAR_BIT * sizeof(pid_t)) / 3 + 2) + 11;
    // "LISTEN_PID=" - 11 characters; the expression above gives a conservative estimate
    // on the maxiumum number of bytes required for LISTEN=nnn, including nul terminator,
    // where nnn is a pid_t in decimal (i.e. one decimal digit is worth just over 3 bits).
    char nbuf[bufsz];

    // "DINIT_CS_FD=" - 12 bytes. (we -1 from sizeof(int) in account of sign bit).
    constexpr int csenvbufsz = ((CHAR_BIT * sizeof(int) - 1) / 3 + 2) + 12;
    char csenvbuf[csenvbufsz];

    int minfd = (socket_fd == -1) ? 3 : 4;

    // Move wpipefd/csfd to another fd if necessary
    if (wpipefd < minfd) {
        wpipefd = fcntl(wpipefd, F_DUPFD_CLOEXEC, minfd);
        if (wpipefd == -1) goto failure_out;
    }

    if (csfd != -1 && csfd < minfd) {
        csfd = fcntl(csfd, F_DUPFD, minfd);
        if (csfd == -1) goto failure_out;
    }

    if (socket_fd != -1) {

        if (dup2(socket_fd, 3) == -1) goto failure_out;
        if (socket_fd != 3) {
            close(socket_fd);
        }

        if (putenv(const_cast<char *>("LISTEN_FDS=1"))) goto failure_out;
        snprintf(nbuf, bufsz, "LISTEN_PID=%jd", static_cast<intmax_t>(getpid()));
        if (putenv(nbuf)) goto failure_out;
    }

    if (csfd != -1) {
        snprintf(csenvbuf, csenvbufsz, "DINIT_CS_FD=%d", csfd);
        if (putenv(csenvbuf)) goto failure_out;
    }

    if (! on_console) {
        // Re-set stdin, stdout, stderr
        close(0); close(1); close(2);

        if (open("/dev/null", O_RDONLY) == 0) {
            // stdin = 0. That's what we should have; proceed with opening
            // stdout and stderr.
            if (open(logfile, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR) != 1) {
                goto failure_out;
            }
            if (dup2(1, 2) != 2) {
                goto failure_out;
            }
        }
        else goto failure_out;

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
            // Disable suspend (^Z) (and on some systems, delayed suspend / ^Y)
            signal(SIGTSTP, SIG_IGN);

            // Become session leader
            setsid();
            ioctl(0, TIOCSCTTY, 0);
        }
        setpgid(0,0);
        tcsetpgrp(0, getpgrp());
    }

    sigprocmask(SIG_SETMASK, &sigwait_set, nullptr);

    execvp(args[0], const_cast<char **>(args));

    // If we got here, the exec failed:
    failure_out:
    int exec_status = errno;
    write(wpipefd, &exec_status, sizeof(int));
    _exit(0);
}
