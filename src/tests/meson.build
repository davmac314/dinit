# Included from top-level meson.build

## Includes
for_tests_incdir = include_directories('includes/', '../../dasynq/include/')
for_cptests_incdir = include_directories('cptests/includes', '../../dasynq/include/') #ToDo; we need to another meson.build for cptests
for_tests_dinit_sources = ['../service.cc', 
                           '../proc-service.cc',
                           '../dinit-log.cc',
                           '../load-service.cc',
                           '../baseproc-service.cc',
                           '../dinit-env.cc']
for_tests_general_sources = ['test-dinit.cc',
                             'test-bpsys.cc',
                             'test-run-child-proc.cc']

## Write mconfig.h from top-level meson.build
configure_file(output : 'mconfig.h',
               configuration : conf_data)

## Outputs
# Unit tests
tests_exec = executable('tests', 'tests.cc', for_tests_general_sources, for_tests_dinit_sources, include_directories : for_tests_incdir)
proctests_exec = executable('proctests', 'proctests.cc', for_tests_general_sources, for_tests_dinit_sources, include_directories : for_tests_incdir)
loadtests_exec = executable('loadtests', 'loadtests.cc', for_tests_general_sources, for_tests_dinit_sources, include_directories : for_tests_incdir)
envtests_exec = executable('envtests', 'envtests.cc', for_tests_general_sources, for_tests_dinit_sources, include_directories : for_tests_incdir)
cptests_exec = executable('cptests', 'cptests/cptests.cc', for_tests_general_sources, for_tests_dinit_sources, '../control.cc', include_directories : for_cptests_incdir)
test('tests', tests_exec)
test('proctests', proctests_exec)
test('loadtests', loadtests_exec, should_fail : true)
test('envtests', envtests_exec)
test('cptests', cptests_exec)
