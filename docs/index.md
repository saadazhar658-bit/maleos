# Maleos

Maleos is a hybrid x86_64 kernel: Multiboot2 boot, higher-half kernel, preemptive priority
scheduler with SMP, VFS with ramfs and read-only ext2, ATA/AHCI/e1000 drivers, a TCP/IP
stack, and a small user space (ELF loader, shell, utilities) reached through `syscall`.

Start with the [repository layout](REPOSITORY_LAYOUT.md), then read the subsystem pages in
the order of the navigation. The [changelog](CHANGELOG.md) lists what each release contains
and what it does not.

## Build and test
```
make iso                       # build/maleos.iso
make test                      # headless boot with all self-tests
SMP=4 ./scripts/boot-test.sh   # choose the number of virtual CPUs
make shell-test input-test net-test
```

## Known limits
Single-user, static ET_EXEC programs only (no fork/exec, pipes or signals); the kernel is
serialised by one big lock; ext2 is read-only; the network is statically configured; no
kernel KASLR; the random source is not cryptographic. Each page lists its own limits.
