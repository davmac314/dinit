# Included from top-level meson.build

## Includes
for_tests_incdir = include_directories('test-includes/')
libtests = static_library(
    'for_tests_general_objects',
    'test-dinit.cc',
    'test-bpsys.cc',
    'test-run-child-proc.cc',
    '../service.cc',
    '../proc-service.cc',
    '../load-service.cc',
    '../baseproc-service.cc',
    '../dinit-log.cc',
    '../control.cc',
    link_with: libdinit,
    include_directories: [ for_tests_incdir, default_incdir ],
    install: false
)

## Outputs
# Unit tests
tests_exec = executable(
    'tests',
    'tests.cc',
    link_with: libtests,
    include_directories: [ for_tests_incdir, default_incdir ],
    dependencies: [libcap_dep]
)
proctests_exec = executable(
    'proctests',
    'proctests.cc',
    link_with: libtests,
    include_directories: [ for_tests_incdir, default_incdir ],
    dependencies: [libcap_dep]
)
loadtests_exec = executable(
    'loadtests',
    'loadtests.cc',
    link_with: libtests,
    include_directories: [ for_tests_incdir, default_incdir ],
    dependencies: [libcap_dep]
)
envtests_exec = executable(
    'envtests',
    'envtests.cc',
    link_with: libtests,
    include_directories: [ for_tests_incdir, default_incdir ],
    dependencies: [libcap_dep]
)
test('tests', tests_exec, suite: 'unit_tests')
test('proctests', proctests_exec, suite: 'unit_tests')
test('loadtests', loadtests_exec, workdir: meson.current_source_dir(), suite: 'unit_tests')
test('envtests', envtests_exec, suite: 'unit_tests')
