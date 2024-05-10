#include <string>
#include <iostream>
#include <cstring>

#include <pwd.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "igr.h"

extern char **environ;

// Integration test suite runner.

std::string igr_dinit_socket_path;

void basic_test();
void environ_test();
void environ2_test();
void ps_environ_test();
void chain_to_test();
void force_stop_test();
void restart_test();
void check_basic_test();
void check_cycle_test();
void check_cycle2_test();
void check_lint_test();
void reload1_test();

int main(int argc, char **argv)
{
    void (*test_funcs[])() = { basic_test, environ_test, environ2_test, ps_environ_test,
            chain_to_test, force_stop_test, restart_test, check_basic_test, check_cycle_test,
            check_cycle2_test, check_lint_test, reload1_test };
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

        std::cout << test_dir << "... " << std::flush;

        if ((unsigned)i < (sizeof(test_funcs) / sizeof(test_funcs[0]))) {
            // run function instead
            bool success;
            std::string failure_msg;
            try {
                test_funcs[i]();
                success = true;
            }
            catch (igr_failure_exc &exc) {
                success = false;
                failure_msg = exc.get_message();
            }

            if (success) {
                std::cout << "PASSED\n";
                passed++;
            }
            else {
                std::cout << "FAILED\n";
                failed++;
                std::cout << failure_msg << std::endl;
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
    igr_test_setup setup("basic");
    std::string ran_file = setup.prep_output_file("basic-ran");

    // Start the "basic" service. This creates an output file, "basic-ran", containing "ran\n".
    dinit_proc dinit_p;
    dinit_p.start("basic", {"-u", "-d", "sd", "-p", igr_dinit_socket_path, "-q", "basic"});
    dinit_p.wait_for_term({1,0} /* max 1 second */);

    igr_assert_eq("", dinit_p.get_stdout());
    igr_assert_eq("", dinit_p.get_stderr());

    check_file_contents(ran_file.c_str(), "ran\n");
}

void environ_test()
{
    igr_test_setup setup("environ");
    std::string output_file = setup.prep_output_file("env-record");

    igr_env_var_setup env_output("OUTPUT", output_file.c_str());
    igr_env_var_setup env_socket("SOCKET", igr_dinit_socket_path.c_str());
    igr_env_var_setup env_dinitctl("DINITCTL", (dinit_bindir + "/dinitctl").c_str());

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
}

void environ2_test()
{
    igr_test_setup setup("environ2");
    std::string output_file = setup.prep_output_file("env-record");

    uid_t my_uid = getuid();
    gid_t my_gid = getgid();
    struct passwd *my_pwd_ent = getpwuid(my_uid);
    if (my_pwd_ent == nullptr) {
        throw std::system_error(errno, std::generic_category(), "getpwuid");
    }

    // unset variables to make sure the values seen in the test service were initialised by dinit:
    igr_env_var_setup env_user("USER", nullptr);
    igr_env_var_setup env_logname("LOGNAME", nullptr);
    igr_env_var_setup env_shell("SHELL", nullptr);
    igr_env_var_setup env_uid("UID", nullptr);
    igr_env_var_setup env_gid("GID", nullptr);

    // test whether vars from global environment propagate
    igr_env_var_setup env_test_var("TEST_VAR", "helloworld");

    dinit_proc dinit_p;
    dinit_p.start("environ2", {"-u", "-d", "sd", "-p", igr_dinit_socket_path, "-q", "-e", "env-dinit", "checkenv"});
    dinit_p.wait_for_term({1,0} /* max 1 second */);

    check_file_contents(output_file,
            std::string("helloworld\n") +
            "hello\n" +
            "override\n" +
            my_pwd_ent->pw_name + "\n" +
            my_pwd_ent->pw_name + "\n" +
            "/bogus/value\n" +
            std::to_string(my_uid) + "\n" +
            std::to_string(my_gid) + "\n");
}

void ps_environ_test()
{
    igr_test_setup setup("ps-environ");
    std::string output_file = setup.prep_output_file("env-record");

    igr_env_var_setup env_output("OUTPUT", output_file.c_str());
    igr_env_var_setup env_test_var_two("TEST_VAR_TWO", "set-via-script");

    dinit_proc dinit_p;
    dinit_p.start("ps-environ", {"-u", "-d", "sd", "-p", igr_dinit_socket_path, "-q", "checkenv1"});
    dinit_p.wait_for_term({1,0} /* max 1 second */);

    dinit_p.start("ps-environ", {"-u", "-d", "sd", "-p", igr_dinit_socket_path, "-q", "checkenv2"});
    dinit_p.wait_for_term({1,0} /* max 1 second */);

    dinit_p.start("ps-environ", {"-u", "-d", "sd", "-p", igr_dinit_socket_path, "-q", "checkenv3"});
    dinit_p.wait_for_term({1,0} /* max 1 second */);

    // "set-in-dinit-env"
    dinit_p.start("ps-environ", {"-u", "-d", "sd", "-p", igr_dinit_socket_path, "-q", "-e", "dinit-environment", "checkenv4"});
    dinit_p.wait_for_term({1,0} /* max 1 second */);

    // "set-via-script" (as per above)
    dinit_p.start("ps-environ", {"-u", "-d", "sd", "-p", igr_dinit_socket_path, "-q", "checkenv4"});
    dinit_p.wait_for_term({1,0} /* max 1 second */);

    check_file_contents(output_file, read_file_contents("./ps-environ/env-expected"));
}

void chain_to_test()
{
    igr_test_setup setup("chain-to");
    std::string output_file = setup.prep_output_file("recorded-output");

    dinit_proc dinit_p;
    dinit_p.start("chain-to", {"-u", "-d", "sd", "-p", igr_dinit_socket_path, "-q", "part1"});
    dinit_p.wait_for_term({1,0} /* max 1 second */);

    check_file_contents(output_file, read_file_contents("./chain-to/expected-output"));
}

void force_stop_test()
{
    igr_test_setup setup("force-stop");

    dinit_proc dinit_p;
    dinit_p.start("force-stop", {"-u", "-d", "sd", "-p", igr_dinit_socket_path, "-q"}, true);

    // "dinitctl list"
    dinitctl_proc dinitctl_p;
    dinitctl_p.start("force-stop", {"-p", igr_dinit_socket_path, "list"});
    dinitctl_p.wait_for_term({1, 0}  /* max 1 second */);

    igr_assert_eq(read_file_contents("./force-stop/expected-1"), dinitctl_p.get_stdout());
    igr_assert_eq("", dinitctl_p.get_stderr());

    // "dinitctl stop critical"
    dinitctl_p.start("force-stop", {"-p", igr_dinit_socket_path, "stop", "critical"});
    dinitctl_p.wait_for_term({1, 0}  /* max 1 second */);

    igr_assert_eq("", dinitctl_p.get_stdout());
    igr_assert_eq(read_file_contents("./force-stop/expected-2.err"), dinitctl_p.get_stderr());

    // "dinitctl stop --force critical"
    dinitctl_p.start("force-stop", {"-p", igr_dinit_socket_path, "stop", "--force", "critical"});
    dinitctl_p.wait_for_term({1, 0}  /* max 1 second */);

    igr_assert_eq(read_file_contents("./force-stop/expected-3"), dinitctl_p.get_stdout());
    igr_assert_eq("", dinitctl_p.get_stderr());

    // dinit should stop since all services are now stopped
    dinit_p.wait_for_term({1, 0});
}

void restart_test()
{
    igr_test_setup setup("restart");
    std::string output_file = setup.prep_output_file("basic-ran");

    dinit_proc dinit_p;
    dinit_p.start("restart", {"-u", "-d", "sd", "-p", igr_dinit_socket_path, "-q"}, true);

    // "dinitctl start boot" - wait until "boot" has fully started:
    dinitctl_proc dinitctl_p;
    dinitctl_p.start("restart", {"-p", igr_dinit_socket_path, "start", "boot"});
    dinitctl_p.wait_for_term({1, 0}  /* max 1 second */);

    // "basic" is a process service. It has started, but we need to give it a little time to
    // write its output:
    nanosleepx(0, 1000000000u / 10u);

    igr_assert_eq("ran\n", read_file_contents(output_file));
    if (unlink(output_file.c_str()) == -1) {
        throw std::system_error(errno, std::generic_category(), "unlink");
    }

    // "dinitctl restart basic"
    dinitctl_p.start("restart", {"-p", igr_dinit_socket_path, "restart", "basic"});
    dinitctl_p.wait_for_term({1, 0}  /* max 1 second */);

    nanosleepx(0, 1000000000u / 10u);

    igr_assert_eq("ran\n", read_file_contents(output_file));
}

void check_basic_test()
{
    igr_test_setup setup("check-basic");

    auto check_result = run_dinitcheck("check-basic", {"-d", "sd"});
    igr_assert_eq(read_file_contents("./check-basic/expected.txt"), check_result.first);
    igr_assert(WEXITSTATUS(check_result.second) == 1, "dinitcheck exit status == 1");
}

void check_cycle_test()
{
    igr_test_setup setup("check-cycle");

    auto check_result = run_dinitcheck("check-cycle", {"-d", "sd"});
    igr_assert_eq(read_file_contents("./check-cycle/expected.txt"), check_result.first);
    igr_assert(WEXITSTATUS(check_result.second) == 1, "dinitcheck exit status == 1");
}

void check_cycle2_test()
{
    igr_test_setup setup("check-cycle2");

    auto check_result = run_dinitcheck("check-cycle2", {"-d", "sd"});
    igr_assert_eq(read_file_contents("./check-cycle2/expected.txt"), check_result.first);
    igr_assert(WEXITSTATUS(check_result.second) == 1, "dinitcheck exit status == 1");
}

void check_lint_test()
{
    igr_test_setup setup("check-lint");

    auto check_result = run_dinitcheck("check-lint", {"-d", "sd"});
    igr_assert_eq(read_file_contents("./check-lint/expected.txt"), check_result.first);
    igr_assert(WEXITSTATUS(check_result.second) == 1, "dinitcheck exit status == 1");
}

void reload1_test()
{
    igr_test_setup setup("reload1");

    std::string service_dir = setup.get_output_dir() + "/sd";
    auto cwd = getfullcwd();

    // This test requires reloading services after modifying a service description,
    // which for convenience we do by replacing the entire service directory (sd1 with sd2).
    // In order to do that we create a symlink to sd1 and use the link as the directory, then we retarget
    // the link at sd2.

    std::string sd_dir = setup.get_output_dir() + "/sd";
    unlink(sd_dir.c_str());
    if (symlink((cwd + "/reload1/sd1").c_str(), sd_dir.c_str()) == -1) {
        throw std::system_error(errno, std::generic_category(), "symlink");
    }

    dinit_proc dinit_p;
    dinit_p.start("reload1", {"-u", "-d", sd_dir.c_str(), "-p", igr_dinit_socket_path, "-q"}, true);

    // "dinitctl list" and check output
    dinitctl_proc dinitctl_p;
    dinitctl_p.start("reload1", {"-p", igr_dinit_socket_path, "list"});
    dinitctl_p.wait_for_term({1, 0}  /* max 1 second */);

    igr_assert_eq("", dinitctl_p.get_stderr());
    igr_assert_eq(read_file_contents("./reload1/initial.expected"), dinitctl_p.get_stdout());

    // Replace service directory with sd2
    unlink(sd_dir.c_str());
    if (symlink((cwd + "/reload1/sd2").c_str(), sd_dir.c_str()) == -1) {
        throw std::system_error(errno, std::generic_category(), "symlink");
    }

    // reload should fail: c not started but is a dependency in the new service description
    dinitctl_p.start("reload1", {"--quiet", "-p", igr_dinit_socket_path, "reload", "boot"});
    dinitctl_p.wait_for_term({1, 0}  /* max 1 second */);

    igr_assert_eq(read_file_contents("./reload1/output2.expected"), dinitctl_p.get_stderr());
    igr_assert_eq("", dinitctl_p.get_stdout());

    // if we start c, should then be able to do the reload
    dinitctl_p.start("reload1", {"--quiet", "-p", igr_dinit_socket_path, "start", "c"});
    dinitctl_p.wait_for_term({1, 0}  /* max 1 second */);
    igr_assert_eq("", dinitctl_p.get_stderr());
    igr_assert_eq("", dinitctl_p.get_stdout());

    dinitctl_p.start("reload1", {"--quiet", "-p", igr_dinit_socket_path, "reload", "boot"});
    dinitctl_p.wait_for_term({1, 0}  /* max 1 second */);
    igr_assert_eq("", dinitctl_p.get_stderr());
    igr_assert_eq("", dinitctl_p.get_stdout());

    // list again and check output
    dinitctl_p.start("reload1", {"-p", igr_dinit_socket_path, "list"});
    dinitctl_p.wait_for_term({1, 0}  /* max 1 second */);

    igr_assert_eq("", dinitctl_p.get_stderr());
    igr_assert_eq(read_file_contents("./reload1/output3.expected"), dinitctl_p.get_stdout());
}
