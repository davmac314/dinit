# Included from top-level meson.build

igr_tests_env += [
     'IGR_OUTPUT_BASE=@0@'.format(meson.current_build_dir()),
     'DEBUG=yes'
]

# Integration tests
basic_test_script = files(meson.current_source_dir() + '/basic/run-test.sh')
environ_test_script = files(meson.current_source_dir() + '/environ/run-test.sh')
environ2_test_script = files(meson.current_source_dir() + '/environ2/run-test.sh')
psenviron_test_script = files(meson.current_source_dir() + '/ps-environ/run-test.sh')
chainto_test_script = files(meson.current_source_dir() + '/chain-to/run-test.sh')
forcestop_test_script = files(meson.current_source_dir() + '/force-stop/run-test.sh')
restart_test_script = files(meson.current_source_dir() + '/restart/run-test.sh')
checkbasic_test_script = files(meson.current_source_dir() + '/check-basic/run-test.sh')
checkcycle_test_script = files(meson.current_source_dir() + '/check-cycle/run-test.sh')
checkcycle2_test_script = files(meson.current_source_dir() + '/check-cycle2/run-test.sh')
checklint_test_script = files(meson.current_source_dir() + '/check-lint/run-test.sh')
reload1_test_script = files(meson.current_source_dir() + '/reload1/run-test.sh')
reload2_test_script = files(meson.current_source_dir() + '/reload2/run-test.sh')
nocommanderror_test_script = files(meson.current_source_dir() + '/no-command-error/run-test.sh')
addrmdep_test_script = files(meson.current_source_dir() + '/add-rm-dep/run-test.sh')
varsubst_test_script = files(meson.current_source_dir() + '/var-subst/run-test.sh')
svcstartfail_test_script = files(meson.current_source_dir() + '/svc-start-fail/run-test.sh')
depnotfound_test_script = files(meson.current_source_dir() + '/dep-not-found/run-test.sh')
pseudocycle_test_script = files(meson.current_source_dir() + '/pseudo-cycle/run-test.sh')
beforeafter_test_script = files(meson.current_source_dir() + '/before-after/run-test.sh')
beforeafter2_test_script = files(meson.current_source_dir() + '/before-after2/run-test.sh')
logviapipe_test_script = files(meson.current_source_dir() + '/log-via-pipe/run-test.sh')
catlog_test_script = files(meson.current_source_dir() + '/catlog/run-test.sh')
offlineenable_test_script = files(meson.current_source_dir() + '/offline-enable/run-test.sh')
test(
     'basic',
     basic_test_script,
     env : igr_tests_env,
     suite : 'igr_tests'
)
test(
     'environ',
     environ_test_script,
     env : igr_tests_env,
     suite : 'igr_tests'
)
test(
     'environ2',
     environ2_test_script,
     env : igr_tests_env,
     suite : 'igr_tests'
)
test(
     'ps-environ',
     psenviron_test_script,
     env : igr_tests_env,
     suite : 'igr_tests'
)
test(
     'chain-to',
     chainto_test_script,
     env : igr_tests_env,
     suite : 'igr_tests'
)
test(
     'force-stop',
     forcestop_test_script,
     env : igr_tests_env,
     suite : 'igr_tests'
)
test(
     'restart',
     restart_test_script,
     env : igr_tests_env,
     suite : 'igr_tests'
)
test(
     'check-basic',
     checkbasic_test_script,
     env : igr_tests_env,
     suite : 'igr_tests'
)
test(
     'check-cycle',
     checkcycle_test_script,
     env : igr_tests_env,
     suite : 'igr_tests'
)
test(
     'check-cycle2',
     checkcycle2_test_script,
     env : igr_tests_env,
     suite : 'igr_tests'
)
test(
     'check-lint',
     checklint_test_script,
     env : igr_tests_env,
     suite : 'igr_tests'
)
test(
     'reload1',
     reload1_test_script,
     env : igr_tests_env,
     suite : 'igr_tests'
)
test(
     'reload2',
     reload2_test_script,
     env : igr_tests_env,
     suite : 'igr_tests'
)
test(
     'no-command-error',
     nocommanderror_test_script,
     env : igr_tests_env,
     suite : 'igr_tests'
)
test(
     'add-rm-dep',
     addrmdep_test_script,
     env : igr_tests_env,
     suite : 'igr_tests'
)
test(
     'var-subst',
     varsubst_test_script,
     env : igr_tests_env,
     suite : 'igr_tests'
)
test(
     'svc-start-fail',
     svcstartfail_test_script,
     env : igr_tests_env,
     suite : 'igr_tests'
)
test(
     'dep-not-found',
     depnotfound_test_script,
     env : igr_tests_env,
     suite : 'igr_tests'
)
test(
     'pseudo-cycle',
     pseudocycle_test_script,
     env : igr_tests_env,
     suite: 'igr_tests'
)
test(
     'before-after',
     beforeafter_test_script,
     env : igr_tests_env,
     suite: 'igr_tests'
)
test(
     'before-after2',
     beforeafter2_test_script,
     env : igr_tests_env,
     suite : 'igr_tests'
)
test(
    'log-via-pipe',
    logviapipe_test_script,
    env : igr_tests_env,
    suite : 'igr_tests'
)
test(
    'catlog',
    catlog_test_script,
    env : igr_tests_env,
    suite : 'igr_tests'
)
test(
    'offline-enable',
    offlineenable_test_script,
    env : igr_tests_env,
    suite : 'igr_tests'
)
