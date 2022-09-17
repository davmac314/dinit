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
## We include `src/includes/` & `dasynq/include/` as defualt_incdir 
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

compiler = meson.get_compiler('cpp')

## Use -D_GLIBCXX_USE_CXX11_ABI?
if get_option('use_new_abi').auto()
  if compiler.get_id() == 'gcc'
    if compiler.version().version_compare('>=7')
      message('System uses g++ 7 or later; Use new ABI = YES')
      add_global_arguments('-D_GLIBCXX_USE_CXX11_ABI=1', language : 'cpp')
    else
      message('System uses g++ 6 or easiler; Use new ABI = NO')
      add_global_arguments('-D_GLIBCXX_USE_CXX11_ABI=0', language : 'cpp')
    endif
  endif
elif get_option('use_new_abi').enabled()
  add_global_arguments('-D_GLIBCXX_USE_CXX11_ABI=1', language : 'cpp')
elif get_option('use_new_abi').disabled()
  add_global_arguments('-D_GLIBCXX_USE_CXX11_ABI=0', language : 'cpp')
endif

## ToDo; its must handeled with `meson_options.txt` instead of force them
## Use -flto -fno-rtti -fno-plt
add_global_arguments('-fno-rtti', language : 'cpp')
add_global_arguments('-fno-plt', language : 'cpp')

## We have custom prefix for install files
# Prefix = /usr
# Sbin = /sbin
# By default; you find dinit on /sbin
prefix = get_option('prefix')
sbindir = get_option('sbindir')

## Prepare mconfig.h
conf_data = configuration_data()
conf_data.set('DINIT_VERSION', '"' + '@0@'.format(meson.project_version()) + '"')
conf_data.set('SYSCONTROLSOCKET', '"' + get_option('dinit_control_socket_path') + '"')
conf_data.set('SBINDIR', '"' + prefix + sbindir + '"')
conf_data.set('SHUTDOWN_PREFIX', '"' + get_option('shutdown_prefix') + '"')
if get_option('support_cgroups').auto()
  if host_machine.system() == 'linux'
    message('System uses "linux"; Support Cgroups = YES')
    conf_data.set('SUPPORT_CGROUPS', '1') 
  else
    message('System not uses "linux"; Support Cgroups = NO')
  endif
elif get_option('support_cgroups').enabled()
  conf_data.set('SUPPORT_CGROUPS', '1')
endif
## Write mconfig.h
configure_file(output : 'mconfig.h',
               configuration : conf_data)

## Outputs
executable('dinit', dinit_needed_files , include_directories : default_incdir, install : true, install_dir : sbindir)
executable('dinitctl', 'src/dinitctl.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
executable('dinitcheck', 'src/dinitcheck.cc', 'src/options-processing.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
executable('dinit-monitor', 'src/dinit-monitor.cc', include_directories : default_incdir, install : true, install_dir : sbindir)

if get_option('build_shutdown').auto()
  if host_machine.system() == 'linux'
    if get_option('shutdown_prefix') != ''
      shutdown_prefix = get_option('shutdown_prefix')
      message('System uses "linux"; Build shutdown = YES')
      message('shutdown_prefix is configured: ' + shutdown_prefix)
      executable(shutdown_prefix + 'shutdown', 'src/shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
      executable(shutdown_prefix + 'reboot', 'src/shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
      executable(shutdown_prefix + 'halt', 'src/shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
    else
      message('System uses "linux"; Build shutdown = YES')
      executable('shutdown', 'src/shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
      executable('reboot', 'src/shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
      executable('halt', 'src/shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
      endif
  else
    message('System not uses "linux"; Build shutdown = NO')
  endif
elif get_option('build_shutdown').enabled()
  if get_option('shutdown_prefix') != ''
    shutdown_prefix = get_option('shutdown_prefix')
    message('shutdown_prefix is configured: ' + shutdown_prefix)
    executable(shutdown_prefix + 'shutdown', 'src/shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
    executable(shutdown_prefix + 'reboot', 'src/shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
    executable(shutdown_prefix + 'halt', 'src/shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
  else
    executable('shutdown', 'src/shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
    executable('reboot', 'src/shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
    executable('halt', 'src/shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
  endif
endif
