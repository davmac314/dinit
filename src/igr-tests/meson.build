# Included from top-level meson.build

## Prepare integration tests
run_command([find_program('cp'), '-r', './', '@0@/igr-tests/'.format(meson.build_root())], check : true)

## Outputs
# Integration tests
basic_test_script = find_program('@0@/igr-tests/basic/run-test.sh'.format(meson.build_root()))
environ_test_script = find_program('@0@/igr-tests/environ/run-test.sh'.format(meson.build_root()))
psenviron_test_script = find_program('@0@/igr-tests/ps-environ/run-test.sh'.format(meson.build_root()))
chainto_test_script = find_program('@0@/igr-tests/chain-to/run-test.sh'.format(meson.build_root()))
forcestop_test_script = find_program('@0@/igr-tests/force-stop/run-test.sh'.format(meson.build_root()))
restart_test_script = find_program('@0@/igr-tests/restart/run-test.sh'.format(meson.build_root()))
checkbasic_test_script = find_program('@0@/igr-tests/check-basic/run-test.sh'.format(meson.build_root()))
test('basic', basic_test_script, env : ['IS_MESON=true', 'APPS_PATH=@0@/src/'.format(meson.build_root())])
test('environ', environ_test_script, env : ['IS_MESON=true', 'APPS_PATH=@0@/src/'.format(meson.build_root())], is_parallel : false)
test('ps-environ', psenviron_test_script, env : ['IS_MESON=true', 'APPS_PATH=@0@/src/'.format(meson.build_root())])
test('chain-to', chainto_test_script, env : ['IS_MESON=true', 'APPS_PATH=@0@/src/'.format(meson.build_root())])
test('force-stop', forcestop_test_script, env : ['IS_MESON=true', 'APPS_PATH=@0@/src/'.format(meson.build_root())])
test('restart', restart_test_script, env : ['IS_MESON=true', 'APPS_PATH=@0@/src/'.format(meson.build_root())])
test('check-basic', checkbasic_test_script, env : ['IS_MESON=true', 'APPS_PATH=@0@/src/'.format(meson.build_root())])
