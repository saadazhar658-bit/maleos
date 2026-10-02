#ifndef MALEOS_MM_VMM_H
#define MALEOS_MM_VMM_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel/bootinfo.h"

/* Protection flags. Read is always implied. */
#define VMM_WRITE (1u << 0)
#define VMM_EXEC (1u << 1)
#define VMM_USER (1u << 2)
#define VMM_NOCACHE (1u << 3)

/* Return codes. */
#define VMM_OK 0
#define VMM_ENOMEM (-1)
#define VMM_EEXIST (-2)
#define VMM_EINVAL (-3)
#define VMM_ENOENT (-4)

/*
 * Build the kernel address space (HHDM + W^X kernel image) and switch to it.
 * After this call the boot identity map is gone.
 */
void vmm_init(const struct boot_info *bi);

uint64_t vmm_kernel_pml4(void); /* physical address */

/*
 * Map one 4 KiB page. Writable+executable mappings are rejected (W^X).
 * Page tables are allocated from the PMM as needed.
 */
int vmm_map(uint64_t pml4, uint64_t virt, uint64_t phys, uint32_t prot);
int vmm_unmap(uint64_t pml4, uint64_t virt);
int vmm_protect(uint64_t pml4, uint64_t virt, uint32_t prot);

/* Walk the tables. Returns false if unmapped. phys/prot may be NULL. */
bool vmm_translate(uint64_t pml4, uint64_t virt, uint64_t *phys, uint32_t *prot);

#endif
