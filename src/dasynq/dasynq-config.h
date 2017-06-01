#ifndef DASYNQ_CONFIG_H_INCLUDED
#define DASYNQ_CONFIG_H_INCLUDED

#if defined(__OpenBSD__) || defined(__APPLE__) || defined(__FreeBSD__)
#define DASYNQ_HAVE_KQUEUE 1
#endif

#if defined(__linux__)
#define DASYNQ_HAVE_EPOLL 1
#endif

// General feature availability

#if defined(__OpenBSD__) || defined(__linux__)
#define HAVE_PIPE2 1
#endif


// Allow optimisation of empty classes by including this in the body:
// May be included as the last entry for a class which is only
// _potentially_ empty.

#ifdef __GNUC__
#ifndef __clang__
#define DASYNQ_EMPTY_BODY    char empty[0];  // Make class instances take up no space (gcc)
#else
#define DASYNQ_EMPTY_BODY    char empty[0] __attribute__((unused));  // Make class instances take up no space (clang)
#endif
#define DASYNQ_UNREACHABLE          __builtin_unreachable()
#endif

#endif
