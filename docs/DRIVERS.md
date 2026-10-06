# Drivers (Phase 4)

## Boot-time discovery

1. `acpi_init()` finds the RSDP (passed by GRUB), walks RSDT/XSDT to the MADT and records the I/O APICs and ISA interrupt overrides. Without a MADT the kernel falls back to the standard defaults.
2. `ioapic_init()` maps each I/O APIC through `vmm_ioremap()` and masks every pin. `ioapic_route_isa(irq, vector)` honours the overrides (QEMU remaps the timer and the ACPI SCI).
3. `pci_init()` scans bus 0 with config mechanism #1 and recurses through PCI-to-PCI bridges. BAR sizes are measured with the write-ones trick, with decoding disabled while probing.
4. `drivers_register_builtin()` registers every driver; `drivers_probe_all()` binds them to devices.

## The driver shim

```c
struct driver {
    const char *name;
    const struct pci_match *pci_ids; /* NULL-terminated; class/subclass/prog-if or vendor/device */
    int (*probe)(struct pci_dev *dev);   /* PCI drivers */
    int (*init)(void);                   /* platform drivers (e.g. the PS/2 keyboard) */
};
int driver_register(struct driver *drv);
```

Drivers never touch the core directly: they use `irq_register()`, `ioapic_route_isa()`, `vmm_ioremap()`, `pci_*` config accessors and `blk_register()`. A driver is bound at most once per device, and a probe that returns an error leaves the device unbound.

## PS/2 keyboard

8042 controller, scancode set 1 (translation on). The IRQ handler pushes decoded key events into a ring buffer and wakes a wait queue; `kbd_read()` blocks. The 8042 raises no new interrupt while its output buffer is full, so initialisation drains it before and after routing IRQ 1. `make input-test` sends real key presses through the QEMU monitor and checks the decoded events.

## Storage

| Layer | File | Notes |
|---|---|---|
| Block layer | `drivers/block.c` | `struct blockdev`, `blk_read/write/read_bytes`, argument validation, statistics |
| ATA PIO | `drivers/ata.c` | LBA28/48, polling, compat and native-mode channels, IDENTIFY parsing |
| AHCI | `drivers/ahci.c` | one command slot, 64 KiB bounce buffer, polling |

Both drivers are **polled** (no DMA interrupts yet); that keeps them simple and deterministic until a user-space scheduler needs async I/O. Devices are named `hda…` (ATA) and `sda…` (AHCI).

## Tests

`drivers_selftest()` (PS/2 decoder, PCI, I/O APIC routing) and `storage_selftest()` (block layer on a RAM disk, then read/write/verify on the real ATA and AHCI disks, with host-side checks of what reached the image).
