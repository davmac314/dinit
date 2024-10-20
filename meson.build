## Experimetal meson build system for dinit
project(
    'dinit', 
    'cpp', 
    version : run_command('grep', '^VERSION=', 'build/version.conf',
        check: true).stdout().strip().split('=')[1],
    license : 'Apache-2.0',
    meson_version : '>= 0.61.0',
    default_options : [
        'cpp_std=c++11',
        'optimization=s',
        'debug=false'
    ]
)

## Import variables from build/version.conf
version_conf = import('keyval').load('build/version.conf')

## General Defines
compiler = meson.get_compiler('cpp')
mconfig_data = configuration_data()
version = meson.project_version()
platform = host_machine.system()
month = version_conf.get('MONTH')
year = version_conf.get('YEAR')
build_shutdown = get_option('build-shutdown')
shutdown_prefix = get_option('shutdown-prefix')
dinit_control_socket_path = get_option('dinit-control-socket-path')
unit_tests = get_option('unit-tests')
igr_tests = get_option('igr-tests')
fuzzer = get_option('fuzzer')
man_pages = get_option('man-pages')
support_cgroups = get_option('support-cgroups')
support_capabilities = get_option('support-capabilities')
use_utmpx = get_option('use-utmpx')
use_initgroups = get_option('use-initgroups')
default_auto_restart = get_option('default-auto-restart')
default_start_timeout = get_option('default-start-timeout').to_string()
default_stop_timeout = get_option('default-stop-timeout').to_string()
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
    warning('It appears that "sbindir" has been set/modified. Dinit\'s build doesn\'t use the "sbindir" option. Please use "dinit-sbindir" option instead!')
endif

## Use -lrt?
# We need to pass -lrt to c++ linker on FreeBSD. see BUILD_MESON
if platform == 'freebsd' and compiler.has_link_argument('-lrt')
    add_project_link_arguments('-lrt', language : 'cpp')
endif

libcap_dep = dependency('libcap', required: support_capabilities)

## Prepare mconfig.h
mconfig_data.set_quoted('DINIT_VERSION', version)
mconfig_data.set_quoted('SYSCONTROLSOCKET', dinit_control_socket_path)
mconfig_data.set_quoted('SBINDIR', sbindir)
mconfig_data.set_quoted('SHUTDOWN_PREFIX', shutdown_prefix)
mconfig_data.set('DEFAULT_AUTO_RESTART', default_auto_restart)
mconfig_data.set('DEFAULT_START_TIMEOUT', default_start_timeout)
mconfig_data.set('DEFAULT_STOP_TIMEOUT', default_stop_timeout)
mconfig_data.set10('USE_INITGROUPS', use_initgroups)
if support_cgroups.auto() and platform == 'linux' or support_cgroups.enabled()
    mconfig_data.set('SUPPORT_CGROUPS', '1') 
endif
if libcap_dep.found()
    mconfig_data.set('SUPPORT_CAPABILITIES', '1')
endif
if use_utmpx.enabled() or (use_utmpx.auto() and compiler.has_header_symbol('utmpx.h', '_PATH_UTMPX') and
        compiler.has_header_symbol('utmpx.h', '_PATH_WTMPX'))
    mconfig_data.set('USE_UTMPX', '1')
else
    mconfig_data.set('USE_UTMPX', '0')
endif
configure_file(
    input : 'build/mconfig.mesontemplate',
    output : 'mconfig.h',
    configuration : mconfig_data
)

## Outputs
subdir('src')
if unit_tests
    subdir('src/tests/')
endif
if unit_tests or fuzzer
    subdir('src/tests/cptests/')
endif
if igr_tests
    subdir('src/igr-tests/')
endif
if man_pages
    subdir('doc/manpages/')
endif
