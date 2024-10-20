#!/bin/sh
# Generate build configuration for Linux.

rm -f ../mconfig

INST_PATH_OPTS=$(
  echo "# Installation path options.";
  echo "";
  echo "SBINDIR=/sbin";
  echo "MANDIR=/usr/share/man";
  echo "SYSCONTROLSOCKET=/run/dinitctl"
)

test_compiler_arg() {
  "$1" -c "$2" testfile.cc -o testfile.o > /dev/null 2>&1
  if test $? = 0; then
    rm testfile.o
    supported_opts="$supported_opts $2"
    supported_opts=${supported_opts# }
    return 0
  else
    return 1
  fi
}

# test argument is supported by compiler at both compile and link
test_compile_link_arg() {
  "$1" "$2" testfile.cc -o testfile > /dev/null 2>&1
  if test $? = 0; then
    rm testfile
    supported_opts="$supported_opts $2"
    supported_opts=${supported_opts# }
    return 0
  else
    return 1
  fi
}

for compiler in g++ clang++ c++ ""; do
  if test -z "$compiler"; then
    break # none found
  fi
  type $compiler > /dev/null
  if test $? = 0; then
    break # found
  fi
done

if test -z "$compiler"; then
  echo "*** No compiler found ***"
  exit 1
fi

echo "Compiler found          : $compiler"

echo "int main(int argc, char **argv) { return 0; }" > testfile.cc
supported_opts=""
test_compiler_arg "$compiler" -flto
NOT_HAS_LTO=$?
test_compiler_arg "$compiler" -fno-rtti
test_compiler_arg "$compiler" -fno-plt
BUILD_OPTS="-std=c++11 -Os -Wall $supported_opts"

echo "Using build options     : $supported_opts"

supported_opts=""
test_compile_link_arg "$compiler" -fsanitize=address,undefined
SANITIZE_OPTS="$supported_opts"

echo "Sanitize options        : $SANITIZE_OPTS"

rm testfile.cc

GENERAL_BUILD_SETTINGS=$(
  echo ""
  echo ""
  echo "# General build options."
  echo ""
  echo "# Linux (GCC). Note with GCC 5.x/6.x you must use the old ABI, with GCC 7.x you must use"
  echo "# the new ABI. See BUILD file for more information."
  echo "CXX=$compiler"
  echo "CXXFLAGS=$BUILD_OPTS"
  echo "CPPFLAGS=-D_GLIBCXX_USE_CXX11_ABI=1"
  echo "LDFLAGS_BASE="
  if [ "$NOT_HAS_LTO" = 0 ]; then
      echo "LDFLAGS=\$(LDFLAGS_BASE) \$(CXXFLAGS)"
  else
      echo "LDFLAGS=\$(LDFLAGS_BASE)"  
  fi
  echo "TEST_CXXFLAGS=\$(CXXFLAGS) $SANITIZE_OPTS"
  echo "TEST_LDFLAGS_BASE=\$(LDFLAGS_BASE)"
  if [ "$NOT_HAS_LTO" = 0 ]; then
      echo "TEST_LDFLAGS=\$(TEST_LDFLAGS_BASE) \$(TEST_CXXFLAGS)"
  else
      echo "TEST_LDFLAGS=\$(TEST_LDFLAGS_BASE)"  
  fi  
  echo "BUILD_SHUTDOWN=yes"
  echo ""
  echo "# Notes:"
  echo "#   -D_GLIBCXX_USE_CXX11_ABI=1 : force use of new ABI, see above / BUILD"
  echo "#   -fno-rtti (optional) : Dinit does not require C++ Run-time Type Information"
  echo "#   -fno-plt  (optional) : Recommended optimisation"
  echo "#   -flto     (optional) : Perform link-time optimisation"
  echo "#   -fsanitize=address,undefined :  Apply sanitizers (during unit tests)"
  echo "# LDFLAGS should also contain C++ optimisation flags for LTO (-flto)."
)

FEATURE_SETTINGS=$(
  echo ""
  echo ""
  echo "# Feature settings"
  echo ""
  echo "SUPPORT_CGROUPS=1"
  echo "SUPPORT_CAPABILITIES=1"
)

SERVICE_DEFAULTS=$(
  echo ""
  echo ""
  echo "# Service defaults"
  echo ""
  echo "DEFAULT_AUTO_RESTART=ALWAYS"
  echo "DEFAULT_START_TIMEOUT=60"
  echo "DEFAULT_STOP_TIMEOUT=10"
)

(
  echo "$INST_PATH_OPTS"
  echo "$GENERAL_BUILD_SETTINGS" 
  echo "$FEATURE_SETTINGS"
  echo "$SERVICE_DEFAULTS"
) >> ../mconfig
