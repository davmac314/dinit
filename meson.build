## Experimetal meson build system for dinit
project(
    'dinit', 
    'cpp', 
    version : run_command(
    [find_program('sh'), 'build/general_variables_for_meson.sh', 'version'],
    check : true
    ).stdout(),
    license : 'Apache',
    default_options : ['cpp_std = c++11',
                       'b_lto = true',
                       'prefix=/',
                       'sbindir=/sbin'])

## General Defines
compiler = meson.get_compiler('cpp')
shutdown_prefix = get_option('shutdown_prefix')
dinit_control_socket_path = get_option('dinit_control_socket_path')
use_new_abi = get_option('use_new_abi')
unit_tests = get_option('unit_tests')
igr_tests = get_option('igr_tests')
conf_data = configuration_data()
support_cgroups = get_option('support_cgroups')
# We have custom prefix for install files
# By default: Prefix = /
# By default: Sbin = /sbin
# By default: you find dinit on /sbin
prefix = get_option('prefix')
sbindir = get_option('sbindir')

## Use -D_GLIBCXX_USE_CXX11_ABI?
if use_new_abi.auto()
  if compiler.get_id() == 'gcc'
    if compiler.version().version_compare('>=7')
      message('System uses g++ 7 or later; Use new ABI = YES')
      add_project_arguments('-D_GLIBCXX_USE_CXX11_ABI=1', language : 'cpp')
    else
      message('System uses g++ 6 or easiler; Use new ABI = NO')
      add_project_arguments('-D_GLIBCXX_USE_CXX11_ABI=0', language : 'cpp')
    endif
  endif
elif use_new_abi.enabled()
  add_project_arguments('-D_GLIBCXX_USE_CXX11_ABI=1', language : 'cpp')
elif use_new_abi.disabled()
  add_project_arguments('-D_GLIBCXX_USE_CXX11_ABI=0', language : 'cpp')
endif

## Use -lrt?
# We need to pass -lrt to c++ linker on FreeBSD. see BUILD_MESON
if host_machine.system() == 'freebsd'
  message('System uses FreeBSD; Pass "-lrt" to linker = YES')
  add_project_link_arguments('-lrt', language : 'cpp')
endif

## ToDo; its must handeled with `meson_options.txt` instead of force them
# Use -fno-rtti -fno-plt
add_project_arguments('-fno-rtti', language : 'cpp')
add_project_arguments('-fno-plt', language : 'cpp')

## Prepare mconfig.h
conf_data.set('DINIT_VERSION', '"' + '@0@'.format(meson.project_version()) + '"')
conf_data.set('SYSCONTROLSOCKET', '"' + dinit_control_socket_path + '"')
conf_data.set('SBINDIR', '"' + prefix + sbindir + '"')
conf_data.set('SHUTDOWN_PREFIX', '"' + shutdown_prefix + '"')
if support_cgroups.auto()
  if host_machine.system() == 'linux'
    message('System uses "linux"; Support Cgroups = YES')
    conf_data.set('SUPPORT_CGROUPS', '1') 
  else
    message('System not uses "linux"; Support Cgroups = NO')
  endif
elif support_cgroups.enabled()
  conf_data.set('SUPPORT_CGROUPS', '1')
endif

## Outputs
# Standard apps: dinit, dinitctl, dinitcheck, dinit-monitor + shutdown/reboot/halt
subdir('src')
# Unit tests
if unit_tests.enabled()
  subdir('src/tests')
endif
if igr_tests.enabled()
  subdir('src/igr-tests')
endif
