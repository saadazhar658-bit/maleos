#include "mm/vmm.h"

#include "arch/cpu.h"
#include "kernel/printk.h"
#include "kernel/string.h"
#include "mm/mm.h"
#include "mm/pmm.h"

/*
 * Virtual memory manager for 4-level x86_64 paging.
 *
 * Policy:
 *   - W^X: no mapping is ever writable and executable.
 *   - The kernel image is mapped per section (text RX, rodata R, data/bss RW+NX).
 *   - The kernel stack's lowest page is left unmapped as a guard page.
 *   - All RAM is direct-mapped at HHDM_BASE with 2 MiB pages (RW, NX).
 */

#define PTE_PRESENT (1ULL << 0)
#define PTE_WRITE (1ULL << 1)
#define PTE_USER (1ULL << 2)
#define PTE_PWT (1ULL << 3)
#define PTE_PCD (1ULL << 4)
#define PTE_HUGE (1ULL << 7)
#define PTE_NX (1ULL << 63)
#define PTE_ADDR 0x000FFFFFFFFFF000ULL

#define IDX(virt, shift) (((virt) >> (shift)) & 0x1FF)

extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], __data_end[];
extern char __stack_guard[];

bool hhdm_ready;
static uint64_t kernel_pml4;

static inline uint64_t *table_virt(uint64_t phys)
{
    return (uint64_t *)phys_to_virt(phys);
}

static uint64_t alloc_table(void)
{
    uint64_t phys = pmm_alloc_frame();
    if (!phys)
        return 0;
    if (!hhdm_ready && phys >= BOOT_IDENTITY_LIMIT) {
        /* Cannot touch it yet; the allocator hands out low frames first, so this is rare. */
        pmm_free_frame(phys);
        return 0;
    }
    memset(table_virt(phys), 0, PAGE_SIZE);
    return phys;
}

static bool canonical(uint64_t v)
{
    uint64_t top = v >> 47;
    return top == 0 || top == 0x1FFFF;
}

static uint64_t prot_to_flags(uint32_t prot)
{
    uint64_t f = PTE_PRESENT;
    if (prot & VMM_WRITE)
        f |= PTE_WRITE;
    if (prot & VMM_USER)
        f |= PTE_USER;
    if (prot & VMM_NOCACHE)
        f |= PTE_PCD | PTE_PWT;
    if (!(prot & VMM_EXEC))
        f |= PTE_NX;
    return f;
}

static uint32_t flags_to_prot(uint64_t f)
{
    uint32_t p = 0;
    if (f & PTE_WRITE)
        p |= VMM_WRITE;
    if (f & PTE_USER)
        p |= VMM_USER;
    if (f & (PTE_PCD | PTE_PWT))
        p |= VMM_NOCACHE;
    if (!(f & PTE_NX))
        p |= VMM_EXEC;
    return p;
}

/*
 * Walk down to the table that holds the entry for `virt` at the target level.
 * stop_shift is 12 for the PT (4 KiB pages) or 21 for the PD (2 MiB pages).
 * Returns the table's virtual address, or NULL.
 */
static uint64_t *walk(uint64_t pml4, uint64_t virt, int stop_shift, bool create, bool user,
                      int *err)
{
    uint64_t *table = table_virt(pml4);

    for (int shift = 39; shift > stop_shift; shift -= 9) {
        uint64_t *e = &table[IDX(virt, shift)];

        if (!(*e & PTE_PRESENT)) {
            if (!create) {
                *err = VMM_ENOENT;
                return NULL;
            }
            uint64_t t = alloc_table();
            if (!t) {
                *err = VMM_ENOMEM;
                return NULL;
            }
            /* Intermediate entries stay permissive; the leaf decides W/NX. */
            *e = t | PTE_PRESENT | PTE_WRITE | (user ? PTE_USER : 0);
        } else if (*e & PTE_HUGE) {
            *err = VMM_EEXIST; /* a large page already covers this address */
            return NULL;
        } else if (user) {
            *e |= PTE_USER;
        }
        table = table_virt(*e & PTE_ADDR);
    }
    return table;
}

int vmm_map(uint64_t pml4, uint64_t virt, uint64_t phys, uint32_t prot)
{
    if ((virt | phys) & (PAGE_SIZE - 1) || !canonical(virt))
        return VMM_EINVAL;
    if ((prot & VMM_WRITE) && (prot & VMM_EXEC))
        return VMM_EINVAL; /* W^X */

    int err = VMM_OK;
    uint64_t *pt = walk(pml4, virt, 12, true, prot & VMM_USER, &err);
    if (!pt)
        return err;

    uint64_t *e = &pt[IDX(virt, 12)];
    if (*e & PTE_PRESENT)
        return VMM_EEXIST;

    *e = phys | prot_to_flags(prot);
    return VMM_OK;
}

int vmm_unmap(uint64_t pml4, uint64_t virt)
{
    if ((virt & (PAGE_SIZE - 1)) || !canonical(virt))
        return VMM_EINVAL;

    int err = VMM_OK;
    uint64_t *pt = walk(pml4, virt, 12, false, false, &err);
    if (!pt)
        return err;

    uint64_t *e = &pt[IDX(virt, 12)];
    if (!(*e & PTE_PRESENT))
        return VMM_ENOENT;

    *e = 0;
    invlpg(virt);
    return VMM_OK;
}

int vmm_protect(uint64_t pml4, uint64_t virt, uint32_t prot)
{
    if ((virt & (PAGE_SIZE - 1)) || !canonical(virt))
        return VMM_EINVAL;
    if ((prot & VMM_WRITE) && (prot & VMM_EXEC))
        return VMM_EINVAL;

    int err = VMM_OK;
    uint64_t *pt = walk(pml4, virt, 12, false, false, &err);
    if (!pt)
        return err;

    uint64_t *e = &pt[IDX(virt, 12)];
    if (!(*e & PTE_PRESENT))
        return VMM_ENOENT;

    *e = (*e & PTE_ADDR) | prot_to_flags(prot);
    invlpg(virt);
    return VMM_OK;
}

bool vmm_translate(uint64_t pml4, uint64_t virt, uint64_t *phys, uint32_t *prot)
{
    if (!canonical(virt))
        return false;

    uint64_t *table = table_virt(pml4);
    for (int shift = 39; shift >= 12; shift -= 9) {
        uint64_t e = table[IDX(virt, shift)];
        if (!(e & PTE_PRESENT))
            return false;

        /* Large pages: 1 GiB at the PDPT level, 2 MiB at the PD level. */
        if (shift > 12 && (e & PTE_HUGE) && (shift == 30 || shift == 21)) {
            uint64_t size = 1ULL << shift;
            if (phys)
                *phys = (e & PTE_ADDR & ~(size - 1)) | (virt & (size - 1));
            if (prot)
                *prot = flags_to_prot(e);
            return true;
        }
        if (shift == 12) {
            if (phys)
                *phys = (e & PTE_ADDR) | (virt & (PAGE_SIZE - 1));
            if (prot)
                *prot = flags_to_prot(e);
            return true;
        }
        table = table_virt(e & PTE_ADDR);
    }
    return false;
}

static void map_huge(uint64_t pml4, uint64_t virt, uint64_t phys)
{
    int err = VMM_OK;
    uint64_t *pd = walk(pml4, virt, 21, true, false, &err);
    if (!pd)
        kpanic("vmm: cannot build direct map (error %d)", err);
    pd[IDX(virt, 21)] = phys | PTE_PRESENT | PTE_WRITE | PTE_HUGE | PTE_NX;
}

static void map_kernel_range(uint64_t pml4, const char *start, const char *end, uint32_t prot,
                             const char *skip_page)
{
    for (uint64_t v = (uint64_t)start; v < ALIGN_UP((uint64_t)end, PAGE_SIZE); v += PAGE_SIZE) {
        if (skip_page && v == (uint64_t)skip_page)
            continue; /* guard page stays unmapped */
        int r = vmm_map(pml4, v, v - KERNEL_VMA, prot);
        if (r != VMM_OK)
            kpanic("vmm: mapping kernel page %p failed (%d)", (void *)v, r);
    }
}

void vmm_init(const struct boot_info *bi)
{
    (void)bi;

    kernel_pml4 = alloc_table();
    if (!kernel_pml4)
        kpanic("vmm: out of memory for the kernel PML4");

    /* Direct map of all RAM (at least 16 MiB so low legacy addresses like VGA are covered). */
    uint64_t top = pmm_highest_phys();
    if (top < (16ULL << 20))
        top = 16ULL << 20;
    top = ALIGN_UP(top, HUGE_PAGE_SIZE);
    for (uint64_t p = 0; p < top; p += HUGE_PAGE_SIZE)
        map_huge(kernel_pml4, HHDM_BASE + p, p);

    /* Kernel image with W^X per section. */
    map_kernel_range(kernel_pml4, __text_start, __text_end, VMM_EXEC, NULL);
    map_kernel_range(kernel_pml4, __rodata_start, __rodata_end, 0, NULL);
    map_kernel_range(kernel_pml4, __data_start, __data_end, VMM_WRITE, __stack_guard);

    write_cr3(kernel_pml4); /* the boot identity map disappears here */
    hhdm_ready = true;
}

uint64_t vmm_kernel_pml4(void)
{
    return kernel_pml4;
}
