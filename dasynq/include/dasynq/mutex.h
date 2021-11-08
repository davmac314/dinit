#ifndef DASYNQ_MUTEX_H_
#define DASYNQ_MUTEX_H_

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

} // namespace dasynq

#endif /* DASYNQ_MUTEX_H_ */
