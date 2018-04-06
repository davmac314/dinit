#ifndef DASYNQ_CONFIG_H_INCLUDED
#define DASYNQ_CONFIG_H_INCLUDED

// You can customise Dasynq's build options in this file. Typically, you won't need to do anything; the
// defaults are sensible for a range of operating systems, though for some BSD family OSes you may need
// to explicitly define DASYNQ_HAVE_KQUEUE to 1. If neither epoll nor kqueue are available, the select-
// based backend is used, and DASYNQ_HAVE_PSELECT must be defined (to either 1 or 0, if pselect is or is
// not available, respectively).

// There are two parts to the file: the first is the custom configuration section, where you may specify
// custom settings, and the second section contains automatic configuration to fill in remaining settings
// based on known features in certain operating systems and compilers.

// ---------------------------------------------------------------------------------------------------------
// Part 1: Custom configuration, please edit to suit your system / requirements.
// ---------------------------------------------------------------------------------------------------------

// Specify custom configuration here. You can define the following to 1 if the
// corresponding feature is available and should be used, or to 0 otherwise.
//
// If the kqueue/kevent system calls are available:
//     #define DASYNQ_HAVE_KQUEUE 1
//
// If the epoll family of system calls are available:
//     #define DASYNQ_HAVE_EPOLL 1
//
// If the pipe2 system call is available:
//     #define HAVE_PIPE2 1
//
// If the pselect system call is available:
//     #define HAVE_PSELECT 1
//
// A tag to include at the end of a class body for a class which is allowed to have zero size.
// Normally, C++ mandates that all objects (except empty base subobjects) have non-zero size, but on some
// compilers (at least GCC and LLVM-Clang) there are tricks to get around this awkward limitation. Note that
// using this theoretically creates an ABI issue if two different compilers are used to compile different
// parts of the same program which both use Dasynq, if one supports zero-sized classes and the other does
// not; consider defining this empty here if that concerns you:
//    #define DASYNQ_EMPTY_BODY /* compiler specific! */
//
// A statement to tell the compiler that the current line of code is unreachable, that is, it will never be
// the case that program execution flow reaches this statement:
//     #define DASYNQ_UNREACHABLE /* compiler specific! */

// ---------------------------------------------------------------------------------------------------------
// Part 2: Automatic configuration begins here; you should not need to edit beyond this point.
// ---------------------------------------------------------------------------------------------------------

#if ! defined(DASYNQ_HAVE_KQUEUE)
#if defined(__OpenBSD__) || defined(__APPLE__) || defined(__FreeBSD__)
#define DASYNQ_HAVE_KQUEUE 1
#endif
#endif

#if DASYNQ_HAVE_KQUEUE && !defined(DASYNQ_KQUEUE_MACOS_WORKAROUND) && defined(__APPLE__)
// kqueue on macos has "issues". See extra/macos-kqueue-bug. There is an alternate Dasynq kqueue backend
// which avoids the issue, which is enabled via DASYNQ_KQUEUE_MACOS_WORKAROUND.
#define DASYNQ_KQUEUE_MACOS_WORKAROUND 1
#endif

#if ! defined(DASYNQ_HAVE_EPOLL)
#if defined(__linux__)
#define DASYNQ_HAVE_EPOLL 1
#endif
#endif

#if ! defined(DASYNQ_HAVE_PSELECT)
#if defined(__sortix__)
// Sortix doesn't have pselect yet (but has select):
#define DASYNQ_HAVE_PSELECT 0
#else
// POSIX actually requires pselect, so we otherwise assume it's available:
#define DASYNQ_HAVE_PSELECT 1
#endif
#endif

// General feature availability

#if (defined(__OpenBSD__) || defined(__linux__)) && ! defined(HAVE_PIPE2)
#define DASYNQ_HAVE_PIPE2 1
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

#if ! defined(DASYNQ_UNREACHABLE)
#define DASYNQ_UNREACHABLE          __builtin_unreachable()
#endif
#endif /* __GNUC__ */

#endif
