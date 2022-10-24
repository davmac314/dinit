## Experimetal meson build system for dinit
project(
    'dinit', 
    'cpp', 
    version : '0.16.0',
    license : 'Apache-2.0',
    meson_version : '>= 0.47.0',
    default_options : 'cpp_std = c++11'
)

## General Defines
compiler = meson.get_compiler('cpp')
mconfig_data = configuration_data()
version = meson.project_version()
month = run_command('build/general_variables_for_meson.sh', 'month', check : true).stdout()
year = run_command('build/general_variables_for_meson.sh', 'year', check : true).stdout()
build_shutdown = get_option('build-shutdown')
shutdown_prefix = get_option('shutdown-prefix')
dinit_control_socket_path = get_option('dinit-control-socket-path')
unit_tests = get_option('unit-tests')
igr_tests = get_option('igr-tests')
man_pages = get_option('man-pages')
support_cgroups = get_option('support-cgroups')
use_utmpx = get_option('use-utmpx')
# We have custom sbindir for install programs (/sbin instead of /usr/sbin/ by default)
# By default: Prefix = /usr
# By default: Sbin = /sbin
# By default: you find dinit on /sbin/dinit
# Note: Dinit dont follow Meson's default sbindir; Use dinit-sbindir option instead!
prefix = get_option('prefix')
sbindir = get_option('dinit-sbindir')
mandir = get_option('mandir')

## If Meson's default sbindir is modifed; Warn users about it:
if get_option('sbindir') != 'sbin'
    warning('Seems like Default "sbindir" is modified. Dinit dont follow Default "sbindir" option. Please use "dinit-sbindir" option instead!')
endif

## Use -lrt?
# We need to pass -lrt to c++ linker on FreeBSD. see BUILD_MESON
if host_machine.system() == 'freebsd'
    add_project_link_arguments('-lrt', language : 'cpp')
endif

## Prepare mconfig.h
mconfig_data.set('DINIT_VERSION', '"' + meson.project_version() + '"')
mconfig_data.set('SYSCONTROLSOCKET', '"' + dinit_control_socket_path + '"')
mconfig_data.set('SBINDIR', '"' + sbindir + '"')
mconfig_data.set('SHUTDOWN_PREFIX', '"' + shutdown_prefix + '"')
if support_cgroups.auto()
    if host_machine.system() == 'linux'
        mconfig_data.set('SUPPORT_CGROUPS', '1') 
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
