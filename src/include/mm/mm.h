#ifndef MALEOS_MM_MM_H
#define MALEOS_MM_MM_H

#include <stdbool.h>
#include <stdint.h>

#define PAGE_SIZE 4096ULL
#define PAGE_SHIFT 12
#define HUGE_PAGE_SIZE (2ULL * 1024 * 1024)

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((uint64_t)(a) - 1))

/*
 * Virtual address space layout (kernel half):
 *
 *   0xFFFF800000000000  HHDM_BASE   direct map of all physical RAM (RW, NX)
 *   0xFFFFC00000000000  KHEAP_BASE  kernel heap (RW, NX), grows on demand
 *   0xFFFFFFFF80000000  KERNEL_VMA  kernel image (W^X), virtual = KERNEL_VMA + physical
 */
#define HHDM_BASE 0xFFFF800000000000ULL
#define KHEAP_BASE 0xFFFFC00000000000ULL
#define KERNEL_VMA 0xFFFFFFFF80000000ULL

/* Before vmm_init() only the first 1 GiB is reachable, through the boot identity map. */
#define BOOT_IDENTITY_LIMIT (1ULL << 30)

extern bool hhdm_ready;

static inline void *phys_to_virt(uint64_t phys)
{
    return (void *)(hhdm_ready ? HHDM_BASE + phys : phys);
}

#endif
