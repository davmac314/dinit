#ifndef DINIT_SERVICE_DIR_H
#define DINIT_SERVICE_DIR_H 1

#include <vector>

// A service directory entry, tracking the directory as a nul-terminated string, which may either
// be static or dynamically allocated (via new char[...]).
class dir_entry
{
    const char *dir;
    bool dir_dyn_allocd;  // dynamically allocated?

    public:
    dir_entry(const char *dir_p, bool dir_dyn_allocd_p) :
        dir(dir_p), dir_dyn_allocd(dir_dyn_allocd_p)
    { }

    dir_entry(dir_entry &&other)
    {
        dir = other.dir;
        dir_dyn_allocd = other.dir_dyn_allocd;
        other.dir_dyn_allocd = false;
    }

    dir_entry(const dir_entry &other) = delete;

    ~dir_entry()
    {
        if (dir_dyn_allocd) {
            delete[] dir;
        }
    }

    const char *get_dir() const
    {
        return dir;
    }
};

using service_dir_pathlist = std::vector<dir_entry>;

#endif
