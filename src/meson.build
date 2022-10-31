# Included from top-level meson.build

## Includes
# We include `./includes/` & `../dasynq/include/` as defualt_incdir
default_incdir = include_directories(
    'includes/',
    '../dasynq/include/'
)
dinit_source_files = [
    'dinit-main.cc',
    'dinit.cc',
    'load-service.cc',
    'service.cc',
    'proc-service.cc',
    'baseproc-service.cc',
    'control.cc',
    'dinit-log.cc',
    'run-child-proc.cc',
    'options-processing.cc',
    'dinit-env.cc'
]

## src/'s defines
APPS_PATH = meson.current_build_dir()

## Write mconfig.h
configure_file(
    output : 'mconfig.h',
    configuration : mconfig_data
)

## Outputs
# Standard apps: dinit, dinitctl, dinitcheck, dinit-monitor
executable(
    'dinit',
    dinit_source_files,
    include_directories : default_incdir,
    install : true,
    install_dir : sbindir
)
executable(
    'dinitctl',
    'dinitctl.cc',
    include_directories : default_incdir,
    install : true,
    install_dir : sbindir
)
executable(
    'dinitcheck',
    'dinitcheck.cc',
    'options-processing.cc',
    include_directories : default_incdir,
    install : true,
    install_dir : sbindir
)
executable(
    'dinit-monitor',
    'dinit-monitor.cc',
    include_directories : default_incdir,
    install : true,
    install_dir : sbindir
)
# Shutdown/reboot/halt
if build_shutdown.auto() and platform == 'linux' or build_shutdown.enabled()
    shutdown_build = true
    install_shutdown_man = true
elif build_shutdown.auto() and platform != 'linux'
    shutdown_build = false
    install_shutdown_man = false
else
    shutdown_build = false
    install_shutdown_man = false
endif
if shutdown_build
    executable(
          shutdown_prefix + 'shutdown',
          'shutdown.cc',
          include_directories : default_incdir,
          install : true,
          install_dir : sbindir
    )
    executable(
          shutdown_prefix + 'reboot',
          'shutdown.cc',
          include_directories : default_incdir,
          install : true,
          install_dir : sbindir
    )
    executable(
          shutdown_prefix + 'halt',
          'shutdown.cc',
          include_directories : default_incdir,
          install : true,
          install_dir : sbindir
    )
endif
