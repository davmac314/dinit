#include <iostream>
#include <cassert>

#include "baseproc-sys.h"

#include "dinit-env.h"

void test_env_1()
{
    using namespace bp_sys;

    bp_sys::clearenv();

    environment env;

    assert(env.get("ABC").data() == nullptr);
    env.set_var("ABC=DEF");
    assert(strcmp(env.get("ABC").data(), "ABC=DEF") == 0);
    bp_sys::setenv("ABC","GHIJK",1);
    assert(strcmp(env.get("ABC").data(), "ABC=DEF") == 0);
    env.import_parent_var("ABC");
    assert(strcmp(env.get("ABC").data(), "ABC=GHIJK") == 0);
    env.clear_no_inherit();
    assert(env.get("ABC").data() == nullptr);
    env.import_parent_var("ABC");
    assert(strcmp(env.get("ABC").data(), "ABC=GHIJK") == 0);

    bp_sys::clearenv();
}

#define RUN_TEST(name, spacing) \
    std::cout << #name "..." spacing << std::flush; \
    name(); \
    std::cout << "PASSED" << std::endl;

int main(int argc, char **argv)
{
    RUN_TEST(test_env_1, "                ");
}
