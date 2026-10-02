#ifndef MALEOS_KERNEL_BOOTINFO_H
#define MALEOS_KERNEL_BOOTINFO_H

#include <stddef.h>
#include <stdint.h>

/* Multiboot2 memory map entry types. */
#define MEM_AVAILABLE 1
#define MEM_RESERVED 2
#define MEM_ACPI_RECLAIMABLE 3
#define MEM_ACPI_NVS 4
#define MEM_BAD 5

#define BOOTINFO_MAX_REGIONS 64

struct mem_region {
    uint64_t base;
    uint64_t length;
    uint32_t type;
};

/* Everything the kernel needs from the bootloader, copied out of boot memory. */
struct boot_info {
    struct mem_region regions[BOOTINFO_MAX_REGIONS];
    size_t region_count;
    uint64_t mbi_phys; /* Multiboot2 info structure (reserved by the PMM) */
    uint64_t mbi_size;
};

/* Parse the Multiboot2 info structure. Must run while the boot identity map is active. */
void bootinfo_parse(uint64_t mbi_phys, struct boot_info *out);

#endif
