# Repository Layout

```text
maleos/
├── .github/
│   ├── workflows/ci.yml          Build, ISO, and headless boot test
│   ├── ISSUE_TEMPLATE/           Bug and feature templates
│   └── PULL_REQUEST_TEMPLATE.md
├── docs/
│   ├── CODING_STANDARDS.md       Style and safety rules
│   ├── MEMORY.md                 Memory management design (Phase 2)
│   ├── SCHEDULER.md              Interrupts, scheduler, locking, IPC (Phase 3)
│   ├── DRIVERS.md                ACPI/PCI, driver shim, keyboard, storage (Phase 4)
│   ├── FILESYSTEM.md             VFS, ramfs, initrd, ext2 (Phase 5)
│   └── REPOSITORY_LAYOUT.md      This file
├── iso/boot/grub/grub.cfg        GRUB menu packaged into the ISO
├── scripts/
│   ├── install-deps.sh           Install host packages (Debian/Ubuntu)
│   ├── build-toolchain.sh        Optional x86_64-elf cross compiler build
│   ├── boot-test.sh              Headless QEMU boot test (attaches test disks)
│   ├── input-test.sh             Keyboard end-to-end test via the QEMU monitor
│   ├── mkdisks.sh                Build the ext2 test disk images
│   └── mkinitrd.sh               Pack initrd/ into build/initrd.tar
├── initrd/                       Files unpacked into "/" at boot
├── src/
│   ├── arch/x86_64/              boot.asm, GDT/IDT/ISR stubs, APIC, context switch, serial, VGA
│   ├── drivers/                  PCI, driver shim, keyboard, block layer, ATA, AHCI
│   ├── fs/                       VFS core, ramfs, tar, ext2, filesystem self-test
│   ├── include/{arch,kernel,mm,drivers,fs}/ Public headers
│   └── kernel/
│       ├── main.c                kmain() and boot sequence
│       ├── bootinfo.c            Multiboot2 parsing
│       ├── printk.c, string.c    Kernel printf, mem*/str* helpers
│       ├── sched.c, sync.c       Scheduler, threads, mutex/semaphore
│       ├── ipc.c, kstack.c       Message ports, per-thread kernel stacks
│       ├── spinlock.c            Deadlock report for spinlocks
│       └── mm/                   pmm.c, vmm.c, heap.c, selftest.c
├── CONTRIBUTING.md
├── LICENSE
├── Makefile
├── linker.ld                     Kernel memory layout
└── README.md
```

## Where new code goes

| Subsystem | Location (planned) |
|---|---|
| Boot, GDT/IDT, interrupts | `src/arch/x86_64/` |
| Memory manager (PMM/VMM/heap) | `src/kernel/mm/` (done) |
| Scheduler, threads, IPC | `src/kernel/` (done) |
| Drivers and driver shim | `src/drivers/` (done) |
| VFS and filesystems | `src/fs/` (done) |
| Network stack | `src/net/` |

`make` discovers every `.c` and `.asm` file under `src/` automatically, so new files need no Makefile changes.

## Build output

Everything generated goes into `build/` (git-ignored): `kernel.elf`, `initrd.tar`, the staged `isodir/`, `maleos.iso` and `disks/` (test images).
