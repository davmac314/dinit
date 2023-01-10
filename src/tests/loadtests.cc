#include <string>
#include <iostream>
#include <sstream>
#include <cassert>
#include <cstdlib>
#include <cstring>

#include "service.h"
#include "proc-service.h"
//#include "load-service.h"

std::string test_service_dir;

environment tenv;
environment::env_map tenvmap;

void init_test_service_dir()
{
    test_service_dir = "./test-services";
    tenvmap = tenv.build(main_env);
}

void test_basic()
{
    dirload_service_set sset(test_service_dir.c_str());
    auto t1 = sset.load_service("t1");
    assert(t1->get_name() == "t1");
}

void test_env_subst()
{
    dirload_service_set sset(test_service_dir.c_str());
    bp_sys::setenv("ONEVAR", "a", true);
    bp_sys::setenv("TWOVAR", "hellohello", true);
    bp_sys::setenv("THREEVAR", "", true);
    // leave THREEVAR undefined
    auto t2 = static_cast<base_process_service *>(sset.load_service("t2"));
    auto exec_parts = t2->get_exec_arg_parts();
    assert(strcmp("echo", exec_parts[0]) == 0);
    assert(strcmp("a", exec_parts[1]) == 0); // $ONEVAR
    assert(strcmp("a", exec_parts[2]) == 0); // ${ONEVAR}
    assert(strcmp("b", exec_parts[3]) == 0); // ${ONEVAR+b}
    assert(strcmp("b", exec_parts[4]) == 0); // ${ONEVAR:+b}
    assert(strcmp("hellohello", exec_parts[5]) == 0); // $TWOVAR
    assert(strcmp("hellohello", exec_parts[6]) == 0); // ${TWOVAR}
    assert(strcmp("hellohello", exec_parts[7]) == 0); // ${TWOVAR-world}
    assert(strcmp("hellohello", exec_parts[8]) == 0); // ${TWOVAR:-world}
    assert(strcmp("",      exec_parts[9]) == 0); // $THREEVAR
    assert(strcmp("",      exec_parts[10]) == 0); // ${THREEVAR}
    assert(strcmp("empty", exec_parts[11]) == 0); // ${THREEVAR+empty}
    assert(strcmp("",      exec_parts[12]) == 0); // ${THREEVAR:+empty}
    assert(strcmp("",      exec_parts[13]) == 0); // ${THREEVAR-empty}
    assert(strcmp("empty", exec_parts[14]) == 0); // ${THREEVAR:-empty}
    assert(strcmp("",       exec_parts[15]) == 0); // $FOURVAR
    assert(strcmp("",       exec_parts[16]) == 0); // ${FOURVAR}
    assert(strcmp("",       exec_parts[17]) == 0); // ${FOURVAR+empty2}
    assert(strcmp("",       exec_parts[18]) == 0); // ${FOURVAR:+empty2}
    assert(strcmp("empty2", exec_parts[19]) == 0); // ${FOURVAR-empty2}
    assert(strcmp("empty2", exec_parts[20]) == 0); // ${FOURVAR:-empty2}
}

void test_env_subst2()
{
    auto resolve_env_var = [](const std::string &name, environment::env_map const &) {
        if (name == "ONE_VAR") return "a";
        if (name == "TWOVAR") return "hellohello";
        return "";
    };

    std::string line = "test x$ONE_VAR-${ONE_VAR}~ y$${TWOVAR}$TWOVAR$$ONE_VAR";
    std::list<std::pair<unsigned,unsigned>> offsets;
    std::string::iterator li = line.begin();
    std::string::iterator le = line.end();
    dinit_load::read_setting_value(1 /* line_num */, li, le, &offsets);

    dinit_load::value_var_subst("command", line, offsets, resolve_env_var, tenvmap);

    assert(line == "test xa-a~ y${TWOVAR}hellohello$ONE_VAR");

    assert(offsets.size() == 3);
    assert((*std::next(offsets.begin(), 0) == std::pair<unsigned,unsigned>{0, 4}));
    assert((*std::next(offsets.begin(), 1) == std::pair<unsigned,unsigned>{5, 10}));
    assert((*std::next(offsets.begin(), 2) == std::pair<unsigned,unsigned>{11, 39}));
}

void test_nonexistent()
{
    bool got_service_not_found = false;
    dirload_service_set sset(test_service_dir.c_str());
    try {
        sset.load_service("does-not-exist");
    }
    catch (service_not_found &) {
        got_service_not_found = true;
    }
    assert(got_service_not_found);
}

// test_prelim_dep: A preliminary (unresolved) service dependency
class test_prelim_dep
{
    public:
    std::string name;
    dependency_type dep_type;

    test_prelim_dep(const std::string &name_p, dependency_type dep_type_p)
        : name(name_p), dep_type(dep_type_p) { }
    test_prelim_dep(std::string &&name_p, dependency_type dep_type_p)
        : name(std::move(name_p)), dep_type(dep_type_p) { }
};

void test_settings()
{
    using string = std::string;
    using string_iterator = std::string::iterator;

    using prelim_dep = test_prelim_dep;

    dinit_load::service_settings_wrapper<prelim_dep> settings;

    std::stringstream ss;

    ss << "type = process\n"
            "command = /something/test\n"
            "depends-on = abc\n"
            "rlimit-nofile = 50:100\n"
            "rlimit-core = 60:\n"
            "rlimit-data = -:-";

    try {
        process_service_file("test-service", ss,
                [&](string &line, unsigned line_num, string &setting, string_iterator &i, string_iterator &end) -> void {

            auto process_dep_dir_n = [&](std::list<prelim_dep> &deplist, const std::string &waitsford,
                    dependency_type dep_type) -> void {
                //process_dep_dir(name.c_str(), service_filename, deplist, waitsford, dep_type);
            };

            auto load_service_n = [&](const string &dep_name) -> const string & {
                return dep_name;
            };

            try {
                process_service_line(settings, "test-service", line, line_num, setting, i, end, load_service_n, process_dep_dir_n);
            }
            catch (service_description_exc &exc) {
                //report_service_description_exc(exc);
            }
        });
    }
    catch (std::system_error &sys_err)
    {
        //report_error(sys_err, name);
        throw service_description_exc("", "error while reading service description.", "unknown");
    }

    assert(settings.service_type == service_type_t::PROCESS);
    assert(settings.command == "/something/test");
    assert(settings.rlimits.size() == 3);

    assert(settings.rlimits[0].resource_id == RLIMIT_NOFILE);
    assert(settings.rlimits[0].soft_set && settings.rlimits[0].hard_set);
    assert(settings.rlimits[0].limits.rlim_cur == 50);
    assert(settings.rlimits[0].limits.rlim_max == 100);

    assert(settings.rlimits[1].resource_id == RLIMIT_CORE);
    assert(settings.rlimits[1].soft_set && !settings.rlimits[1].hard_set);
    assert(settings.rlimits[1].limits.rlim_cur == 60);

    assert(settings.rlimits[2].resource_id == RLIMIT_DATA);
    assert(settings.rlimits[2].soft_set && settings.rlimits[2].hard_set);
    assert(settings.rlimits[2].limits.rlim_cur == RLIM_INFINITY);
    assert(settings.rlimits[2].limits.rlim_max == RLIM_INFINITY);

    assert(settings.depends.size() == 1);
    assert(settings.depends.front().dep_type == dependency_type::REGULAR);
    assert(settings.depends.front().name == "abc");
}

void test_path_env_subst()
{
    using string = std::string;
    using string_iterator = std::string::iterator;

    using prelim_dep = test_prelim_dep;

    dinit_load::service_settings_wrapper<prelim_dep> settings;

    std::stringstream ss;

    ss << "type = process\n"
            "command = /something/test\n"
            "logfile = /some/$username/dir\n";

    try {
        process_service_file("test-service", ss,
                [&](string &line, unsigned line_num, string &setting, string_iterator &i, string_iterator &end) -> void {

            auto process_dep_dir_n = [&](std::list<prelim_dep> &deplist, const std::string &waitsford,
                    dependency_type dep_type) -> void {
                //process_dep_dir(name.c_str(), service_filename, deplist, waitsford, dep_type);
            };

            auto load_service_n = [&](const string &dep_name) -> const string & {
                return dep_name;
            };

            try {
                process_service_line(settings, "test-service", line, line_num, setting, i, end, load_service_n, process_dep_dir_n);
            }
            catch (service_description_exc &exc) {
                //report_service_description_exc(exc);
            }
        });
    }
    catch (std::system_error &sys_err)
    {
        throw service_description_exc("", "error while reading service description.", "unknown");
    }

    auto report_error = [](const char *msg) {};

    auto resolve_var = [](const std::string &name, environment::env_map const &) -> const char * {
        if (name == "username") return "testsuccess";
        return nullptr;
    };

    settings.finalise(report_error, tenvmap, report_error /* lint */, resolve_var);

    assert(settings.service_type == service_type_t::PROCESS);
    assert(settings.command == "/something/test");
    assert(settings.logfile == "/some/testsuccess/dir");
}

#define RUN_TEST(name, spacing) \
    std::cout << #name "..." spacing << std::flush; \
    name(); \
    std::cout << "PASSED" << std::endl;

int main(int argc, char **argv)
{
    init_test_service_dir();
    RUN_TEST(test_basic, "                ");
    RUN_TEST(test_env_subst, "            ");
    RUN_TEST(test_env_subst2, "           ");
    RUN_TEST(test_nonexistent, "          ");
    RUN_TEST(test_settings, "             ");
    RUN_TEST(test_path_env_subst, "       ");
    bp_sys::clearenv();
    return 0;
}
