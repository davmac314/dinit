#include <string>
#include <iostream>
#include <cstring>

#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>

extern char **environ;

// Integration test suite runner.

int main(int argc, char **argv)
{
    const char * const test_dirs[] = { "basic", "environ", "environ2", "ps-environ", "chain-to", "force-stop",
            "restart", "check-basic", "check-cycle", "check-cycle2", "check-lint", "reload1", "reload2",
            "no-command-error", "add-rm-dep", "var-subst", "svc-start-fail", "dep-not-found", "pseudo-cycle",
            "before-after", "before-after2", "log-via-pipe", "catlog", "offline-enable" };
    constexpr int num_tests = sizeof(test_dirs) / sizeof(test_dirs[0]);

    int passed = 0;
    int skipped = 0;
    int failed = 0;

    bool aborted_run = false;

    std::cout << "============== INTEGRATION TESTS =====================" << std::endl;

    for (int i = 0; i < num_tests; i++) {
        const char * test_dir = test_dirs[i];

        std::string prog_path = "./run-test.sh";
        char * const p_argv[2] = { const_cast<char *>(prog_path.c_str()), nullptr };

        std::cout << test_dir << "... ";

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
