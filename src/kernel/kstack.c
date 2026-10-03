#include "kernel/kstack.h"

#include <stdbool.h>

#include "kernel/spinlock.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

static bool slot_used[KSTACK_MAX];
static spinlock_t kstack_lock = SPINLOCK_INIT;

uint64_t kstack_alloc(int *slot, uint64_t *base)
{
    uint64_t flags = spin_lock_irqsave(&kstack_lock);
    int s = -1;
    for (int i = 0; i < KSTACK_MAX; i++) {
        if (!slot_used[i]) {
            s = i;
            slot_used[i] = true;
            break;
        }
    }
    spin_unlock_irqrestore(&kstack_lock, flags);
    if (s < 0)
        return 0;

    uint64_t va = KSTACK_BASE + (uint64_t)s * KSTACK_SLOT_PAGES * PAGE_SIZE;
    uint64_t pml4 = vmm_kernel_pml4();

    for (int i = 0; i < KSTACK_PAGES; i++) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame || vmm_map(pml4, va + (1 + i) * PAGE_SIZE, frame, VMM_WRITE) != VMM_OK) {
            if (frame)
                pmm_free_frame(frame);
            for (int j = 0; j < i; j++) { /* roll back */
                uint64_t phys;
                if (vmm_translate(pml4, va + (1 + j) * PAGE_SIZE, &phys, NULL)) {
                    vmm_unmap(pml4, va + (1 + j) * PAGE_SIZE);
                    pmm_free_frame(phys);
                }
            }
            flags = spin_lock_irqsave(&kstack_lock);
            slot_used[s] = false;
            spin_unlock_irqrestore(&kstack_lock, flags);
            return 0;
        }
    }

    *slot = s;
    *base = va;
    return va + (1 + KSTACK_PAGES) * PAGE_SIZE;
}

void kstack_free(int slot)
{
    if (slot < 0 || slot >= KSTACK_MAX)
        return;

    uint64_t va = KSTACK_BASE + (uint64_t)slot * KSTACK_SLOT_PAGES * PAGE_SIZE;
    uint64_t pml4 = vmm_kernel_pml4();

    for (int i = 0; i < KSTACK_PAGES; i++) {
        uint64_t phys;
        uint64_t page = va + (1 + i) * PAGE_SIZE;
        if (vmm_translate(pml4, page, &phys, NULL)) {
            vmm_unmap(pml4, page);
            pmm_free_frame(phys);
        }
    }

    uint64_t flags = spin_lock_irqsave(&kstack_lock);
    slot_used[slot] = false;
    spin_unlock_irqrestore(&kstack_lock, flags);
}
