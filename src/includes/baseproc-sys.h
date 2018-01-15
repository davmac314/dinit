/*
 * This header implements a namespace, bp_sys, which wraps various system calls used by baseproc-service.cc.
 *
 * When running tests, another header is substituted in place of this one. The substitute provides mocks/stubs
 * for the functions, to avoid calling the real functions and thus allow for unit-level testing.
 */

namespace bp_sys {

using dasynq::pipe2;

using ::fcntl;
using ::close;
using ::kill;
using ::getpgid;

}
