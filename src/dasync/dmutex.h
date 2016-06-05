#ifndef D_MUTEX_H_INCLUDED
#define D_MUTEX_H_INCLUDED

//#include <pthread.h>
#include <mutex>

namespace dasync {

// Simple non-recursive mutex, with priority inheritance to avoid priority inversion.
/*
class DMutex
{
    private:
    pthread_mutex_t mutex;
    
    public:
    DMutex()
    {
        // Avoid priority inversion by using PTHREAD_PRIO_INHERIT
        pthread_mutexattr_t attribs;
        pthread_mutexattr_init(&attribs);
        pthread_mutexattr_setprotocol(&attribs, PTHREAD_PRIO_INHERIT);
        pthread_mutex_init(&mutex, &attribs);
    }
    
    void lock()
    {
        pthread_mutex_lock(&mutex);
    }
    
    void unlock()
    {
        pthread_mutex_unlock(&mutex);
    }    
};
*/

using DMutex = std::mutex;

// A "null" mutex, for which locking / unlocking actually does nothing.
class NullMutex
{
    #ifdef __GNUC__
    #ifndef __clang__
    char empty[0];  // Make class instances take up no space (gcc)    
    #else
    char empty[0] __attribute__((unused));  // Make class instances take up no space (clang)
    #endif
    #endif

    public:
    void lock() { }
    void unlock() { }
    void try_lock() { }
};


} // end of namespace


#endif
