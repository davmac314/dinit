Building Dinit
=-=-=-=-=-=-=-

Building Dinit should be a straight-forward process. It requires GNU make and a C++11 compiler
(GCC version 4.9 and later, or Clang ~5.0 and later, should be fine).


Short version
=-=-=-=-=-=-=

Run "make" (or "gmake" if that is GNU make on your system). Your system type will hopefully be
detected automatically and appropriate configuration chosen, and Dinit will be built. Continue
reading instructions at "Running the test suite" or skip straight to "Installation".

If this fails, or if you are cross-compiling, read the "long version" instructions.


Long version 
=-=-=-=-=-=-

On the directly supported operating systems - Linux, OpenBSD, FreeBSD and Darwin (macOS) - a
suitable build configuration is provided and will be used automatically if no manual configuration
is supplied - skip directly to running "make" (more details below) if you are on one of these
systems and are happy to use the default configuration.

For other systems, the "configure" script will run and try to generate a suitable "mconfig" file
which specifies the build configuration. If you would prefer, you can run configure yourself
before running "make"; this allows you to specify various options and/or hand-edit the generated
mconfig file.

For more information on available options from the configure script, run:

    ./configure --help

As an alternative to running "configure", you can create and edit the "mconfig" file completely by
hand (or start by copying one for a particular OS from the "configs" directory) to choose
appropriate values for the configuration variables defined within. In particular:

  CXX      : should be set to the name of the C++ compiler (and link driver)
  CXXFLAGS : are options passed to the compiler during compilation
  CPPFLAGS : are preprocessor options to use during compilation (see note for GCC below)
  LDFLAGS  : are any options required for linking; should not normally be needed
             (FreeBSD requires -lrt; link time optimisation requires -flto and other flags).
  TEST_CXXFLAGS : are options passed to the compiler when compiling code for tests
  TEST_LDFLAGS  : are options to be used when linking test code
  
Additionally, for cross-compilation, the following can be specified:

  CXX_FOR_BUILD : C++ compiler for compiling code to run on the build host
  CXXFLAGS_FOR_BUILD  : any options for compiling code to run on the build host
  CPPFLAGS_FOR_BUILD  : any preprocessor options for code to run on the build host
  LDFLAGS_FOR_BUILD   : any options for linking code to run on the build host

Note that the "eg++" or "clang++" package must be installed on OpenBSD as the default "g++"
compiler is too old. Clang is part of the base system in recent releases.

Then, from the top-level directory, run "make" (or "gmake" if the system make is not GNU make,
such as on most BSD systems):

    make

If everything goes smoothly this will build dinit, dinitctl, and optionally the shutdown utility.
Use "make install" to install; you can specify an alternate installation root by setting the
"DESTDIR" variable, eg "make DESTDIR=/tmp/temporary-install-path install".

All of the above variables can be specified on the "make" command line, for example:

    make CXX=gcc

In addition to the above variables, the following can be specified on the command line (as a way
to specify additional options without removing the defaults):

    CXXFLAGS_EXTRA  : additional options to use when compiling code
    LDFLAGS_EXTRA   : additional options to use when linking
    TEST_CXXFLAGS_EXTRA  : additional options to use when compiling test code
    TEST_LDFLAGS_EXTRA   : additional options to use when linking tests

   
Recommended Compiler options
=-=-=-=-=-=-=-=-=-=-=-=-=-=-

Dinit should generally build fine with no additional options, other than:
 -std=c++11 :  may be required to select correct C++ standard.
 -D_GLIBCXX_USE_CXX11_ABI=1 :   see "Special note for GCC/Libstdc++", below. Not needed for
                                most modern systems.

Recommended options, supported by at least GCC and Clang, are:
 -Os       : optimise for size
 -fno-rtti : disable RTTI (run-time type information), it is not required by Dinit. However, on
             some platforms such as Mac OS (and historically FreeBSD, IIRC), this prevents
             exceptions working correctly.
 -fno-plt  : enables better code generation for non-static builds, but may cause unit test
             failures on some older versions of FreeBSD (eg 11.2-RELEASE-p4 with clang++ 6.0.0).
 -flto     : perform link-time optimisation (option required at compile and link).

Consult compiler documentation for further information on the above options.


Other configuration variables
=-=-=-=-=-=-=-=-=-=-=-=-=-=-=

There are a number of other variables you can set in the mconfig file which affect the build:

SBINDIR=...
    Where the "/sbin" directory is. Executables will be installed here.
MANDIR=...
    Where the "man" directory is. Man pages will be installed here.
SYSCONTROLSOCKET=...
    Default full path to the control socket, for when Dinit runs as system service manager.
BUILD_SHUTDOWN=yes|no
    Whether to build the "shutdown" (and "halt" etc) utilities. These are only useful
    if dinit is the system init (i.e. the PID 1 process). You probably don't want this
    unless building for Linux.
SHUTDOWN_PREFIX=...
    Name prefix for "shutdown", "halt" and "reboot" commands (if they are built). This affects
    both the output, and what command dinit will execute as part of system shutdown.
    If you want to install Dinit alongside another init system with its own shutdown/halt/reboot
    commands, set this (for eg. to "dinit-").
USE_UTMPX=1|0
    Whether to build support for manipulating the utmp/utmpx database via the related POSIX
    functions. This may be required (along with appropriate service configuration) for utilities
    like "who" to work correctly (the service configuration items "inittab-id" and "inittab-line"
    have no effect if this is disabled). If not set to any value, support is enabled for certain
    systems automatically and disabled for all others.
USE_INITGROUPS=1|0
    Whether to initialize supplementary groups for run-as services. The C API for this is not
    in POSIX, but is in practice supported on just about every relevant system, so it is enabled
    by default. If it is not supported on yours, you can explicitly disable it.
DEFAULT_AUTO_RESTART=true|false
    Enable/disable auto-restart for services by default. The default if unspecified is "true".
DEFAULT_START_TIMEOUT=XXX
    Specifies the time in seconds allowed for the service to start. If the service takes longer
    than this, service startup will be cancelled (service processes will be signalled to cause
    termination). The default if unspecified is 60 seconds. (The value can be overridden for
    individual services via the service description).
DEFAULT_STOP_TIMEOUT=XXX
    Specifies the time in seconds allowed for the service to stop. If the service takes longer than
    this, its process group is sent a SIGKILL signal which should cause it to terminate immediately.
    The default if unspecified is 10 seconds. (The value can be overridden for individual services
    via the service description).


Running test suite
=-=-=-=-=-=-=-=-=-

Build the "check" target in order to run the test suite:

    make check

The standard mconfig options enable various sanitizers during build of the tests. On Linux you may
see an error such as the following:

    make[3]: Leaving directory '/home/davmac/workspace/dinit/src/tests/cptests'
    ./tests
    ==25332==ERROR: AddressSanitizer failed to allocate 0xdfff0001000 (15392894357504) bytes at
    address 2008fff7000 (errno: 12)
    ==25332==ReserveShadowMemoryRange failed while trying to map 0xdfff0001000 bytes. Perhaps
    you're using ulimit -v
    make[2]: *** [Makefile:12: run-tests] Aborted

If you get this, either disable the address sanitizer or make sure you have overcommit enabled:

    echo 1 > /proc/sys/vm/overcommit_memory 

Any test failures will abort the test suite run immediately.

To run the integration tests:

    make check-igr
    
(The integration tests are more fragile than the unit tests, but give a better indication that
Dinit will actually work correctly on your system).

In addition to the standard test suite, there is experimental support for fuzzing the control
protocol handling using LLVM/clang's fuzzer (libFuzzer). Change to the `src/tests/cptests`
directory and build the "fuzz" target:

    make fuzz

Then create a "corpus" directory and run the fuzzer:

    mkdir corpus
    ./fuzz corpus

This will auto-generate test data as it finds input which triggers new execution paths. Check
libFuzzer documentation for further details.


Installation
=-=-=-=-=-=-

You can install using the "install" target:

    make install

If you want to install to an alternate root (eg for packaging purposes), specify that root via
DESTDIR:

    make DESTDIR=/some/path install

The dinit executable will be put in /sbin (or rather, in $DESTDIR/sbin), which may not be on the
path for normal users. Consider making a symbolic link to /usr/sbin/dinit.


Special note for GCC/Libstdc++
=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

(Note: the issue discussed here has apparently been resolved in recent GCC versions, with the fix
backported to GCC 6.x series and newer).

GCC 5.x onwards includes a "dual ABI" in its standard library implementation, aka Libstdc++.
Compiling against the newer (C++11 and later) ABI can be achieved by adding
-D_GLIBCXX_USE_CXX11_ABI=1 to the compiler command line; this uses a non-standard language
extension to differently mangle symbol names in order to link against the new ABI versions.

(Some systems may be configured to build with the new ABI by default, and in that case you build
against the old ABI using -D_GLIBCXX_USE_CXX11_ABI=0).

This is problematic for several reasons. First, it prevents linking against the new ABI with other
compilers that do not understand the language extension (LLVM i.e. clang++ does so in recent
versions, so this is perhaps no longer much of a problem in practice). Secondly, some aspects of
library behaviour are ABI-dependent but cannot be changed using the ABI macro; in particular,
exceptions thrown as a result of failed I/O operations are, in GCC versions 5.x and 6.x, always
"old ABI" exceptions which cannot be caught by code compiled against the new ABI, and in GCC
version 7.x they are always "new ABI" exceptions which cannot be caught by code compiled against
the old ABI. Since the one library object now supposedly houses both ABIs, this means that at
least one of the two ABIs is always broken.

A blog post describing the dual ABI mechanism can be found here:

    https://developers.redhat.com/blog/2015/02/05/gcc5-and-the-c11-abi/

The bug regarding the issue with catching other-ABI exceptions is here:

    https://gcc.gnu.org/bugzilla/show_bug.cgi?id=66145

Since Dinit is affected by this bug, the unfortunate possibility exists to break Dinit by
upgrading GCC. If you have libstdc++ corresponding to GCC 5.x or 6.x, you *must* build with the
old ABI, but Dinit will be broken if you upgrade to GCC 7. If you have libstdc++ from GCC 7, you
*must* build with the new ABI. If the wrong ABI is used, Dinit may still run successfully but any
attempt to load a non-existing service, for example, will cause Dinit to crash.
