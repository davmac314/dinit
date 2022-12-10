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

## src/'s defines for igr-tests/
if igr_tests
    igr_tests_env = [
        'DINIT_EXEC=' + meson.current_build_dir() + '/dinit',
        'DINITCTL_EXEC=' + meson.current_build_dir() + '/dinitctl',
        'DINITCHECK_EXEC=' + meson.current_build_dir() + '/dinitcheck',
        'DINITMONITOR_EXEC=' + meson.current_build_dir() + '/dinit-monitor'
    ]
endif

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
    foreach exec: ['shutdown', 'reboot', 'halt']
        executable(
            shutdown_prefix + exec,
            'shutdown.cc',
            include_directories : default_incdir,
            install : true,
            install_dir : sbindir
        )
    endforeach
endif
