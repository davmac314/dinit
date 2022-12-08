# Included from top-level meson.build

## Prepare integration tests
run_command([
    'cp',
    '-r',
    './',
    meson.current_build_dir()
    ],
    check : true
)

## Outputs
# Integration tests
basic_test_script = files(meson.current_build_dir() + '/basic/run-test.sh')
environ_test_script = files(meson.current_build_dir() + '/environ/run-test.sh')
psenviron_test_script = files(meson.current_build_dir() + '/ps-environ/run-test.sh')
chainto_test_script = files(meson.current_build_dir() + '/chain-to/run-test.sh')
forcestop_test_script = files(meson.current_build_dir() + '/force-stop/run-test.sh')
restart_test_script = files(meson.current_build_dir() + '/restart/run-test.sh')
checkbasic_test_script = files(meson.current_build_dir() + '/check-basic/run-test.sh')
checkcycle_test_script = files(meson.current_build_dir() + '/check-cycle/run-test.sh')
checklint_test_script = files(meson.current_build_dir() + '/check-lint/run-test.sh')
reload1_test_script = files(meson.current_build_dir() + '/reload1/run-test.sh')
reload2_test_script = files(meson.current_build_dir() + '/reload2/run-test.sh')
nocommanderror_test_script = files(meson.current_build_dir() + '/no-command-error/run-test.sh')
addrmdep_test_script = files(meson.current_build_dir() + '/add-rm-dep/run-test.sh')
varsubst_test_script = files(meson.current_build_dir() + '/var-subst/run-test.sh')
svcstartfail_test_script = files(meson.current_build_dir() + '/svc-start-fail/run-test.sh')
test(
     'basic',
     basic_test_script,
     env : igr_tests_env
)
test(
     'environ',
     environ_test_script,
     env : igr_tests_env,
     is_parallel : false
)
test(
     'ps-environ',
     psenviron_test_script,
     env : igr_tests_env
)
test(
     'chain-to',
     chainto_test_script,
     env : igr_tests_env
)
test(
     'force-stop',
     forcestop_test_script,
     env : igr_tests_env
)
test(
     'restart',
     restart_test_script,
     env : igr_tests_env
)
test(
     'check-basic',
     checkbasic_test_script,
     env : igr_tests_env
)
test(
     'check-cycle',
     checkcycle_test_script,
     env : igr_tests_env
)
test(
     'check-lint',
     checklint_test_script,
     env : igr_tests_env
)
test(
     'reload1',
     reload1_test_script,
     env : igr_tests_env
)
test(
     'reload2',
     reload2_test_script,
     env : igr_tests_env
)
test(
     'no-command-error',
     nocommanderror_test_script,
     env : igr_tests_env
)
test(
     'add-rm-dep',
     addrmdep_test_script,
     env : igr_tests_env
)
test(
     'var-subst',
     varsubst_test_script,
     env : igr_tests_env
)
test(
     'svc-start-fail',
     svcstartfail_test_script,
     env : igr_tests_env
)
