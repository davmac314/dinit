## Experimetal meson build system for dinit
project(
    'dinit', 
    'cpp', 
    version : run_command(
    [find_program('sh'), 'build/general_variables_for_meson.sh', 'version'],
    check : true
    ).stdout(),
    license : 'Apache-2.0',
    meson_version : '>= 0.47.0',
    default_options : 'cpp_std = c++11')

## General Defines
compiler = meson.get_compiler('cpp')
mconfig_data = configuration_data()
version = meson.project_version()
month = run_command('build/general_variables_for_meson.sh', 'month', check : true).stdout()
year = run_command('build/general_variables_for_meson.sh', 'year', check : true).stdout()
build_shutdown = get_option('build_shutdown')
shutdown_prefix = get_option('shutdown_prefix')
dinit_control_socket_path = get_option('dinit_control_socket_path')
unit_tests = get_option('unit_tests')
igr_tests = get_option('igr_tests')
man_pages = get_option('man_pages')
support_cgroups = get_option('support_cgroups')
use_utmpx = get_option('use_utmpx')
# We have custom sbindir for install programs (/sbin instead of /usr/sbin/ by default)
# By default: Prefix = /usr
# By default: Sbin = /sbin
# By default: you find dinit on /sbin/dinit
# Note: Dinit dont follow Meson's default sbindir; Use dinit_sbindir option instead!
prefix = get_option('prefix')
sbindir = get_option('dinit_sbindir')
mandir = get_option('mandir')

## If Meson's default sbindir is modifed; Warn users about it:
if get_option('sbindir') != 'sbin'
  warning('Seems like Default "sbindir" is modified. Dinit dont follow Default "sbindir" option. Please use "dinit_sbindir" option instead!')
endif

## Use -lrt?
# We need to pass -lrt to c++ linker on FreeBSD. see BUILD_MESON
if host_machine.system() == 'freebsd'
  message('System uses FreeBSD; Pass "-lrt" to linker = YES')
  add_project_link_arguments('-lrt', language : 'cpp')
else
  message('System not uses FreeBSD; Pass "-lrt" to linker = NO')
endif

## Prepare mconfig.h
mconfig_data.set('DINIT_VERSION', '"' + meson.project_version() + '"')
mconfig_data.set('SYSCONTROLSOCKET', '"' + dinit_control_socket_path + '"')
mconfig_data.set('SBINDIR', '"' + sbindir + '"')
mconfig_data.set('SHUTDOWN_PREFIX', '"' + shutdown_prefix + '"')
if support_cgroups.auto()
  if host_machine.system() == 'linux'
    message('System uses "linux"; Support Cgroups = YES')
    mconfig_data.set('SUPPORT_CGROUPS', '1') 
  else
    message('System not uses "linux"; Support Cgroups = NO')
  endif
elif support_cgroups.enabled()
  mconfig_data.set('SUPPORT_CGROUPS', '1')
endif
if use_utmpx.enabled()
  mconfig_data.set('USE_UTMPX', '1')
elif use_utmpx.disabled()
  mconfig_data.set('USE_UTMPX', '0')
endif

## Outputs
subdir('src')
if unit_tests
  subdir('src/tests')
endif
if igr_tests
  subdir('src/igr-tests')
endif
if man_pages
  subdir('doc/manpages/')
endif
