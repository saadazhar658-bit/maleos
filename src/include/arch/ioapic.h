#ifndef MALEOS_ARCH_IOAPIC_H
#define MALEOS_ARCH_IOAPIC_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel/acpi.h"

/* Map every I/O APIC found by ACPI (or the default one) and mask all inputs. */
void ioapic_init(const struct acpi_info *acpi);

/*
 * Route a legacy ISA interrupt (e.g. IRQ 1 = keyboard) to `vector` on the boot CPU,
 * applying any ACPI interrupt source override. Returns 0 on success, -1 if unroutable.
 */
int ioapic_route_isa(uint8_t isa_irq, uint8_t vector);

/* Route a global system interrupt directly (PCI interrupts are level triggered, active low). */
int ioapic_route_gsi(uint32_t gsi, uint8_t vector, bool active_low, bool level);

void ioapic_mask_gsi(uint32_t gsi);

/* The GSI a legacy ISA IRQ ends up on after ACPI overrides. */
uint32_t ioapic_isa_to_gsi(uint8_t isa_irq);

/* Diagnostics for the self-tests: raw redirection entry for a GSI, or ~0 if none. */
uint64_t ioapic_read_entry(uint32_t gsi);
int ioapic_count(void);

#endif
