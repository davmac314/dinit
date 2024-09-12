/*
 * This header implements a namespace, bp_sys, which wraps various system calls used by baseproc-service.cc.
 *
 * When running tests, another header is substituted in place of this one. The substitute provides
 * mocks/stubs for the functions, to avoid calling the real functions and thus allow for unit-level testing.
 */

#ifndef BPSYS_INCLUDED
#define BPSYS_INCLUDED

#include <cstdlib> // getenv

#include <dasynq.h> // for pipe2

#include <sys/uio.h> // writev
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

extern char **environ;

namespace bp_sys {

using dasynq::pipe2;

using ::fcntl;
using ::open;
using ::close;
using ::kill;
using ::getpgid;
using ::tcsetpgrp;
using ::getpgrp;
using ::read;
using ::write;
using ::writev;
using ::waitid;
using std::getenv;

using ::environ;

}

#endif  // BPSYS_INCLUDED
