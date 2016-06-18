Building Dinit
=-=-=-=-=-=-=-

Buildingn Dinit should be a straight-forward process. It requires GNU make.

Edit the "mconfig" file to choose appropriate values for the configuration variables defined
within. In particular:

  CXX : should be set to the name of the C++ compiler (and linker)
  CXXOPTS :  are options passed to the compiler during compilation
  EXTRA_LIBS : are any extra libraries required for linking; should not normally be needed.

Defaults for Linux and OpenBSD are provided. Note that the "eg++" package must be installed
on OpenBSD as the default "g++" compiler is too old.

Then, change into the "src" directory, and run "make" (or "gmake" if the system make is not
GNU make):

    cd src
    make
