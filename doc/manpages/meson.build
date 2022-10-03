# Included from top-level meson.build

# doc/manpages's Defines
man5 = meson.current_build_dir() + 'dinit-service.5'
man8 = [meson.current_build_dir() + 'dinit.8',
        meson.current_build_dir() + 'dinitcheck.8',
        meson.current_build_dir() + 'dinitctl.8',
        meson.current_build_dir() + 'dinit-monitor.8']
if install_shutdown_man
  man8 += meson.current_build_dir() + 'shutdown.8'
endif

## Generate man-pages
# Dinit need 'm4' to gen man-pages
find_program('m4', required : true)
# Gen man-pages via script
# Note: We dont use 'env' in 'run_command()' because it's brake compatibility with older meson(s) than >0.50.0.
# Instead of that; use argument in scripts.
#run_command('generate-man-pages.sh',
#            env : ['SOURCE_MAN_PATH=@0@'.format(meson.current_source_dir()),
#            'TARGET_MAN_PATH=@0@'.format(meson.current_build_dir()),
#            'VERSION=' + version,
#            'MONTH=' + month,
#            'YEAR=' + year], check : true)
run_command('generate-man-pages.sh',
            meson.current_source_dir(),
            meson.current_build_dir(),
            version,
            month,
            year,
            check : true)

install_data(man5,
             install_dir : mandir + '/man5/')
install_data(man8,
             install_dir : mandir + '/man8/')
