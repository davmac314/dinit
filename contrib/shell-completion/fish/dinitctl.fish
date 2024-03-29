# dinitctl
# Autogenerated from man page /usr/share/man/man8/dinitctl.8.gz
# Manually edited 

set commands start stop status restart wake release unpin unload reload list shutdown add-dep rm-dep enable disable setenv

complete -f -c dinitctl -n "not __fish_seen_subcommand_from $commands" -l help -d 'Display brief help text and then exit'
complete -f -c dinitctl -n "not __fish_seen_subcommand_from $commands" -l version -d 'Display version and then exit'
complete -f -c dinitctl -n "not __fish_seen_subcommand_from $commands" -s s -l system -d 'Control the system init process (this is the default when run as root)'
complete -f -c dinitctl -n "not __fish_seen_subcommand_from $commands" -s u -l user -d 'Control the user init process (this is the default when not run as root)'
complete -f -c dinitctl -n "not __fish_seen_subcommand_from $commands" -l socket-path -s p -d 'Specify the path to the socket used for communicating with the service manage…'
complete -f -c dinitctl -n "not __fish_seen_subcommand_from $commands" -l quiet -d 'Suppress status output, except for errors'


complete -f -c dinitctl -n "not __fish_seen_subcommand_from $commands" -a "$commands"

set service_management_commands start stop status restart wake release unpin unload reload

complete -f -c dinitctl -n "__fish_seen_subcommand_from $service_management_commands" -xa '(dinitctl list | string match -r -g "^.{10} ([\w-]+)")'

# complete -c dinitctl -l no-wait -d 'Do not wait for issued command to complete; exit immediately'
# complete -c dinitctl -l pin -d 'Pin the service in the requested state'
# complete -c dinitctl -l force -d 'Stop the service even if it will require stopping other services which depend…'
# complete -c dinitctl -l ignore-unstarted -d 'If the service is not started or doesn\'t exist, ignore the command and return…'
