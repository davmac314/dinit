Building Dinit
=-=-=-=-=-=-=-

Building Dinit should be a straight-forward process. It requires GNU make.

Edit the "mconfig" file to choose appropriate values for the configuration variables defined
within. In particular:

  CXX : should be set to the name of the C++ compiler (and linker)
  CXXOPTS :  are options passed to the compiler during compilation (see note for GCC below)
  LDFLAGS :  are any extra flags required for linking; should not normally be needed
             (FreeBSD requires -lrt).

Defaults for Linux and OpenBSD are provided. Note that the "eg++" or "clang++" package must
be installed on OpenBSD as the default "g++" compiler is too old. Clang is part of the base
system in recent releases.

Then, change into the "src" directory, and run "make" (or "gmake" if the system make is not
GNU make, such as on most BSD systems):

    cd src
    make

If everything goes smoothly this will build dinit, dinitctl, and optionally the shutdown
utility. Use "make install" to install; you can specify an alternate installation by
setting the "DESTDIR" variable, eg "make DESTDIR=/tmp/temporary-install-path install".


Special note for GCC/Libstdc++
=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

GCC 5.x onwards includes a "dual ABI" in its standard library implementation, aka Libstdc++.
Compiling against the newer (C++11 and later) ABI can be achieved by adding
-D_GLIBCXX_USE_CXX11_ABI=1 to the compiler command line; this uses a non-standard language
extension to differently mangle symbol names in order to link against the new ABI versions.

(Some systems may be configured to build with the new ABI by default, and in that case you
build against the old ABI using D_GLIBCXX_USE_CXX11_ABI=1).

This is problematic for several reasons. First, it prevents linking against the new ABI with
other compilers that do not understand the language extension (LLVM i.e. clang++ does so
in recent versions, so this is perhaps no longer much of a problem in practice). Secondly,
some aspects of library behavior are ABI-dependent but cannot be changed using the ABI
macro; in particular, exceptions thrown as a result of failed I/O operations are, in GCC
versions 5.x and 6.x, always "old ABI" exceptions which cannot be caught by code compiled
against the new ABI, and in GCC version 7.x they are always "new ABI" exceptions which cannot
be caught by code compiled against the old ABI. Since the one library object now supposedly
houses both ABIs, this means that at least one of the two ABIs is always broken.

A blog post describing the dual ABI mechanism can be found here:

    https://developers.redhat.com/blog/2015/02/05/gcc5-and-the-c11-abi/

The bug regarding the issue with catching other-ABI exceptions is here:

    https://gcc.gnu.org/bugzilla/show_bug.cgi?id=66145

Since Dinit is affected by this bug, the unfortunate possibility exists to break Dinit by
upgrading GCC. If you have libstdc++ corresponding to GCC 5.x or 6.x, you *must* build with
the old ABI, but Dinit will be broken if you upgrade to GCC 7. If you have libstdc++ from
GCC 7, you *must* build with the new ABI. If the wrong ABI is used, Dinit may still run
successfully but any attempt to load a non-existing service, for example, will cause Dinit
to crash.
