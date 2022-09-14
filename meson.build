## Experimetal meson build system for dinit
project(
    'dinit', 
    'cpp', 
    version : '0.15.1',
    license : 'Apache',
    default_options : ['cpp_std = c++11',
                       'b_lto = true'])

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

## Use -D_GLIBCXX_USE_CXX11_ABI?
if get_option('use_new_abi').auto()
# ToDo. We must verify libstdc++ version and set -D_GLIBCXX_USE_CXX11_ABI to 1 or 0
elif get_option('use_new_abi').enabled()
  add_global_arguments('-D_GLIBCXX_USE_CXX11_ABI=1', language : 'cpp')
elif get_option('use_new_abi').disabled()
  add_global_arguments('-D_GLIBCXX_USE_CXX11_ABI=0', language : 'cpp')
endif

## ToDo; its must handeled with `meson_options.txt` instead of force them
## Use -flto -fno-rtti -fno-plt
add_global_arguments('-fno-rtti', language : 'cpp')
add_global_arguments('-fno-plt', language : 'cpp')

## Prepare mconfig.h
conf_data = configuration_data()
conf_data.set('DINIT_VERSION', '""') # ToDo; Set dinit version
conf_data.set('SYSCONTROLSOCKET', '"' + get_option('dinit_control_socket_path') + '"')
conf_data.set('SBINDIR', '""')
conf_data.set('SHUTDOWN_PREFIX', '"' + get_option('shutdown_prefix') + '"')
if get_option('support_cgroups').auto()
  #ToDo. we must verify host system uses Linux & have cgroup file system type
elif get_option('support_cgroups').enabled()
  conf_data.set('SUPPORT_CGROUPS', true)
endif
## Write mconfig.h
configure_file(output : 'mconfig.h',
               configuration : conf_data)

## Outputs
executable('dinit', dinit_needed_files , include_directories : default_incdir)
executable('dinitctl', 'src/dinitctl.cc', include_directories : default_incdir)
executable('dinitcheck', 'src/dinitcheck.cc', 'src/options-processing.cc', include_directories : default_incdir)
executable('dinit-monitor', 'src/dinit-monitor.cc', include_directories : default_incdir)

if get_option('build_shutdown').auto()
  if host_machine.system() == 'linux'
    if get_option('shutdown_prefix') != ''
      shutdown_prefix = get_option('shutdown_prefix')
      message('System uses "linux"; Build shutdown = YES')
      message('shutdown_prefix is configured: ' + shutdown_prefix)
      executable(shutdown_prefix + 'shutdown', 'src/shutdown.cc', include_directories : default_incdir)
      executable(shutdown_prefix + 'reboot', 'src/shutdown.cc', include_directories : default_incdir)
      executable(shutdown_prefix + 'halt', 'src/shutdown.cc', include_directories : default_incdir)
    else
      message('System uses "linux"; Build shutdown = YES')
      executable('shutdown', 'src/shutdown.cc', include_directories : default_incdir)
      executable('reboot', 'src/shutdown.cc', include_directories : default_incdir)
      executable('halt', 'src/shutdown.cc', include_directories : default_incdir)
      endif
  else
    message('System not uses "linux"; Build shutdown = NO')
  endif
elif get_option('build_shutdown').enabled()
  if get_option('shutdown_prefix') != ''
    shutdown_prefix = get_option('shutdown_prefix')
    message('shutdown_prefix is configured: ' + shutdown_prefix)
    executable(shutdown_prefix + 'shutdown', 'src/shutdown.cc', include_directories : default_incdir)
    executable(shutdown_prefix + 'reboot', 'src/shutdown.cc', include_directories : default_incdir)
    executable(shutdown_prefix + 'halt', 'src/shutdown.cc', include_directories : default_incdir)
  else
    executable('shutdown', 'src/shutdown.cc', include_directories : default_incdir)
    executable('reboot', 'src/shutdown.cc', include_directories : default_incdir)
    executable('halt', 'src/shutdown.cc', include_directories : default_incdir)
  endif
endif