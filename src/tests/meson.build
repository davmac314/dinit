# Included from top-level meson.build

## Includes
for_tests_incdir = include_directories(
    '../../',
    'includes/',
    '../../dasynq/include/'
)
for_tests_dinit_sources = [
    '../service.cc', 
    '../proc-service.cc',
    '../dinit-log.cc',
    '../load-service.cc',
    '../baseproc-service.cc',
    '../dinit-env.cc'
]
for_tests_general_sources = [
    'test-dinit.cc',
    'test-bpsys.cc',
    'test-run-child-proc.cc'
]

## Prepare loadtests
run_command([
    'cp',
    '-r',
    meson.current_source_dir() + '/test-services',
    meson.current_build_dir()
    ],
    check : true
)

## Outputs
# Unit tests
tests_exec = executable(
    'tests',
    'tests.cc',
    for_tests_general_sources,
    for_tests_dinit_sources,
    include_directories : for_tests_incdir
)
proctests_exec = executable(
    'proctests',
    'proctests.cc',
    for_tests_general_sources,
    for_tests_dinit_sources,
    include_directories : for_tests_incdir
)
loadtests_exec = executable(
    'loadtests',
    'loadtests.cc',
    for_tests_general_sources,
    for_tests_dinit_sources,
    include_directories : for_tests_incdir
)
envtests_exec = executable(
    'envtests',
    'envtests.cc',
    for_tests_general_sources,
    for_tests_dinit_sources, 
    include_directories : for_tests_incdir
)
test('tests', tests_exec, suite : 'unit_tests')
test('proctests', proctests_exec, suite : 'unit_tests')
test('loadtests', loadtests_exec, workdir : meson.current_build_dir(), suite : 'unit_tests')
test('envtests', envtests_exec, suite : 'unit_tests')
subdir('cptests/')
