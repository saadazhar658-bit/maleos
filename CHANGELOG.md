# Changelog

## Unreleased (Phase 8a)
- Pipes (`pipe`, `dup`, `dup2`), descriptor inheritance in `spawn`, shell pipelines.
- New utilities: `wc`, `grep`, `head`, `cpus`.
- Syscalls 40-42; `abi_spawn_attr` grew `flags`, `stdin_fd` and `stdout_fd`.
- Open files and sockets are reference-counted: `dup`/`dup2` work on every descriptor kind and
  children inherit them. `abi_spawn_attr.stdin_path` and shell `<` redirection.

## v0.1.0
First tagged release: everything from phases 0 to 7c.

- **Boot and memory (0-2):** Multiboot2/GRUB, higher-half kernel, physical and virtual memory
  managers with W^X, kernel heap, direct map.
- **Scheduling (3):** 100 Hz APIC timer, eight strict priorities, wait queues, mutexes, IPC.
- **Drivers and buses (4):** ACPI/MADT, I/O APIC, PCI, PS/2 keyboard, ATA and AHCI.
- **File systems (5):** VFS, ramfs, tar initrd, read-only ext2.
- **User space (6):** `syscall` interface, ELF loader, per-process address spaces, `init`,
  shell, utilities, SMEP/SMAP where available.
- **Hardening (7a):** stack protector, random source, user ASLR (stack, heap), syscall fuzzer.
- **Network (7b):** e1000, ARP, IPv4, ICMP, UDP, TCP, sockets, `ping`, `nc`, `wget`, `httpd`.
- **SMP (7c):** up to 8 CPUs; user code runs in parallel, kernel code under one big lock.

Not in this release: kernel image KASLR, fine-grained kernel locking, fork/exec, pipes,
signals, writable ext2, DHCP/DNS, FPU/SSE state handling.
