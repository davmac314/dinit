## Experimetal meson build system for dinit
project(
    'dinit', 
    'cpp', 
    version : run_command(
    [find_program('sh'), 'build/general_variables_for_meson.sh', 'version'],
    check : true
    ).stdout(),
    license : 'Apache',
    meson_version : '>= 0.47.0',
    default_options : ['cpp_std = c++11',
                       'b_lto = true',
                       'prefix=/usr'])

## General Defines
compiler = meson.get_compiler('cpp')
shutdown_prefix = get_option('shutdown_prefix')
dinit_control_socket_path = get_option('dinit_control_socket_path')
use_new_abi = get_option('use_new_abi')
unit_tests = get_option('unit_tests')
igr_tests = get_option('igr_tests')
support_cgroups = get_option('support_cgroups')
use_utmpx = get_option('use_utmpx')
# We have custom sbindir for install programs (/sbin instead of /usr/sbin/)
# By default: Prefix = /usr
# By default: Sbin = /sbin
# By default: you find dinit on /sbin/dinit
prefix = get_option('prefix')
sbindir = get_option('dinit_sbindir')
mandir = get_option('mandir')
conf_data = configuration_data()
version = '@0@'.format(meson.project_version())
month = run_command('build/general_variables_for_meson.sh', 'month', check : true).stdout()
year = run_command('build/general_variables_for_meson.sh', 'year', check : true).stdout()

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
else
  message('System not uses FreeBSD; Pass "-lrt" to linker = NO')
endif

## ToDo; its must handeled with `meson_options.txt` instead of force them
# Use -fno-rtti -fno-plt
#add_roject_arguments('-fno-rtti', language : 'cpp')
#add_project_arguments('-fno-plt', language : 'cpp')

## Prepare mconfig.h
conf_data.set('DINIT_VERSION', '"' + '@0@'.format(meson.project_version()) + '"')
conf_data.set('SYSCONTROLSOCKET', '"' + dinit_control_socket_path + '"')
conf_data.set('SBINDIR', '"' + sbindir + '"')
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
if use_utmpx
  conf_data.set('USE_UTMPX', '1')
else
  conf_data.set('USE_UTMPX', '0')
endif

## Outputs
# Standard apps: dinit, dinitctl, dinitcheck, dinit-monitor + shutdown/reboot/halt
subdir('src')
# Temp fix for FreeBSD CIs cant find `install_shutdown_man` variable
if get_option('build_shutdown').auto()
  if host_machine.system() == 'linux'
    install_shutdown_man = true
  endif
elif get_option('build_shutdown').enabled()
  install_shutdown_man = true
endif
# Unit tests
if unit_tests
  subdir('src/tests')
endif
# Integration tests
if igr_tests
  subdir('src/igr-tests')
endif
# Man pages
subdir('doc/manpages/')
