#include "mm/uspace.h"

#include "kernel/errno.h"
#include "kernel/string.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

bool user_range_ok(uint64_t va, uint64_t len)
{
    return va < USER_TOP && len <= USER_TOP - va;
}

/* Translate a user address for the requested access; returns its kernel-visible address. */
static uint8_t *user_page(uint64_t pml4, uint64_t va, bool write)
{
    uint64_t phys;
    uint32_t prot;
    if (!vmm_translate(pml4, va, &phys, &prot) || !(prot & VMM_USER))
        return NULL;
    if (write && !(prot & VMM_WRITE))
        return NULL;
    return (uint8_t *)phys_to_virt(phys);
}

int uspace_copy_in(uint64_t pml4, void *dst, uint64_t src_va, size_t n)
{
    if (!user_range_ok(src_va, n))
        return -EFAULT;
    uint8_t *out = dst;
    while (n) {
        size_t chunk = PAGE_SIZE - (src_va & (PAGE_SIZE - 1));
        if (chunk > n)
            chunk = n;
        const uint8_t *p = user_page(pml4, src_va, false);
        if (!p)
            return -EFAULT;
        memcpy(out, p, chunk);
        out += chunk;
        src_va += chunk;
        n -= chunk;
    }
    return 0;
}

bool uspace_writable(uint64_t pml4, uint64_t va, size_t n)
{
    if (!user_range_ok(va, n))
        return false;
    for (uint64_t page = ALIGN_DOWN(va, PAGE_SIZE); n && page < va + n; page += PAGE_SIZE) {
        if (!user_page(pml4, page, true))
            return false;
    }
    return true;
}

int uspace_copy_out(uint64_t pml4, uint64_t dst_va, const void *src, size_t n)
{
    /* Check every page first so a failure does not leave a half-written buffer behind. */
    if (!uspace_writable(pml4, dst_va, n))
        return -EFAULT;
    const uint8_t *in = src;
    while (n) {
        size_t chunk = PAGE_SIZE - (dst_va & (PAGE_SIZE - 1));
        if (chunk > n)
            chunk = n;
        uint8_t *p = user_page(pml4, dst_va, true);
        if (!p)
            return -EFAULT;
        memcpy(p, in, chunk);
        in += chunk;
        dst_va += chunk;
        n -= chunk;
    }
    return 0;
}

int uspace_strcpy_in(uint64_t pml4, char *dst, uint64_t src_va, size_t max)
{
    if (max == 0)
        return -ENAMETOOLONG;
    for (size_t i = 0; i < max; i++) {
        if (!user_range_ok(src_va + i, 1))
            return -EFAULT;
        const uint8_t *p = user_page(pml4, src_va + i, false);
        if (!p)
            return -EFAULT;
        char c = (char)*p;
        dst[i] = c;
        if (c == 0)
            return (int)i;
    }
    dst[max - 1] = 0;
    return -ENAMETOOLONG;
}

/*
 * The strcpy loop above translates once per byte. Strings handed to the kernel are short
 * (paths, arguments), so clarity wins over a per-page fast path.
 */

int uspace_map(uint64_t pml4, uint64_t va, size_t pages, uint32_t prot)
{
    if (!user_range_ok(va, pages * PAGE_SIZE) || (va & (PAGE_SIZE - 1)))
        return -EFAULT;
    size_t done = 0;
    int err = 0;
    for (; done < pages; done++) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame) {
            err = -ENOMEM;
            break;
        }
        memset(phys_to_virt(frame), 0, PAGE_SIZE);
        int rc = vmm_map(pml4, va + done * PAGE_SIZE, frame, prot | VMM_USER);
        if (rc != VMM_OK) {
            pmm_free_frame(frame);
            err = rc == VMM_EEXIST ? -EEXIST : rc == VMM_EINVAL ? -EINVAL : -ENOMEM;
            break;
        }
    }
    if (err) {
        uspace_unmap(pml4, va, done);
        return err;
    }
    return 0;
}

void uspace_unmap(uint64_t pml4, uint64_t va, size_t pages)
{
    for (size_t i = 0; i < pages; i++) {
        uint64_t phys;
        uint64_t page = va + i * PAGE_SIZE;
        if (vmm_translate(pml4, page, &phys, NULL)) {
            vmm_unmap(pml4, page);
            pmm_free_frame(phys);
        }
    }
}
