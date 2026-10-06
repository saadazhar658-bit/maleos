#ifndef MALEOS_KERNEL_ACPI_H
#define MALEOS_KERNEL_ACPI_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel/bootinfo.h"

#define ACPI_MAX_IOAPICS 8
#define ACPI_MAX_ISOS 16

struct acpi_ioapic {
    uint8_t id;
    uint32_t addr;
    uint32_t gsi_base;
};

/* Interrupt source override: ISA IRQ `irq` is wired to global system interrupt `gsi`. */
struct acpi_iso {
    uint8_t irq;
    uint32_t gsi;
    uint16_t flags; /* MPS INTI flags: bits 0-1 polarity, bits 2-3 trigger mode */
};

struct acpi_info {
    bool present; /* a valid MADT was found */
    uint32_t lapic_addr;
    struct acpi_ioapic ioapics[ACPI_MAX_IOAPICS];
    int ioapic_count;
    struct acpi_iso isos[ACPI_MAX_ISOS];
    int iso_count;
};

/* Parse RSDP -> RSDT/XSDT -> MADT. Never fails hard: without ACPI, present stays false. */
void acpi_init(const struct boot_info *bi);
const struct acpi_info *acpi_get(void);

#endif
