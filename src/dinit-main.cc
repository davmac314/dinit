#include <iostream>
#include <system_error>
#include <new>

// Entry point for Dinit.

int dinit_main(int argc, char **argv);

int main(int argc, char **argv)
{
    try {
        return dinit_main(argc, argv);
    }
    catch (std::bad_alloc &badalloc) {
        std::cout << "dinit: out-of-memory during initialisation" << std::endl;
        return 1;
    }
    catch (std::system_error &syserr) {
        std::cout << "dinit: unexpected system error during initialisation: " << syserr.what() << std::endl;
        return 1;
    }
    catch (...) {
        std::cout << "dinit: unexpected error during initialisation" << std::endl;
        return 1;
    }
}
