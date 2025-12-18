Building Dinit
=-=-=-=-=-=-=-

Building Dinit should be a straight-forward process. It requires GNU make and a C++11 compiler
(GCC version 11 and later, or Clang ~7.0 and later, should be fine), as well as a handful of
utilities that should be available on any POSIX-compliant system; in particular, the "m4"
processor is required for the manual pages.


Short version
=-=-=-=-=-=-=

Run "make mconfig" (use "gmake mconfig" if GNU make is installed as "gmake"). Edit the generated
"mconfig" file to your liking; typically you will want to adjust SBINDIR and MANDIR, which control
the installation paths for executable files and  man pages respectively.

Run "make"/"gmake" to build. Your system type will hopefully be detected automatically and
appropriate configuration chosen, and Dinit will be built. Continue reading instructions at
"Running the test suite" or skip straight to "Installation".

If you run into any problems, or if you are cross-compiling, read the "long version" instructions.


Long version 
=-=-=-=-=-=-

On the directly supported operating systems - Linux, OpenBSD, FreeBSD and Darwin (including
macOS) - a suitable build configuration is provided and will be used automatically if no manual
configuration is supplied - skip directly to running "make" (more details below) if you are on one
of these systems and are happy to use the default configuration, but note that you will typically
at least want to alter the installation location.

If you are using a different system, or want to alter the default configuration, first run "make
mconfig" to generate the "mconfig" file which contains the build configuration. You can then edit
this file by hand, before proceeding with the build. Note that the the generated "mconfig" may be
a symbolic link to a system-specific default config file. It is not necessary to edit the file; you
can override variables on the "make" command line if you prefer.

An alternative to "make mconfig" is to use the provided "configure" script. See "'configure'
script", below.

You can also create and edit the "mconfig" file completely by hand (or start by copying one for a
particular OS from the "configs" directory) to choose appropriate values for the configuration
variables defined within.


"configure" script
=-=-=-=-=-=-=-=-=-

The provided "configure" script can be used to generate a suitable "mconfig" file, based on
sensible defaults and any options provided on the command line when the script is run. It also has
checks to enable/disable certain features based on the availability of their requirements (such as
presence of certain header files or libraries).

For more information on available options from the configure script, run:

    ./configure --help

If cross-compiling and using "configure", see the section "Cross-compilation with 'configure'
script" below.


Main build variables
=-=-=-=-=-=-=-=-=-=-

There are some build variables that may typically require adjustment. In particular, some
variables control installation paths or paths that are used at runtime; they are:

  BINDIR   : set to the directory where the executable files should be installed  
  SBINDIR  : set to the directory where the "system" executable files should be installed (i.e.
             shutdown and aliases will be installed here, if built).
  MANDIR   : set to the directory where manual pages should be installed 
  SYSCONTROLSOCKET : set to the default location of the control socket when Dinit is run as a system
                     manager (as opposed to a user service manager)

Some variables affect whether the "shutdown" utility is included and how it is named:

  BUILD_SHUTDOWN : (yes|no)
    Whether to build the "shutdown" (and "halt" etc) utilities. These are only useful
    if dinit is the system init (i.e. the PID 1 process). You probably don't want this
    unless building for Linux as shutdown is not supported on other systems (yet).
  SHUTDOWN_PREFIX :
    Name prefix for "shutdown", "halt" and "reboot" commands (if they are built). This affects
    both the output, and what command dinit will execute as part of system shutdown.
    If you want to install Dinit alongside another init system with its own shutdown/halt/reboot
    commands, set this (for eg. to "dinit-").

The following variables affect compilation and link options:

  CXX      : should be set to the name of the C++ compiler (and link driver)
  CXXFLAGS : are options passed to the compiler during compilation
  CPPFLAGS : are preprocessor options to use during compilation (see note for GCC below)
  LDFLAGS  : are any options required for linking; should not normally be needed
             (FreeBSD requires -lrt; link time optimisation requires -flto and other flags).
  TEST_CXXFLAGS : are options passed to the compiler when compiling code for tests
  TEST_LDFLAGS  : are options to be used when linking test code
  CPPFLAGS_LIBCAP : additional compiler options to include "libcap" headers on the header search
                    path (not normally necessary).
  LDFLAGS_LIBCAP : additional link options required to link with "libcap" (needed if capabilities
                   support is enabled).

For convenience, generated configuration also allows setting the following:

  LDFLAGS_BASE : any link options that should be used by default for linking (including tests),
                 if LDFLAGS is not overridden, to which CXXFLAGS will be added if the
                 configuration enables link-time optimisation (LTO). Will be ignored if LDFLAGS
                 is set.
  TEST_LDFLAGS_BASE : as LDFLAGS_BASE but for tests. The default is to use the same value (if any)
                      as specified for LDFLAGS_BASE. Ignored if TEST_LDFLAGS is set.

Together, LDFLAGS_BASE and TEST_LDFLAGS_BASE provide a simple way to adjust link options without
interfering with link-time optimisation (LTO). With LTO enabled, LDFLAGS must usually include the
same options used for compilation; by adjusting LDFLAGS_BASE instead, you do not have to
specifically include the compilation options as they will be included in LDFLAGS automatically.

For cross-compilation, the following can be specified:

  CXX_FOR_BUILD       : C++ compiler for code to run on the build host (i.e. non-cross compiler)
  CXXFLAGS_FOR_BUILD  : any options for compiling code to run on the build host
  CPPFLAGS_FOR_BUILD  : any preprocessor options for code to run on the build host
  LDFLAGS_FOR_BUILD   : any options for linking code to run on the build host

Note that the "eg++" or "clang++" package may need to be installed on OpenBSD if the default "g++"
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

   
Recommended compiler options
=-=-=-=-=-=-=-=-=-=-=-=-=-=-

Dinit should generally build fine with no additional options, other than:
 -std=c++11 :  may be required to select correct C++ standard.

Recommended options, supported by at least GCC and Clang, are:
 -Os       : optimise for size
 -fno-rtti : disable generation of RTTI (run-time type information) for types for which it should
             not be required by the runtime (i.e. for types not thrown/caught as exceptions).
             Dinit does not require RTTI other than for exceptions, and this option reduces the
             size of binaries slightly. However, it is not always safe to use this option; see
             "Special note for the Clang compiler and the Libcxxrt/Libcxxabi runtime".
 -fno-plt  : enables better code generation for non-static builds, but may cause unit test
             failures on some older versions of FreeBSD (eg 11.2-RELEASE-p4 with clang++ 6.0.0).
 -flto     : perform link-time optimisation (option required at compile and link).

Consult compiler documentation for further information on the above options.


Cross-compilation with "configure" script
=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=

If cross-compiling (i.e. specifying the CXX_FOR_BUILD variable), there are some additional
considerations regarding use of the "configure" script.  

Unless compiler arguments to use are specified when "configure" is invoked, the script will check
for compiler support of certain options (and enable compilation with these options, if they are
available). One such check is for compiler support for the "-fno-rtti" compiler option (see
"Recommended compiler options", above). Unlike checks for other options, this check involves
compiling a test program and running it to ensure that the option works correctly, because it is
known not to do so in some cases (see "Special note for the Clang compiler and the Libcxxrt C++
runtime", below, for details). However, when cross-compiling, because of the inability to run the
test program on the target machine, this check cannot be performed. In this case, "configure"
disables use of "-fno-rtti" and outputs this warning:

    Cross-compilation detected. Disabling -fno-rtti to avoid a compiler bug on some platforms.

With the option disabled, the generated binaries will be slightly larger.


Other configuration variables
=-=-=-=-=-=-=-=-=-=-=-=-=-=-=

There are a number of other variables you can set in the mconfig file which affect the build:

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
DEFAULT_AUTO_RESTART=ALWAYS|ON_FAILURE|NEVER
    Default for when to automatically restart services. This controls the default for the
    "restart" service setting. ON_FAILURE restarts process/bgprocess services only if they exit
    with a non-zero status or are terminated via (most) signals; for other service types it
    behaves the same as NEVER. See documentation (dinit-service(5) man page) for details.
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
SUPPORT_CGROUPS=1|0
    Whether to include support for cgroups (Linux only).
SUPPORT_CAPABILITIES=1|0
    Whether to include support for capabilities (Linux only; requires libcap).
SUPPORT_IOPRIO=1|0
    Whether to include support for adjusting IO priority (Linux only).
SUPPORT_OOMADJ=1|0
    Whether to include support for adusting OOM-killer score adjustment (Linux only).


Running the test suite
=-=-=-=-=-=-=-=-=-=-=-

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

Note that if you specify installation paths via variables on the "make" command line, you should
specify the same values for both build and install steps.  


Special note for the Clang compiler and the Libcxxrt/Libcxxabi runtime
=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

Clang when used as a compiler together with the Libcxxrt or Libcxxabi C++ runtime library on
certain platforms has an issue that prevents exceptions from working properly in particular cases
when the "-fno-rtti" compiler option (see "Recommended compiler options") is used. This issue
can prevent Dinit from working correctly. The known platforms exhibiting this problem are FreeBSD,
OpenBSD and macOS; it may affect other *BSD variants using Clang, but this hasn't been verified.

Some details regarding the issue can be found here:

    https://github.com/llvm/llvm-project/issues/66117

It's recommended not to use the "-fno-rtti" compiler option when building Dinit on the mentioned
platforms. As a consequence of not using this option, the output binary will be larger.
