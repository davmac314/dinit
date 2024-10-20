# Included from top-level meson.build

## Includes
incdir = include_directories(
    '../../../',
    '../test-includes/',
    '../../includes/',
    '../../../dasynq/include/'
)
sources = [
    '../test-bpsys.cc',
    '../test-dinit.cc',
    '../test-run-child-proc.cc',
    '../../control.cc',
    '../../dinit-log.cc',
    '../../service.cc',
    '../../load-service.cc',
    '../../proc-service.cc',
    '../../baseproc-service.cc',
    '../../dinit-env.cc',
    '../../settings.cc'
]

## dinit with libfuzzer can't be compiled without clang/clang++. For some reasons, Meson don't support set default compiler in default_options.
if fuzzer and compiler.get_id() != 'clang'
    error('Fuzz target must compiles with clang instead of @0@'.format(compiler.get_id()))
endif

## Outputs
# Cptests
if unit_tests
    cptests_exec = executable(
        'cptests',
        'cptests.cc',
        sources,
        include_directories: incdir,
        dependencies: [libcap_dep]
    )
    test('cptests', cptests_exec, suite: 'unit_tests')
endif
# LLVM's Libfuzzer
if fuzzer
    executable(
        'fuzz',
        'fuzz.cc',
        sources,
        include_directories: incdir,
        install: false,
        cpp_args: '-fsanitize=address,undefined,fuzzer-no-link,leak',
        link_args: '-fsanitize=fuzzer,address,undefined,leak'
    )
endif
