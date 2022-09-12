## Experimetal meson build system for dinit
project(
    'dinit', 
    'cpp', 
    version : '0.15.1',
    license : 'Apache',
    default_options : 'cpp_std = c++11')

## Includes
## We include `src/includes/` & `dasynq/include/` as defualt_incdir 
default_incdir = include_directories('src/includes/', 'dasynq/include/')
dinit_needed_files = ['src/dinit-main.cc',
                      'src/dinit.cc',
                      'src/load-service.cc',
                      'src/service.cc',
                      'src/proc-service.cc',
                      'src/baseproc-service.cc',
                      'src/control.cc',
                      'src/dinit-log.cc',
                      'src/run-child-proc.cc',
                      'src/options-processing.cc',
                      'src/dinit-env.cc']

## Use -D_GLIBCXX_USE_CXX11_ABI?
if get_option('use_new_abi').auto()
# ToDo. We must verify libstdc++ version and set -D_GLIBCXX_USE_CXX11_ABI to 1 or 0
elif get_option('use_new_abi').enabled()
  add_global_arguments('-D_GLIBCXX_USE_CXX11_ABI=1', language : 'cpp')
elif get_option('use_new_abi').disabled()
  add_global_arguments('-D_GLIBCXX_USE_CXX11_ABI=0', language : 'cpp')
endif

## ToDo; its must handeled with `meson_options.txt` instead of force them
## Use -Wall -flto -fno-rtti -fno-plt
add_global_arguments('-Wall', language : 'cpp')
add_global_arguments('-flto', language : 'cpp')
add_global_arguments('-fno-rtti', language : 'cpp')
add_global_arguments('-fno-plt', language : 'cpp')

## Outputs
executable('dinit', dinit_needed_files , include_directories : default_incdir)
executable('dinitctl', 'src/dinitctl.cc', include_directories : default_incdir)
executable('dinitcheck', 'src/dinitcheck.cc', 'src/options-processing.cc', include_directories : default_incdir)
executable('dinit-monitor', 'src/dinit-monitor.cc', include_directories : default_incdir)
