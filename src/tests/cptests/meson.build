## LLVM's Libfuzzer for Dinit

project(
    'dinit-fuzzer',
    'cpp',
    'c',
    version : '0.16.0',
    default_options : [
        'cpp_std = c++11',
        'c_std = c11'
    ]
)

## Includes
fuzz_incdir = include_directories(
    'includes/',
    '../../../dasynq/include'
)
fuzz_sources = [
    '../test-bpsys.cc',
    '../test-dinit.cc',
    '../test-run-child-proc.cc',
    '../../control.cc',
    '../../dinit-log.cc',
    '../../service.cc',
    '../../load-service.cc',
    '../../proc-service.cc',
    '../../baseproc-service.cc',
    '../../dinit-env.cc'
]

## Defines
c_compiler = meson.get_compiler('c')
cpp_compiler = meson.get_compiler('cpp')
shutdown_prefix = get_option('shutdown-prefix')
dinit_control_socket_path = get_option('dinit-control-socket-path')
support_cgroups = get_option('support-cgroups')
conf_data = configuration_data()
sbindir = '' # For fuzzing dinit; we dont need to specify sbindir. so we empty them.

## dinit with libfuzzer can't be compiled without clang/clang++. For some reasons, Meson dont support set default compiler in default_options
if c_compiler.get_id() != 'clang'
    error('Fuzz target must be compiled with clang instead of @0@'.format(c_compiler.get_id()))
endif
if cpp_compiler.get_id() != 'clang'
    error('Fuzz target must be compiled with clang instead of @0@'.format(cpp_compiler.get_id()))
endif

## Prepare compiler/linker arguments for c/c++ files
add_project_arguments('-fsanitize=address,undefined,fuzzer-no-link,leak', language : 'cpp')
add_project_arguments('-fsanitize=fuzzer,address,undefined,leak', language : 'c')

## Prepare mconfig.h
conf_data.set('DINIT_VERSION', '"' + '@0@'.format(meson.project_version()) + '"')
conf_data.set('SYSCONTROLSOCKET', '"' + dinit_control_socket_path + '"')
conf_data.set('SBINDIR', '"' + sbindir + '"')
conf_data.set('SHUTDOWN_PREFIX', '"' + shutdown_prefix + '"')
if support_cgroups.auto()
    if host_machine.system() == 'linux'
        message('System uses "linux"; Support Cgroups = YES')
        conf_data.set('SUPPORT_CGROUPS', '1') 
    else
        message('System not uses "linux"; Support Cgroups = NO')
    endif
elif support_cgroups.enabled()
    conf_data.set('SUPPORT_CGROUPS', '1')
endif
## Write mconfig.h
configure_file(
    output : 'mconfig.h',
    configuration : conf_data
)

executable(
    'fuzz',
    'fuzz.cc',
    fuzz_sources,
    include_directories : fuzz_incdir,
    install : false,
    link_args : '-fsanitize=fuzzer,address,undefined,leak'
)
