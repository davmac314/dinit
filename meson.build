## Experimetal meson build system for dinit
project(
    'dinit', 
    'cpp', 
    version : '0.16.0pre',
    license : 'Apache',
    default_options : ['cpp_std = c++11',
                       'b_lto = true',
                       'prefix=/',
                       'sbindir=/sbin'])

## Includes
# We include `src/includes/` & `dasynq/include/` as defualt_incdir 
default_incdir = include_directories('src/includes/', 'dasynq/include/')
dinit_needed_files = ['src/dinit-main.cc',
                      'src/dinit.cc',
                      'src/load-service.cc',
                      'src/service.cc',
                      'src/proc-service.cc',
                      'src/baseproc-service.cc',
                      'src/control.cc',
                      'src/dinit-log.cc',
                      'src/run-child-proc.cc',
                      'src/options-processing.cc',
                      'src/dinit-env.cc']

## Defines
compiler = meson.get_compiler('cpp')
shutdown_prefix = get_option('shutdown_prefix')
dinit_control_socket_path = get_option('dinit_control_socket_path')
use_new_abi = get_option('use_new_abi')
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
## Use -flto -fno-rtti -fno-plt
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
## Write mconfig.h
configure_file(output : 'mconfig.h',
               configuration : conf_data)

## Outputs
# Standard apps: dinit, dinitctl, dinitcheck, dinit-monitor
executable('dinit', dinit_needed_files , include_directories : default_incdir, install : true, install_dir : sbindir)
executable('dinitctl', 'src/dinitctl.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
executable('dinitcheck', 'src/dinitcheck.cc', 'src/options-processing.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
executable('dinit-monitor', 'src/dinit-monitor.cc', include_directories : default_incdir, install : true, install_dir : sbindir)

# Shutdown/reboot/halt
if get_option('build_shutdown').auto()
  if host_machine.system() == 'linux'
    message('System uses "linux"; Build shutdown = YES')
    if shutdown_prefix != ''
      message('shutdown_prefix is configured: ' + shutdown_prefix)
    endif
    executable(shutdown_prefix + 'shutdown', 'src/shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
    executable(shutdown_prefix + 'reboot', 'src/shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
    executable(shutdown_prefix + 'halt', 'src/shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
  else
    message('System not uses "linux"; Build shutdown = NO')
  endif
elif get_option('build_shutdown').enabled()
  if shutdown_prefix != ''
      message('shutdown_prefix is configured: ' + shutdown_prefix)
  endif
    executable(shutdown_prefix + 'shutdown', 'src/shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
    executable(shutdown_prefix + 'reboot', 'src/shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
    executable(shutdown_prefix + 'halt', 'src/shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
endif
