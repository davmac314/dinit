# Included from top-level meson.build

## Includes
default_incdir = include_directories(
    '../',
    'includes/',
    '../dasynq/include/'
)
libdinit = static_library(
    'shared_dinit_objects',
    'options-processing.cc',
    'dinit-env.cc',
    'settings.cc',
    include_directories: default_incdir,
    install: false
)

## src/'s defines for tests/ and fuzzer
if unit_tests or fuzzer
    for_tests_incdir = include_directories('tests/test-includes/')
    libtests = static_library(
        'for_tests_general_objects',
        'tests/test-dinit.cc',
        'tests/test-bpsys.cc',
        'tests/test-run-child-proc.cc',
        'service.cc',
        'proc-service.cc',
        'load-service.cc',
        'baseproc-service.cc',
        'dinit-log.cc',
        'control.cc',
        link_with: libdinit,
        include_directories: [ for_tests_incdir, default_incdir ],
        install: false
    )
endif

## src/'s Defines
shutdown_built = false
misc_args = {
    'include_directories': default_incdir,
    'install': true,
    'install_dir': sbindir,
    'link_with': libdinit,
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
    'dinit-main.cc',
    'dinit.cc',
    'run-child-proc.cc',
    'service.cc',
    'proc-service.cc',
    'load-service.cc',
    'baseproc-service.cc',
    'dinit-log.cc',
    'control.cc',
    kwargs: misc_args
)
executable(
    'dinitctl',
    'dinitctl.cc',
    kwargs: misc_args
)
executable(
    'dinitcheck',
    'dinitcheck.cc',
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
