#include <iostream>
#include <string>
#include <cassert>
#include <cstring>

#include "baseproc-sys.h"

#include "dinit-env.h"

#ifdef NDEBUG
#error "This file must be built with assertions ENABLED!"
#endif

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

void test_env_2()
{
    using namespace bp_sys;

    bp_sys::clearenv();

    environment env1; // parent
    environment env2;

    bp_sys::setenv("VAR1","VAR1-env",1); // value to be inherited right through
    bp_sys::setenv("VAR2","VAR2-env",1); // value to be overridden in env1
    bp_sys::setenv("VAR3","VAR3-env",1); // value to be undefined in env1
    bp_sys::setenv("VAR4","VAR4-env",1); // value to be undefined in env1 and overridden in env2
    bp_sys::setenv("VAR5","VAR5-env",1); // value to be overridden in env2
    bp_sys::setenv("VAR6","VAR6-env",1); // value to be overridden in both env1 and env2
    bp_sys::setenv("VAR7","VAR7-env",1); // value to be overridden env1 and undefined in env2
    bp_sys::setenv("VAR8","VAR8-env",1); // value to be undefined in env2

    env1.set_var("VAR2=VAR2-env1");
    env1.undefine_var("VAR3");
    env1.undefine_var("VAR4");
    env1.set_var("VAR6=VAR6-env1");
    env1.set_var("VAR7=VAR7-env1");

    env2.set_var("VAR4=VAR4-env2");
    env2.set_var("VAR5=VAR5-env2");
    env2.set_var("VAR6=VAR6-env2");
    env2.undefine_var("VAR7");
    env2.undefine_var("VAR8");

    environment::env_map mapping = env2.build(env1);

    assert(mapping.env_list.size() == 6); // 5 + null terminator

    auto checkvalue = [&](const std::string &name, const char *val) {
        for (const char *vv : mapping.env_list) {
            if (vv == nullptr) break;
            if (strncmp(vv, name.c_str(), name.size()) == 0 && vv[name.size()] == '=') {
                const char *vv_val = vv + name.size() + 1;
                assert(val != nullptr && "expected undefined variable, but has value");
                if (strcmp(vv_val, val) != 0) {
                    std::cout << "\nfor: " << name << ", expected: " << val << ", actual: " << vv_val << "\n";
                }
                assert(strcmp(vv_val, val) == 0 && "variable has wrong value");
                return;
            }
        }
        assert(val == nullptr && "expected defined variable, but variable not found");
    };

    checkvalue("VAR1", "VAR1-env");
    checkvalue("VAR2", "VAR2-env1");
    checkvalue("VAR3", nullptr);
    checkvalue("VAR4", "VAR4-env2");
    checkvalue("VAR5", "VAR5-env2");
    checkvalue("VAR6", "VAR6-env2");
    checkvalue("VAR7", nullptr);
    checkvalue("VAR8", nullptr);

    bp_sys::clearenv();
}

#define RUN_TEST(name, spacing) \
    std::cout << #name "..." spacing << std::flush; \
    name(); \
    std::cout << "PASSED" << std::endl;

int main(int argc, char **argv)
{
    RUN_TEST(test_env_1, "                ");
    RUN_TEST(test_env_2, "                ");
}
