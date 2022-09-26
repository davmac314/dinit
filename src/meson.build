# Included from top-level meson.build

## Includes
# We include `./includes/` & `../dasynq/include/` as defualt_incdir
default_incdir = include_directories('includes/', '../dasynq/include/')
dinit_source_files = ['dinit-main.cc',
                      'dinit.cc',
                      'load-service.cc',
                      'service.cc',
                      'proc-service.cc',
                      'baseproc-service.cc',
                      'control.cc',
                      'dinit-log.cc',
                      'run-child-proc.cc',
                      'options-processing.cc',
                      'dinit-env.cc']

## Write mconfig.h
configure_file(output : 'mconfig.h',
               configuration : conf_data)

## Outputs
# Standard apps: dinit, dinitctl, dinitcheck, dinit-monitor
dinit = executable('dinit', dinit_source_files , include_directories : default_incdir, install : true, install_dir : sbindir)
dinitctl = executable('dinitctl', 'dinitctl.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
executable('dinitcheck', 'dinitcheck.cc', 'options-processing.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
executable('dinit-monitor', 'dinit-monitor.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
# Shutdown/reboot/halt
if get_option('build_shutdown').auto()
  if host_machine.system() == 'linux'
    message('System uses "linux"; Build shutdown = YES')
    if shutdown_prefix != ''
      message('shutdown_prefix is configured: ' + shutdown_prefix)
    endif
    executable(shutdown_prefix + 'shutdown', 'shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
    executable(shutdown_prefix + 'reboot', 'shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
    executable(shutdown_prefix + 'halt', 'shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
  else
    message('System not uses "linux"; Build shutdown = NO')
  endif
elif get_option('build_shutdown').enabled()
  if shutdown_prefix != ''
      message('shutdown_prefix is configured: ' + shutdown_prefix)
  endif
    executable(shutdown_prefix + 'shutdown', 'shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
    executable(shutdown_prefix + 'reboot', 'shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
    executable(shutdown_prefix + 'halt', 'shutdown.cc', include_directories : default_incdir, install : true, install_dir : sbindir)
endif
