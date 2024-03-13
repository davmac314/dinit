#include <string>
#include <iostream>
#include <cstring>

#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "igr.h"

extern char **environ;

// Integration test suite runner.

std::string igr_output_basedir;
std::string igr_dinit_socket_path;

void basic_test();
void environ_test();

int main(int argc, char **argv)
{
    void (*test_funcs[])() = { basic_test, environ_test };
    const char * const test_dirs[] = { "basic", "environ", "environ2", "ps-environ", "chain-to", "force-stop",
            "restart", "check-basic", "check-cycle", "check-cycle2", "check-lint", "reload1", "reload2",
            "no-command-error", "add-rm-dep", "var-subst", "svc-start-fail", "dep-not-found", "pseudo-cycle",
            "before-after", "before-after2", "log-via-pipe", "catlog", "offline-enable", "xdg-config",
            "cycles" };
    constexpr int num_tests = sizeof(test_dirs) / sizeof(test_dirs[0]);

    dinit_bindir = "../.."; // XXX
    igr_output_basedir = "igr-output"; // XXX

    int passed = 0;
    int skipped = 0;
    int failed = 0;

    bool aborted_run = false;

    char *env_igr_output_base = getenv("IGR_OUTPUT_BASE");
    if (env_igr_output_base != nullptr) {
        igr_output_basedir = env_igr_output_base;
    }

    igr_dinit_socket_path = igr_output_basedir + "/dinit.socket";

    std::cout << "============== INTEGRATION TESTS =====================" << std::endl;

    if (mkdir(igr_output_basedir.c_str(), 0700) == -1 && errno != EEXIST) {
        std::system_error(errno, std::generic_category(), std::string("mkdir: ") + igr_output_basedir);
    }

    for (int i = 0; i < num_tests; i++) {
        const char * test_dir = test_dirs[i];

        std::cout << test_dir << "... ";

        if (i < (sizeof(test_funcs) / sizeof(test_funcs[0]))) {
            // run function instead
            bool success;
            try {
                test_funcs[i]();
                success = true;
            }
            catch (igr_failure_exc &exc) {
                success = false;
            }

            if (success) {
                std::cout << "PASSED" << std::endl;
                passed++;
            }
            else {
                std::cout << "FAILED" << std::endl;
                failed++;
            }

            continue;
        }

        std::string prog_path = "./run-test.sh";
        char * const p_argv[2] = { const_cast<char *>(prog_path.c_str()), nullptr };

        // "Use posix_spawn", they said. "It will be easy", they said.

        if (chdir(test_dir) != 0) {
            std::cerr << "Couldn't chdir: " << test_dir << ": " << strerror(errno) << std::endl;
            continue;
        }

        posix_spawn_file_actions_t p_actions;
        posix_spawnattr_t p_attr;

        if (posix_spawn_file_actions_init(&p_actions) != 0) {
            // out of memory?
            std::cerr << "Error launching process: " << test_dir << "/run-test.sh: " << strerror(errno) << std::endl;
            aborted_run = true;
            break;
        }

        if (posix_spawnattr_init(&p_attr) != 0) {
            // out of memory?
            std::cerr << "Error launching process: " << test_dir << "/run-test.sh: " << strerror(errno) << std::endl;
            aborted_run = true;
            break;
        }

        pid_t subproc_pid;
        if (posix_spawn(&subproc_pid, prog_path.c_str(), &p_actions, &p_attr, p_argv, environ) != 0) {
            // fail out
            std::cerr << "Failed to run run-test.sh in " << test_dir << std::endl;
            continue;
        }

        int wstatus;
        if (waitpid(subproc_pid, &wstatus, 0) == -1) {
            std::cout << "(unknown)" << std::endl;
            std::cerr << "waitpid() failed" << std::endl;
            aborted_run = true;
            break;
        }

        if (WIFEXITED(wstatus)) {
            if (WEXITSTATUS(wstatus) == 0) {
                std::cout << "PASSED" << std::endl;
                passed++;
            }
            else if (WEXITSTATUS(wstatus) == 1) {
                std::cout << "FAILED" << std::endl;
                failed++;
            }
            else if (WEXITSTATUS(wstatus) == 77) {
                std::cout << "SKIPPED" << std::endl;
                skipped++;
            }
            else {
                std::cout << "???" << std::endl;
            }
        }
        else {
            std::cout << "*** terminated abnormally ***" << std::endl;
            aborted_run = true;
            break;
        }

        posix_spawnattr_destroy(&p_attr);
        posix_spawn_file_actions_destroy(&p_actions);
        chdir("..");
    }

    std::cout << "======================================================" << std::endl;

    if (! aborted_run) {
        std::cout << "Test run finished.\n"
                "Passed: " << passed << "\n"
                "Failed: " << failed;
        if (failed != 0) {
            std::cout << " XXX";
        }
        std::cout << "\n"
                "Skipped: " << skipped << std::endl;
    }
    else {
        std::cout << "Test run aborted." << std::endl;
    }

    return failed == 0 ? 0 : 1;
}

void basic_test()
{
    std::string output_dir = igr_output_basedir + "/basic";
    if (mkdir(output_dir.c_str(), 0700) == -1 && errno != EEXIST) {
        throw std::system_error(errno, std::generic_category(), std::string("mkdir: ") + output_dir);
    }

    setenv("IGR_OUTPUT", output_dir.c_str(), true);

    // Start the "basic" service. This creates an output file, "basic-ran", containing "ran\n".
    dinit_proc dinit_p;
    dinit_p.start("basic", {"-u", "-d", "sd", "-p", igr_dinit_socket_path, "-q", "basic"});
    dinit_p.wait_for_term({1,0} /* max 1 second */);

    igr_assert_eq("", dinit_p.get_stdout());
    igr_assert_eq("", dinit_p.get_stderr());

    check_file_contents(igr_output_basedir + "/basic/basic-ran", "ran\n");

    unsetenv("IGR_OUTPUT");
}

void environ_test()
{
    std::string output_dir = igr_output_basedir + "/environ";
    if (mkdir(output_dir.c_str(), 0700) == -1 && errno != EEXIST) {
        throw std::system_error(errno, std::generic_category(), std::string("mkdir: ") + output_dir);
    }

    setenv("IGR_OUTPUT", output_dir.c_str(), true);

    std::string output_file = output_dir + "/env-record";
    if (unlink(output_file.c_str()) == -1 && errno != ENOENT) {
        throw std::system_error(errno, std::generic_category(),
                std::string("unlink " + output_file + ": ") + output_dir);
    }

    setenv("OUTPUT", (output_dir + "/env-record").c_str(), true);
    setenv("SOCKET", igr_dinit_socket_path.c_str(), true);
    setenv("DINITCTL", (dinit_bindir + "/dinitctl").c_str(), true);

    dinit_proc dinit_p;
    dinit_p.start("environ", {"-u", "-d", "sd", "-p", igr_dinit_socket_path, "-q", "-e", "environment1", "checkenv"});
    dinit_p.wait_for_term({1,0} /* max 1 second */);

    dinit_p.start("environ", {"-u", "-d", "sd", "-p", igr_dinit_socket_path, "-q", "-e", "environment2", "checkenv"});
    dinit_p.wait_for_term({1,0} /* max 1 second */);

    dinit_p.start("environ", {"-u", "-d", "sd", "-p", igr_dinit_socket_path, /* "-q", */ "setenv1"});
    dinit_p.wait_for_term({1,0} /* max 1 second */);

    check_file_contents(output_file, igr_dinit_socket_path + "\n" +
            "checkenv\n" +
            "gotenv1\n" +
            "hello\n" +
            "gotenv2\n" +
            "goodbye\n" +
            "3\n2\n1\n");

    unsetenv("DINITCTL");
    unsetenv("SOCKET");
    unsetenv("OUTPUT");

    unsetenv("IGR_OUTPUT");
}
