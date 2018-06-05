#include <string>
#include <iostream>
#include <cassert>

#include "service.h"

std::string test_service_dir;

void init_test_service_dir()
{
    test_service_dir = "./test-services";
}

void test1()
{
    dirload_service_set sset(test_service_dir.c_str());
    auto t1 = sset.load_service("t1");
    assert(t1->get_name() == "t1");
}

#define RUN_TEST(name, spacing) \
    std::cout << #name "..." spacing; \
    name(); \
    std::cout << "PASSED" << std::endl;

int main(int argc, char **argv)
{
    init_test_service_dir();
    RUN_TEST(test1, "    ");
    return 0;
}
