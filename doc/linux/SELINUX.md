# Dinit SELinux Awareness

Dinit has support for basic SELinux awareness. This document is intended to
outline the extent and inner workings of Dinit's SELinux awareness. The reader
is assumed to be knowledgeable about the basics of [SELinux](https://github.com/SELinuxProject/selinux-notebook)
and Dinit.

Dinit needs to be built with SELinux support (see [BUILD](/BUILD)) to enable the features that are
mentioned in this document.

## Loading the system SELinux policy
When booted as the system init system, dinit by default will attempt to load the
system's SELinux policy and transition itself to a context specified by that policy
if not already done so in earlier boot (e.g. by an initramfs). This behaviour may be
disabled by passing dinit the `--disable-selinux-policy` flag.

If not already mounted in earlier boot (e.g. by an initramfs), dinit will mount `/sys`,
and selinuxfs (typically `/sys/fs/selinux`). This occurs before any services are started,
as loading the SELinux policy is the first thing dinit does.

The following flowchart provides an overview of the process of loading the policy:
```mermaid
flowchart TD
    A[Start] --> B{"Is dinit running as the system manager?"}
    B -->|Yes| C{Have we been requested to not load the SELinux policy?}
    C -->|No| D[Continue rest of dinit initialization]
    C -->|Yes| E[Is the SELinux policy already loaded?]
    E -->|Yes| D
    E --> |No| G[Attempt to mount /proc]
    G --> J[Attempt to load the SELinux policy]
    J --> K{Did the SELinux policy load succeed?}
    K -->|Yes| L[Attempt to calculate our new context and transition]
    K -->|No| M{Was enforcing mode requested?}
    M -->|Yes| I[Error exit early]
    M -->|No| D
    L --> N{Did we successfully transition?}
    N -->|Yes| P{Did we mount /proc?}
    N -->|No| O[Log an error]
    O --> P
    P -->|Yes| Q[Unmount /proc]
    P -->|No| D
    Q --> D
```
