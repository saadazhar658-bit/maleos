#ifndef MALEOS_KERNEL_BOOTINFO_H
#define MALEOS_KERNEL_BOOTINFO_H

#include <stdbool.h>
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

#define BOOTINFO_MAX_MODULES 4
#define BOOTINFO_CMDLINE_MAX 48

/* A file GRUB loaded for us with the `module2` command (e.g. the initrd). */
struct boot_module {
    uint64_t start; /* physical */
    uint64_t end;   /* exclusive */
    char name[BOOTINFO_CMDLINE_MAX];
};

/* Everything the kernel needs from the bootloader, copied out of boot memory. */
struct boot_info {
    struct mem_region regions[BOOTINFO_MAX_REGIONS];
    size_t region_count;
    struct boot_module modules[BOOTINFO_MAX_MODULES];
    size_t module_count;
    uint8_t rsdp[36]; /* copy of the ACPI RSDP, if the bootloader provided one */
    bool has_rsdp;
    uint64_t mbi_phys; /* Multiboot2 info structure (reserved by the PMM) */
    uint64_t mbi_size;
};

/* Parse the Multiboot2 info structure. Must run while the boot identity map is active. */
void bootinfo_parse(uint64_t mbi_phys, struct boot_info *out);

#endif
