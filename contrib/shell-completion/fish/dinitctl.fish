# dinitctl completion
# Author: Aicaya Maro <aicaya@posteo.net>

set -l commands \
    start stop status is-started is-failed restart wake release unpin \
    unload reload list shutdown add-dep rm-dep enable disable \
    trigger untrigger setenv unsetenv catlog signal

set -l loaded_service_commands \
    stop status is-started is-failed restart wake release unpin \
    unload reload disable trigger untrigger catlog signal

function __fish_dinitctl_need_system_services
    switch $argv[1]
        case root
            true
        case user
            false
        case '*'
            if fish_is_root_user
                true
            else
                false
            end
    end
end

function __fish_dinitctl_list_all_services
    if __fish_dinitctl_need_system_services $argv[1]
        set -f services (path filter -f -- \
            /etc/dinit.d/* \
            /run/dinit.d/* \
            /usr/local/lib/dinit.d/* \
            /lib/dinit.d/* | path basename | path sort --unique)
    else
        set -f services (path filter -f -- $HOME/.config/dinit.d/* |
                path basename)
    end
    set -q services[1]; or return
    printf "%s\n" $services
end

function __fish_dinitctl_list_loaded_services
    if __fish_dinitctl_need_system_services $argv[1]
        dinitctl -s list 2> /dev/null
    else
        dinitctl -u list 2> /dev/null
    end
end

set -l no_comm "not __fish_seen_subcommand_from $commands; and not __fish_seen_argument -l help -l version"
set -l wants_system_service_list "__fish_seen_argument -s s -l system"
set -l wants_user_service_list "__fish_seen_argument -s u -l user"
set -l wants_default_service_list "not __fish_seen_argument -s s -l system -s u -l user"

# no file completion by default
complete -f -c dinitctl

# General options
complete -c dinitctl -n "$no_comm; and not __fish_seen_argument -s s -l system -s u -l user -s p -l socket-path -l use-passed-cfd -l quiet -s o -l offline -s d -l services-dir" -l help -d 'display help text and then exit'
complete -c dinitctl -n "$no_comm; and not __fish_seen_argument -s s -l system -s u -l user -s p -l socket-path -l use-passed-cfd -l quiet -s o -l offline -s d -l services-dir" -l version  -d 'display version and then exit'
complete -c dinitctl -n "$no_comm; and not __fish_seen_argument -s s -l system -s u -l user" -s s -l system -d 'control system processes (default if root)'
complete -c dinitctl -n "$no_comm; and not __fish_seen_argument -s s -l system -s u -l user" -s u -l user -d 'control user processes (default if not root)'
complete -c dinitctl -n "$no_comm" -Fr -s p -l socket-path -d 'path to socket used for communicating with service manager daemon'
complete -c dinitctl -n "$no_comm; and not __fish_seen_argument -l use-passed-cfd" -l use-passed-cfd -d 'use pre-opened connection instead of connecting to service daemon'
complete -c dinitctl -n "$no_comm; and not __fish_seen_argument -l quiet" -l quiet -d 'suppress status output, except for errors'
complete -c dinitctl -n "$no_comm; and not __fish_seen_argument -s o -l offline" -s o -l offline -d 'do not communicate with dinit daemon (enable/disable only)'
complete -c dinitctl -n $no_comm -Fr -s d -l services-dir -d 'specify service directory'

# Commands
complete -c dinitctl -n $no_comm -a start -d 'start service'
complete -c dinitctl -n $no_comm -a stop -d 'stop service'
complete -c dinitctl -n $no_comm -a status -d 'report status of service'
complete -c dinitctl -n $no_comm -a is-started -d 'check if service started'
complete -c dinitctl -n $no_comm -a is-failed -d 'check if service failed'
complete -c dinitctl -n $no_comm -a restart -d 'restart service'
complete -c dinitctl -n $no_comm -a wake -d 'start service without marking as explicitly activated'
complete -c dinitctl -n $no_comm -a release -d 'clear explicit activation mark from service'
complete -c dinitctl -n $no_comm -a unpin -d 'remove start- and stop- pins from service'
complete -c dinitctl -n $no_comm -a unload -d 'completely unload a stopped service with no dependents'
complete -c dinitctl -n $no_comm -a reload -d 'attempt to reload service description'
complete -c dinitctl -n $no_comm -a list -d 'list services and their state'
complete -c dinitctl -n $no_comm -a shutdown -d 'stop all services and terminate dinit'
complete -c dinitctl -n $no_comm -a add-dep -d 'add dependency between two services'
complete -c dinitctl -n $no_comm -a rm-dep -d 'remove dependency between two services'
complete -c dinitctl -n $no_comm -a enable -d 'enable persistent service'
complete -c dinitctl -n $no_comm -a disable -d 'disable persistent service'
complete -c dinitctl -n $no_comm -a trigger -d 'mark triggered service as set'
complete -c dinitctl -n $no_comm -a untrigger -d 'clear trigger for triggered service'
complete -c dinitctl -n $no_comm -a setenv -d 'export one or more variables into activation environment'
complete -c dinitctl -n $no_comm -a unsetenv -d 'unset one or more variables in activation environment'
complete -c dinitctl -n $no_comm -a catlog -d 'show contents of log buffer for service'
complete -c dinitctl -n $no_comm -a signal -d 'send signal to process associated with service'

# Command options
complete -c dinitctl -n "__fish_seen_subcommand_from start stop restart wake; and not __fish_seen_argument -l no-wait" -l no-wait -d 'exit immediately after issuing command'
complete -c dinitctl -n "__fish_seen_subcommand_from start stop; and not __fish_seen_argument -l pin" -l pin -d 'pin service in requested state'
complete -c dinitctl -n "__fish_seen_subcommand_from stop restart; and not __fish_seen_argument -l force" -l force -d 'stop service along with dependents'
complete -c dinitctl -n "__fish_seen_subcommand_from stop restart release; and not __fish_seen_argument -l ignore-unstarted" -l ignore-unstarted -d 'ignore non-started/nonexistent services'
complete -c dinitctl -n "__fish_seen_subcommand_from catlog; and not __fish_seen_argument -l clear" -l clear -d 'clear log buffer for service'
complete -c dinitctl -n "__fish_seen_subcommand_from enable disable; and not __fish_seen_argument -l from" -l from -d "specify dependent service"
complete -c dinitctl -n "__fish_seen_subcommand_from signal; and not __fish_seen_argument -s l -l list" -s l -l list -d 'list all supported signal names'

# add-dep, rm-dep: List dependency types
set -l subcmd_add_rm_dep "__fish_seen_subcommand_from add-dep rm-dep"
set -l dependency_set "__fish_seen_subcommand_from need milestone waits-for"
complete -c dinitctl -n "$subcmd_add_rm_dep; and not $dependency_set" -ra "need" -d "hard dependency, must be running alongside service"
complete -c dinitctl -n "$subcmd_add_rm_dep; and not $dependency_set" -ra "milestone" -d "dependency must start successfully, but isn't fully needed after"
complete -c dinitctl -n "$subcmd_add_rm_dep; and not $dependency_set" -ra "waits-for" -d "dependency must at least try to start, but service will load regardless"
complete -c dinitctl -n "$wants_system_service_list; and $subcmd_add_rm_dep; and $dependency_set" -ra '(__fish_dinitctl_list_all_services root)'
complete -c dinitctl -n "$wants_user_service_list; and $subcmd_add_rm_dep; and $dependency_set" -ra '(__fish_dinitctl_list_all_services user)'
complete -c dinitctl -n "$wants_default_service_list; and $subcmd_add_rm_dep; and $dependency_set" -ra '(__fish_dinitctl_list_all_services)'

# start, enable: List all available services
complete -c dinitctl -n "$wants_system_service_list; and __fish_seen_subcommand_from start enable" -ra '(__fish_dinitctl_list_all_services root)'
complete -c dinitctl -n "$wants_user_service_list; and __fish_seen_subcommand_from start enable" -ra '(__fish_dinitctl_list_all_services user)'
complete -c dinitctl -n "$wants_default_service_list; and __fish_seen_subcommand_from start enable" -ra '(__fish_dinitctl_list_all_services)'

# every other service-related command: List all loaded services
complete -c dinitctl -n "$wants_system_service_list; and __fish_seen_subcommand_from $loaded_service_commands" -ra '(__fish_dinitctl_list_loaded_services root | string match -r -g "^.{10} ([\S]+)")'
complete -c dinitctl -n "$wants_user_service_list; and __fish_seen_subcommand_from $loaded_service_commands" -ra '(__fish_dinitctl_list_loaded_services user | string match -r -g "^.{10} ([\S]+)")'
complete -c dinitctl -n "$wants_default_service_list; and __fish_seen_subcommand_from $loaded_service_commands" -ra '(__fish_dinitctl_list_loaded_services | string match -r -g "^.{10} ([\S]+)")'
