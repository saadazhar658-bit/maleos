#ifndef MALEOS_MM_PMM_H
#define MALEOS_MM_PMM_H

#include <stdint.h>

#include "kernel/bootinfo.h"

/* Physical memory above this is ignored (bitmap is statically sized). */
#define PMM_MAX_PHYS (8ULL << 30)

/* Build the frame bitmap from the boot memory map and reserve kernel/boot memory. */
void pmm_init(const struct boot_info *bi);

/* Each returns a page-aligned physical address, or 0 on failure (frame 0 is never free). */
uint64_t pmm_alloc_frame(void);
uint64_t pmm_alloc_frames(uint64_t count); /* physically contiguous */

void pmm_free_frame(uint64_t phys);
void pmm_free_frames(uint64_t phys, uint64_t count);

uint64_t pmm_total_frames(void); /* usable frames at boot */
uint64_t pmm_free_frame_count(void);
uint64_t pmm_highest_phys(void); /* end of the highest usable region */

#endif
