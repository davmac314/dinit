# Included from top-level meson.build

## Includes
# We include `../`, `./includes/` & `../dasynq/include/` as defualt_incdir
default_incdir = include_directories(
    '../',
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
    'dinit-env.cc',
    'settings.cc'
]

## src/'s Defines
shutdown_built = false
misc_args = {
    'include_directories': default_incdir,
    'install': true,
    'install_dir': sbindir,
    'dependencies': [libcap_dep]
}

## src/'s defines for igr-tests/
if igr_tests
    igr_tests_env = [ 'DINIT_BINDIR=@0@'.format(meson.current_build_dir()) ]
endif

## Outputs
# Standard apps: dinit, dinitctl, dinitcheck, dinit-monitor
executable(
    'dinit',
    dinit_source_files,
    kwargs: misc_args
)
executable(
    'dinitctl',
    'dinitctl.cc',
    'options-processing.cc',
    'settings.cc',
    kwargs: misc_args
)
executable(
    'dinitcheck',
    'dinitcheck.cc',
    'options-processing.cc',
    'settings.cc',
    kwargs: misc_args
)
executable(
    'dinit-monitor',
    'dinit-monitor.cc',
    kwargs: misc_args
)
# Shutdown/reboot/halt
if build_shutdown.auto() and platform == 'linux' or build_shutdown.enabled()
    shutdown_built = true
    executable(
        shutdown_prefix + 'shutdown',
        'shutdown.cc',
        kwargs: misc_args
    )
    foreach exec: ['reboot', 'soft-reboot', 'halt', 'poweroff']
        install_symlink(
            shutdown_prefix + exec,
            pointing_to: shutdown_prefix + 'shutdown',
            install_dir: sbindir
        )
    endforeach
endif
