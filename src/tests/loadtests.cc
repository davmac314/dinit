#include <string>
#include <iostream>
#include <cassert>
#include <cstdlib>
#include <cstring>

#include "service.h"
#include "proc-service.h"

std::string test_service_dir;

void init_test_service_dir()
{
    test_service_dir = "./test-services";
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
    setenv("ONEVAR", "a", true);
    setenv("TWOVAR", "hellohello", true);
    // leave THREEVAR undefined
    auto t2 = static_cast<base_process_service *>(sset.load_service("t2"));
    auto exec_parts = t2->get_exec_arg_parts();
    assert(strcmp("echo", exec_parts[0]) == 0);
    assert(strcmp("a", exec_parts[1]) == 0);
    assert(strcmp("hellohello", exec_parts[2]) == 0);
    assert(strcmp("", exec_parts[3]) == 0);
}

#define RUN_TEST(name, spacing) \
    std::cout << #name "..." spacing; \
    name(); \
    std::cout << "PASSED" << std::endl;

int main(int argc, char **argv)
{
    init_test_service_dir();
    RUN_TEST(test_basic, "    ");
    RUN_TEST(test_env_subst, "");
    return 0;
}
