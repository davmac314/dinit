#!/bin/sh

MODULES=/etc/modules

# Check that the kernel has module support.
[ -e /proc/ksyms -o -e /proc/modules ] || exit 0

case "${1}" in
    start)

        # Exit if there's no modules file or there are no valid entries
        [ -r ${MODULES} ] && grep -Eqv '^($|#)' ${MODULES} || exit 0

        while read module args; do

            # Ignore comments and blank lines.
            case "$module" in
                ""|"#"*) continue ;;
            esac

            # Try to load the module with its arguments
            modprobe ${module} ${args} > /dev/null
        done < ${MODULES}
        ;;
    *)
        echo "Usage: ${0} {start}"
        exit 1
        ;;
esac
