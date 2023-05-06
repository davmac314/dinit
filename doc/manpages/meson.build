# Included from top-level meson.build

## Generate man-pages
# Dinit need 'm4' to gen man-pages
foreach man: ['dinit-service.5', 'dinit.8', 'dinitcheck.8', 'dinitctl.8', 'dinit-monitor.8']
    section = 'man' + man.split('.')[1]
    custom_target(
        man,
        input: man + '.m4',
        output: man,
        command: [
            'm4',
            '-DVERSION='+version,
            '-DMONTH='+month,
            '-DYEAR='+year,
            '-DSHUTDOWN_PREFIX='+shutdown_prefix,
            '-DDEFAULT_AUTO_RESTART='+default_auto_restart,
            '-DDEFAULT_START_TIMEOUT='+default_start_timeout,
            '-DDEFAULT_STOP_TIMEOUT='+default_stop_timeout,
            '@INPUT@'
        ],
        capture: true,
        install: true,
        install_dir: join_paths(get_option('mandir'), section),
    )
endforeach
if shutdown_build
    foreach shutdown_man: ['shutdown.8', 'reboot.8', 'halt.8']
        custom_target(
            shutdown_man,
            input: 'shutdown.8.m4',
            output: shutdown_prefix + shutdown_man,
            command: [
                'm4',
                '-DVERSION='+version,
                '-DMONTH='+month,
                '-DYEAR='+year,
                '-DSHUTDOWN_PREFIX='+shutdown_prefix,
                '@INPUT@'
            ],
            capture: true,
            install: true,
            install_dir: join_paths(get_option('mandir'), section),
        )
    endforeach
endif
