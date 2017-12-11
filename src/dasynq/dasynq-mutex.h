#ifndef DASYNQ_MUTEX_H_INCLUDED
#define DASYNQ_MUTEX_H_INCLUDED

#include <mutex>

namespace dasynq {

// A "null" mutex, for which locking / unlocking actually does nothing.
class null_mutex
{
    public:
    void lock() { }
    void unlock() { }
    void try_lock() { }

	private:
    DASYNQ_EMPTY_BODY;
};

} // end of namespace

#endif
