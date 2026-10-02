# Repository Layout

```text
maleos/
├── .github/
│   ├── workflows/ci.yml          Build, ISO, and headless boot test
│   ├── ISSUE_TEMPLATE/           Bug and feature templates
│   └── PULL_REQUEST_TEMPLATE.md
├── docs/
│   ├── CODING_STANDARDS.md       Style and safety rules
│   └── REPOSITORY_LAYOUT.md      This file
├── iso/boot/grub/grub.cfg        GRUB menu packaged into the ISO
├── scripts/
│   ├── install-deps.sh           Install host packages (Debian/Ubuntu)
│   ├── build-toolchain.sh        Optional x86_64-elf cross compiler build
│   └── boot-test.sh              Headless QEMU boot test
├── src/
│   ├── arch/x86_64/              Machine-specific code (HAL, boot)
│   ├── include/                  Public kernel headers
│   └── kernel/                   Architecture-independent core
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
| Memory manager (PMM/VMM/heap) | `src/kernel/mm/` |
| Scheduler, threads, IPC | `src/kernel/sched/`, `src/kernel/ipc/` |
| Drivers and driver shim | `src/drivers/` |
| VFS and filesystems | `src/fs/` |
| Network stack | `src/net/` |

`make` discovers every `.c` and `.asm` file under `src/` automatically, so new files need no Makefile changes.

## Build output

Everything generated goes into `build/` (git-ignored): `kernel.elf`, the staged `isodir/`, and `maleos.iso`.
