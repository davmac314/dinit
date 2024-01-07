# Included from top-level meson.build

igr_tests_env += [
     'IGR_OUTPUT_BASE=@0@'.format(meson.current_build_dir()),
     'DEBUG=yes'
]

## Integration tests
# We use shell globbing because meson doesn't do that for us.
igr_tests = run_command('/bin/sh', '-c', 'echo */', check: true).stdout().split( )

foreach igr_test : igr_tests
     igr_test = igr_test.strip('/')
     if run_command('grep', '\"' + igr_test + '\"', 'igr-runner.cc', check: false).returncode() == 0
          test(
               igr_test,
               files(meson.current_source_dir() + igr_test + '/run-test.sh'),
               env: igr_tests_env,
               suite: 'igr_tests'
          )
     endif
endforeach
