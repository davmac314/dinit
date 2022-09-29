# Included from top-level meson.build

## Prepare integration tests
run_command([find_program('cp'), '-r', './', '@0@'.format(meson.current_build_dir())], check : true)

## Outputs
# Integration tests
basic_test_script = find_program('@0@/basic/run-test.sh'.format(meson.current_build_dir()))
environ_test_script = find_program('@0@/environ/run-test.sh'.format(meson.current_build_dir()))
psenviron_test_script = find_program('@0@/ps-environ/run-test.sh'.format(meson.current_build_dir()))
chainto_test_script = find_program('@0@/chain-to/run-test.sh'.format(meson.current_build_dir()))
forcestop_test_script = find_program('@0@/force-stop/run-test.sh'.format(meson.current_build_dir()))
restart_test_script = find_program('@0@/restart/run-test.sh'.format(meson.current_build_dir()))
checkbasic_test_script = find_program('@0@/check-basic/run-test.sh'.format(meson.current_build_dir()))
checkcycle_test_script = find_program('@0@/check-cycle/run-test.sh'.format(meson.current_build_dir()))
checklint_test_script = find_program('@0@/check-lint/run-test.sh'.format(meson.current_build_dir()))
reload1_test_script = find_program('@0@/reload1/run-test.sh'.format(meson.current_build_dir()))
reload2_test_script = find_program('@0@/reload2/run-test.sh'.format(meson.current_build_dir()))
nocommanderror_test_script = find_program('@0@/no-command-error/run-test.sh'.format(meson.current_build_dir()))
addrmdep_test_script = find_program('@0@/add-rm-dep/run-test.sh'.format(meson.current_build_dir()))
varsubst_test_script = find_program('@0@/var-subst/run-test.sh'.format(meson.current_build_dir()))
svcstartfail_test_script = find_program('@0@/svc-start-fail/run-test.sh'.format(meson.current_build_dir()))
test('basic',
     basic_test_script,
     env : ['IS_MESON=true', 'APPS_PATH=' + APPS_PATH])
test('environ',
     environ_test_script,
     env : ['IS_MESON=true', 'APPS_PATH='+ APPS_PATH],
     is_parallel : false)
test('ps-environ',
     psenviron_test_script,
     env : ['IS_MESON=true', 'APPS_PATH=' + APPS_PATH])
test('chain-to',
     chainto_test_script,
     env : ['IS_MESON=true', 'APPS_PATH=' + APPS_PATH])
test('force-stop',
     forcestop_test_script,
     env : ['IS_MESON=true', 'APPS_PATH=' + APPS_PATH])
test('restart',
     restart_test_script,
     env : ['IS_MESON=true', 'APPS_PATH=' + APPS_PATH])
test('check-basic',
     checkbasic_test_script,
     env : ['IS_MESON=true', 'APPS_PATH=' + APPS_PATH])
test('check-cycle',
     checkcycle_test_script,
     env : ['IS_MESON=true', 'APPS_PATH=' + APPS_PATH])
test('check-lint',
     checklint_test_script,
     env : ['IS_MESON=true', 'APPS_PATH=' + APPS_PATH])
test('reload1',
     reload1_test_script,
     env : ['IS_MESON=true', 'APPS_PATH=' + APPS_PATH])
test('reload2',
     reload2_test_script,
     env : ['IS_MESON=true', 'APPS_PATH=' + APPS_PATH])
test('no-command-error',
     nocommanderror_test_script,
     env : ['IS_MESON=true', 'APPS_PATH=' + APPS_PATH])
test('add-rm-dep',
     addrmdep_test_script,
     env : ['IS_MESON=true', 'APPS_PATH=' + APPS_PATH])
test('var-subst',
      varsubst_test_script,
     env : ['IS_MESON=true', 'APPS_PATH=' + APPS_PATH])
test('svc-start-fail',
     svcstartfail_test_script,
     env : ['IS_MESON=true', 'APPS_PATH=' + APPS_PATH])
