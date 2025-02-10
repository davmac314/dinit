# Included from top-level meson.build

igr_tests_env += [
    'IGR_OUTPUT_BASE=@0@'.format(meson.current_build_dir()),
    'IGR_INPUT_BASE=@0@'.format(meson.current_source_dir())
]

runner = executable(
    'igr-runner',
    'igr-runner.cc',
    include_directories: include_directories('../../dasynq/include', '../..'),
    install: false
)

tests = [
    'basic', 'environ', 'environ2', 'ps-environ', 'chain-to', 'force-stop', 'restart',
    'check-basic', 'check-cycle', 'check-cycle2', 'check-lint', 'reload1', 'reload2',
    'no-command-error', 'add-rm-dep', 'var-subst', 'svc-start-fail', 'dep-not-found',
    'pseudo-cycle', 'before-after', 'before-after2', 'log-via-pipe', 'catlog',
    'offline-enable', 'xdg-config', 'cycles', 'svc-arg'
]

foreach test: tests
    test(
        test,
        runner,
        args: test,
        env: igr_tests_env,
        suite: 'igr_tests'
    )
endforeach
