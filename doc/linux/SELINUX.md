# Dinit SELinux Awareness

Dinit has support for basic SELinux awareness. This document is intended to
outline the extent and inner workings of Dinit's SELinux awareness. The reader
is assumed to be knowledgeable about the basics of [SELinux](https://github.com/SELinuxProject/selinux-notebook) and Dinit.

Dinit needs to be built with SELinux support to enable any of the features that
are mentioned in this document.

## Loading the system SELinux policy
When booted as the system init system, dinit by default will attempt to load the
system's SELinux policy and transition itself to a context specified by that policy
if not already done so in earlier boot (e.g. by an initramfs). This behaviour may be
disabled by passing dinit the `--disable-selinux-policy` flag. As dinit will always
be PID1 in this senario, this can be done by appending the flag to the kernel cmdline.

If not already mounted in earlier boot (e.g. by an initramfs), dinit will mount `/sys`,
and selinuxfs (typically `/sys/fs/selinux`) during the call to `selinux_init_load_policy(3)`.

The following flowchart provides an overview of the process of loading the policy:
```mermaid
flowchart TD
    A[Start] --> B{"Is dinit running as the init system (PID1)?"}
    B -->|Yes| C{Have we been requested to not load the SELinux policy?}
    B -->|No| D[Continue rest of dinit initialization]
    C -->|Yes| D
    C -->|No| E[Is the SELinux policy already loaded?]
    E -->|Yes| D
    E --> |No| F{Is /proc mounted?}
    F --> |Yes| J
    F --> |No| G[Attempt to mount /proc]
    G --> H{Could we successfully mount /proc?}
    H --> |Yes| J
    H -->|No| I[Error exit early]
    J[Attempt to load the SELinux policy]
    J --> K{Did the SELinux policy load succeed?}
    K -->|Yes| L[Attempt to calculate our new context and transition]
    K -->|No| I
    L --> M{Did we successfully transition?}
    M -->|Yes| O{Did we mount /proc?}
    M -->|No| N[Log an error to stderr]
    N --> O
    O -->|Yes| P[Unmount /proc]
    O -->|No| D
    P --> D
```
