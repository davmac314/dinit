#include <string>
#include <iostream>
#include <sstream>
#include <cassert>
#include <cstdlib>
#include <cstring>

#include "service.h"
#include "proc-service.h"
#include "load-service.h"

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
    // leave FOURVAR undefined
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
    auto resolve_env_var = [](const std::string &name) {
        if (name == "ONE_VAR") return "a";
        if (name == "TWOVAR") return "hellohello";
        return "";
    };

    std::string line = "test x$ONE_VAR-${ONE_VAR}~ y$${TWOVAR}$TWOVAR$$ONE_VAR";
    std::list<std::pair<unsigned,unsigned>> offsets;
    std::string::iterator li = line.begin();
    std::string::iterator le = line.end();

    std::string file_name = "dummy";
    file_pos_ref fpr { file_name, 1 };

    dinit_load::read_setting_value(fpr, li, le, &offsets);

    dinit_load::value_var_subst("command", line, offsets, resolve_env_var, nullptr);

    assert(line == "test xa-a~ y${TWOVAR}hellohello$ONE_VAR");

    assert(offsets.size() == 3);
    assert((*std::next(offsets.begin(), 0) == std::pair<unsigned,unsigned>{0, 4}));
    assert((*std::next(offsets.begin(), 1) == std::pair<unsigned,unsigned>{5, 10}));
    assert((*std::next(offsets.begin(), 2) == std::pair<unsigned,unsigned>{11, 39}));
}

void test_env_subst3()
{
    auto resolve_env_var = [](const std::string &name) {
        if (name == "EMPTY") return "";
        if (name == "WS") return "    ";
        if (name == "PADDED") return " p ";
        return "";
    };

    std::string file_name = "dummy";
    file_pos_ref fpr { file_name, 1 };

    std::string line = "test $/EMPTY foo";
    std::list<std::pair<unsigned,unsigned>> offsets;
    std::string::iterator li = line.begin();
    std::string::iterator le = line.end();
    dinit_load::read_setting_value(fpr, li, le, &offsets);
    dinit_load::value_var_subst("command", line, offsets, resolve_env_var, nullptr);

    auto check_arg = [&](unsigned idx, const char *val)
    {
        auto &offs = *std::next(offsets.begin(), idx);
        assert(line.substr(offs.first, offs.second - offs.first) == val);
    };

    assert(line == "test  foo");
    check_arg(1, "foo");

    line = "test $EMPTY foo";
    li = line.begin(); le = line.end(); offsets.clear();
    dinit_load::read_setting_value(fpr, li, le, &offsets);
    dinit_load::value_var_subst("command", line, offsets, resolve_env_var, nullptr);

    assert(line == "test  foo");
    check_arg(1, "");
    check_arg(2, "foo");

    // adjacent collapsing
    line = "test $/EMPTY$/EMPTY$/EMPTY foo";
    li = line.begin(); le = line.end(); offsets.clear();
    dinit_load::read_setting_value(fpr, li, le, &offsets);
    dinit_load::value_var_subst("command", line, offsets, resolve_env_var, nullptr);

    assert(line == "test  foo");
    check_arg(1, "foo");

    // middle empty is non-collapsing:
    line = "test $/EMPTY$EMPTY$/EMPTY foo";
    li = line.begin(); le = line.end(); offsets.clear();
    dinit_load::read_setting_value(fpr, li, le, &offsets);
    dinit_load::value_var_subst("command", line, offsets, resolve_env_var, nullptr);

    assert(line == "test  foo");
    check_arg(1, "");
    check_arg(2, "foo");

    // empty doesn't wordsplit:
    line = "test abc$/{EMPTY}def";
    li = line.begin(); le = line.end(); offsets.clear();
    dinit_load::read_setting_value(fpr, li, le, &offsets);
    dinit_load::value_var_subst("command", line, offsets, resolve_env_var, nullptr);

    assert(line == "test abcdef");
    check_arg(1, "abcdef");

    // whitespace does wordsplit:
    line = "test abc$/{WS}def";
    li = line.begin(); le = line.end(); offsets.clear();
    dinit_load::read_setting_value(fpr, li, le, &offsets);
    dinit_load::value_var_subst("command", line, offsets, resolve_env_var, nullptr);

    assert(line == "test abc def");
    check_arg(1, "abc");
    check_arg(2, "def");

    // internal words handled correctly:
    line = "test abc$/{PADDED}def";
    li = line.begin(); le = line.end(); offsets.clear();
    dinit_load::read_setting_value(fpr, li, le, &offsets);
    dinit_load::value_var_subst("command", line, offsets, resolve_env_var, nullptr);

    assert(line == "test abc p def");
    check_arg(1, "abc");
    check_arg(2, "p");
    check_arg(3, "def");
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

    file_input_stack input_stack;
    input_stack.add_source(ss.str(), "dummy");

    try {
        auto resolve_var = [](const std::string &name) {
            return (char *)nullptr;
        };

        process_service_file("test-service", input_stack,
                [&](string &line, file_pos_ref input_pos, string &setting,
                        dinit_load::setting_op_t op, string_iterator &i,
                        string_iterator &end) -> void {

                    auto process_dep_dir_n = [&](std::list<prelim_dep> &deplist,
                            const std::string &waitsford, dependency_type dep_type) -> void {
                        //process_dep_dir(name.c_str(), service_filename, deplist, waitsford, dep_type);
                    };

                    auto load_service_n = [&](const string &dep_name) -> const string & {
                        return dep_name;
                    };

                    try {
                        process_service_line(settings, "test-service", nullptr, line, input_pos,
                                setting, op, i, end, load_service_n, process_dep_dir_n);
                    }
                    catch (service_description_exc &exc) {
                        //report_service_description_exc(exc);
                    }
                },
                nullptr, resolve_var);
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
            "logfile = /some/$1/$username/${1}/dir\n";

    file_input_stack input_stack;
    input_stack.add_source(ss.str(), "dummy");

    try {
        auto resolve_var = [](const std::string &name) {
            return (char *)nullptr;
        };

        process_service_file("test-service", input_stack,
                [&](string &line, file_pos_ref input_pos, string &setting,
                        dinit_load::setting_op_t op, string_iterator &i,
                        string_iterator &end) -> void {

                    auto process_dep_dir_n = [&](std::list<prelim_dep> &deplist,
                            const std::string &waitsford, dependency_type dep_type) -> void {
                        //process_dep_dir(name.c_str(), service_filename, deplist, waitsford, dep_type);
                    };

                    auto load_service_n = [&](const string &dep_name) -> const string & {
                        return dep_name;
                    };

                    try {
                        process_service_line(settings, "test-service", nullptr, line, input_pos,
                                setting, op, i, end, load_service_n, process_dep_dir_n);
                    }
                    catch (service_description_exc &exc) {
                        //report_service_description_exc(exc);
                    }
                },
                nullptr, resolve_var);
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

    settings.finalise(report_error, tenvmap, "foo", report_error /* lint */, resolve_var);

    assert(settings.service_type == service_type_t::PROCESS);
    assert(settings.command == "/something/test");
    assert(settings.logfile == "/some/foo/testsuccess/foo/dir");
}

void test_newline()
{
    dirload_service_set sset(test_service_dir.c_str());
    bp_sys::setenv("arg", "t3", true);
    auto t3 = static_cast<base_process_service *>(sset.load_service("t3"));
    auto exec_parts = t3->get_exec_arg_parts();
    assert(t3->get_type() == service_type_t::PROCESS);
    assert(strcmp(exec_parts[0], "command1") == 0);
    assert(strcmp(exec_parts[1], "t3") == 0);
    assert(strcmp(exec_parts[2], "arg1") == 0);
    assert(strcmp(exec_parts[3], "command2") == 0);
    assert(strcmp(exec_parts[4], "t3") == 0);
    assert(strcmp(exec_parts[5], "arg2") == 0);
    assert(strcmp(exec_parts[6], "command3") == 0);
    assert(strcmp(exec_parts[7], "t3") == 0);
    assert(strcmp(exec_parts[8], "arg3") == 0);
}

void test_newline_err()
{
    using string = std::string;
    using string_iterator = std::string::iterator;
    using sstream = std::stringstream;
    using prelim_dep = test_prelim_dep;

    unsigned errcount = 0;

    auto test_inner = [&](std::stringstream &ss)
    {
        dinit_load::service_settings_wrapper<prelim_dep> settings;

        file_input_stack input_stack;
        input_stack.add_source(ss.str(), "dummy");

        auto resolve_var = [](const std::string &name) {
            return (char *)nullptr;
        };

        process_service_file("test-service", input_stack,
                [&](string &line, file_pos_ref input_pos, string &setting,
                        dinit_load::setting_op_t op, string_iterator &i,
                        string_iterator &end) -> void {

                    auto process_dep_dir_n = [&](std::list<prelim_dep> &deplist,
                            const std::string &waitsford, dependency_type dep_type) -> void {
                        //process_dep_dir(name.c_str(), service_filename, deplist, waitsford, dep_type);
                    };

                    auto load_service_n = [&](const string &dep_name) -> const string & {
                        return dep_name;
                    };

                    process_service_line(settings, "test-service", nullptr, line, input_pos,
                            setting, op, i, end, load_service_n, process_dep_dir_n);
                },
                nullptr, resolve_var);
    };

    try {
        sstream ss;
        ss << "type = process\n"
                "command = /something/test\\\n"
                " # comment with leading space\\\n"
                "# comment without leading space";
        test_inner(ss);
    }
    catch (service_description_exc &exc)
    {
        if (exc.input_pos.get_line_num() == 4) ++errcount;
    }

    try {
        sstream ss;
        ss << "type = process\n"
                "command = EOF\\";
        test_inner(ss);
    }
    catch (service_description_exc &exc)
    {
        if (exc.input_pos.get_line_num() == 2) ++errcount;
    }

    assert(errcount == 2);
}

void test_newline2()
{
    using string = std::string;
    using string_iterator = std::string::iterator;

    using prelim_dep = test_prelim_dep;

    dinit_load::service_settings_wrapper<prelim_dep> settings;

    std::stringstream ss;

    ss << "type = process\n"
            "command = /something/test\\\\\n"  // even number of backslashes
            "stop-command = /something/stop\\\\\\\n"  // odd number (3) backslashes
            " next line\n";

    file_input_stack input_stack;
    input_stack.add_source(ss.str(), "dummy");

    try {
        auto resolve_var = [](const std::string &name) {
            return (char *)nullptr;
        };

        process_service_file("test-service", input_stack,
                [&](string &line, file_pos_ref input_pos, string &setting,
                        dinit_load::setting_op_t op, string_iterator &i,
                        string_iterator &end) -> void {

                    auto process_dep_dir_n = [&](std::list<prelim_dep> &deplist,
                            const std::string &waitsford,
                            dependency_type dep_type) -> void {
                        //process_dep_dir(name.c_str(), service_filename, deplist, waitsford, dep_type);
                    };

                    auto load_service_n = [&](const string &dep_name) -> const string & {
                        return dep_name;
                    };

                    try {
                        process_service_line(settings, "test-service", nullptr, line, input_pos,
                                setting, op, i, end, load_service_n, process_dep_dir_n);
                    }
                    catch (service_description_exc &exc) {
                        //report_service_description_exc(exc);
                    }
                },
                nullptr, resolve_var);
    }
    catch (std::system_error &sys_err)
    {
        //report_error(sys_err, name);
        throw service_description_exc("", "error while reading service description.", "unknown");
    }

    assert(settings.command == "/something/test\\");
    assert(settings.stop_command == "/something/stop\\ next line");
}

void test_comments()
{
    std::string file_name = "dummy";
    file_pos_ref input_pos { file_name, 1 };

    std::string line = "one two three # comment";
    std::list<std::pair<unsigned,unsigned>> offsets;
    std::string::iterator li = line.begin();
    std::string::iterator le = line.end();
    std::string val = dinit_load::read_setting_value(input_pos, li, le, &offsets);

    assert(val == "one two three");
    assert(offsets.size() == 3);

    auto i = offsets.begin();
    assert(i->first == 0);
    assert(i->second == 3);
    ++i;
    assert(i->first == 4);
    assert(i->second == 7);
    ++i;
    assert(i->first == 8);
    assert(i->second == 13);
}

void test_plusassign()
{
    std::string file_name = "dummy";
    file_pos_ref input_pos { file_name, 1 };

    std::string setting_val;
    std::list<std::pair<unsigned,unsigned>> part_positions;

    std::string primary = "echo one";
    std::string::iterator i = primary.begin();
    std::string::iterator e = primary.end();

    dinit_load::read_setting_value(setting_val, dinit_load::setting_op_t::ASSIGN, input_pos, i, e, &part_positions);

    assert(setting_val == "echo one");
    assert(part_positions.size() == 2);

    std::string altered = "echo two";
    i = altered.begin();
    e = altered.end();

    dinit_load::read_setting_value(setting_val, dinit_load::setting_op_t::ASSIGN, input_pos, i, e, &part_positions);

    assert(setting_val == "echo two");
    assert(part_positions.size() == 2);

    std::string addendum = "three";
    i = addendum.begin();
    e = addendum.end();

    dinit_load::read_setting_value(setting_val, dinit_load::setting_op_t::PLUSASSIGN, input_pos, i, e, &part_positions);

    assert(setting_val == "echo two three");
    assert(part_positions.size() == 3);
    auto ppi = part_positions.begin();
    ++ppi; ++ppi;
    assert(ppi->first == 9);
    assert(ppi->second == 14);

    std::string s2 = "echo \"space \"";
    i = s2.begin();
    e = s2.end();

    dinit_load::read_setting_value(setting_val, dinit_load::setting_op_t::ASSIGN, input_pos, i, e, &part_positions);

    assert(setting_val == "echo space ");

    i = addendum.begin();
    e = addendum.end();

    dinit_load::read_setting_value(setting_val, dinit_load::setting_op_t::PLUSASSIGN, input_pos, i, e, &part_positions);

    assert(setting_val == "echo space  three");
    assert(part_positions.size() == 3);
    ppi = part_positions.begin();
    ++ppi;
    assert(ppi->first == 5);
    assert(ppi->second == 11);
    ++ppi;
    assert(ppi->first == 12);
    assert(ppi->second == 17);
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
    RUN_TEST(test_env_subst3, "           ");
    RUN_TEST(test_nonexistent, "          ");
    RUN_TEST(test_settings, "             ");
    RUN_TEST(test_path_env_subst, "       ");
    RUN_TEST(test_newline, "              ");
    RUN_TEST(test_newline_err, "          ");
    RUN_TEST(test_newline2, "             ");
    RUN_TEST(test_comments, "             ");
    RUN_TEST(test_plusassign, "           ");
    bp_sys::clearenv();
    return 0;
}
