#include "mm/selftest.h"

#include "arch/cpu.h"
#include "kernel/printk.h"
#include "kernel/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

static int checks;

#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond))                                                                               \
            kpanic("mm selftest failed: %s (%s:%d)", what, __FILE__, __LINE__);                    \
    } while (0)

extern char __stack_guard[];

static int data_probe = 42; /* lives in .data */

static void pmm_tests(void)
{
    uint64_t before = pmm_free_frame_count();

    uint64_t a = pmm_alloc_frame();
    uint64_t b = pmm_alloc_frame();
    CHECK(a && b && a != b, "pmm: distinct frames");
    CHECK(!(a & (PAGE_SIZE - 1)), "pmm: frames are page aligned");
    CHECK(a >= 0x100000, "pmm: never hands out low memory");
    CHECK(pmm_free_frame_count() == before - 2, "pmm: free count drops");

    pmm_free_frame(a);
    pmm_free_frame(b);
    CHECK(pmm_free_frame_count() == before, "pmm: free count restored");

    uint64_t run = pmm_alloc_frames(8);
    CHECK(run != 0, "pmm: contiguous allocation");
    CHECK(pmm_free_frame_count() == before - 8, "pmm: contiguous count");
    pmm_free_frames(run, 8);
    CHECK(pmm_free_frame_count() == before, "pmm: contiguous free");
}

static void vmm_tests(void)
{
    uint64_t pml4 = vmm_kernel_pml4();
    uint64_t phys;
    uint32_t prot;

    /* map / use / translate / unmap */
    const uint64_t va = 0xFFFFD00000000000ULL;
    uint64_t frame = pmm_alloc_frame();
    CHECK(frame != 0, "vmm: got a frame");
    CHECK(vmm_map(pml4, va, frame, VMM_WRITE) == VMM_OK, "vmm: map");
    CHECK(vmm_map(pml4, va, frame, VMM_WRITE) == VMM_EEXIST, "vmm: double map rejected");

    *(volatile uint64_t *)va = 0x1122334455667788ULL;
    CHECK(*(volatile uint64_t *)phys_to_virt(frame) == 0x1122334455667788ULL,
          "vmm: mapping and direct map alias the same frame");

    CHECK(vmm_translate(pml4, va + 0x123, &phys, &prot), "vmm: translate mapped");
    CHECK(phys == frame + 0x123, "vmm: translate address");
    CHECK((prot & VMM_WRITE) && !(prot & VMM_EXEC), "vmm: RW+NX flags");

    CHECK(vmm_protect(pml4, va, 0) == VMM_OK, "vmm: protect to read-only");
    CHECK(vmm_translate(pml4, va, NULL, &prot) && !(prot & VMM_WRITE), "vmm: now read-only");

    CHECK(vmm_unmap(pml4, va) == VMM_OK, "vmm: unmap");
    CHECK(!vmm_translate(pml4, va, NULL, NULL), "vmm: unmapped after unmap");
    CHECK(vmm_unmap(pml4, va) == VMM_ENOENT, "vmm: unmap twice");
    pmm_free_frame(frame);

    /* W^X policy */
    CHECK(vmm_map(pml4, va, 0x200000, VMM_WRITE | VMM_EXEC) == VMM_EINVAL, "vmm: W+X rejected");
    CHECK(vmm_map(pml4, va + 1, 0x200000, VMM_WRITE) == VMM_EINVAL, "vmm: unaligned rejected");

    /* Kernel image permissions */
    CHECK(vmm_translate(pml4, (uint64_t)&vmm_tests, NULL, &prot), "kernel: text mapped");
    CHECK((prot & VMM_EXEC) && !(prot & VMM_WRITE), "kernel: text is R+X, not W");

    CHECK(vmm_translate(pml4, (uint64_t) "rodata probe", NULL, &prot), "kernel: rodata mapped");
    CHECK(!(prot & VMM_EXEC) && !(prot & VMM_WRITE), "kernel: rodata is R only");

    CHECK(vmm_translate(pml4, (uint64_t)&data_probe, NULL, &prot), "kernel: data mapped");
    CHECK((prot & VMM_WRITE) && !(prot & VMM_EXEC), "kernel: data is RW+NX");

    CHECK(vmm_translate(pml4, (uint64_t)__stack_guard - 1, NULL, &prot),
          "stack: page below mapped");
    CHECK(vmm_translate(pml4, (uint64_t)__stack_guard + PAGE_SIZE, NULL, NULL),
          "stack: stack mapped");
    CHECK(!vmm_translate(pml4, (uint64_t)__stack_guard, NULL, NULL), "stack: guard page unmapped");

    /* Direct map */
    CHECK(vmm_translate(pml4, HHDM_BASE + 0x1234000, &phys, &prot), "hhdm: mapped");
    CHECK(phys == 0x1234000 && (prot & VMM_WRITE) && !(prot & VMM_EXEC), "hhdm: RW+NX identity");

    /* The boot identity map must be gone. */
    CHECK(!vmm_translate(pml4, 0x100000, NULL, NULL), "boot identity map removed");
}

static void heap_tests(void)
{
    struct heap_stats s0, s1;
    heap_get_stats(&s0);
    CHECK(heap_check() == 0, "heap: consistent at start");

    uint8_t *a = kmalloc(100);
    uint8_t *b = kmalloc(200);
    uint8_t *c = kmalloc(300);
    CHECK(a && b && c, "heap: small allocations");
    CHECK(!((uint64_t)a & 15) && !((uint64_t)b & 15) && !((uint64_t)c & 15),
          "heap: 16-byte aligned");
    CHECK(a + 100 <= b && b + 200 <= c, "heap: allocations do not overlap");

    memset(a, 0xAA, 100);
    memset(b, 0xBB, 200);
    memset(c, 0xCC, 300);
    CHECK(a[99] == 0xAA && b[0] == 0xBB && c[299] == 0xCC, "heap: data intact");

    kfree(b);
    CHECK(heap_check() == 0, "heap: consistent after free");
    uint8_t *b2 = kmalloc(150); /* should reuse b's hole */
    CHECK(b2 == b, "heap: freed block is reused");
    kfree(b2);
    kfree(a);
    kfree(c);
    CHECK(heap_check() == 0, "heap: consistent after freeing all");

    heap_get_stats(&s1);
    CHECK(s1.used_bytes == s0.used_bytes, "heap: no leaked bytes");
    CHECK(s1.free_block_count == 1, "heap: fully coalesced");

    /* zeroed allocation */
    uint8_t *z = kzalloc(4096);
    int zero = 1;
    for (int i = 0; z && i < 4096; i++)
        zero &= (z[i] == 0);
    CHECK(z && zero, "heap: kzalloc returns zeroed memory");
    kfree(z);

    /* realloc keeps contents */
    uint32_t *r = kmalloc(16 * sizeof(uint32_t));
    for (uint32_t i = 0; i < 16; i++)
        r[i] = i * 7;
    r = krealloc(r, 4096 * sizeof(uint32_t));
    int same = 1;
    for (uint32_t i = 0; r && i < 16; i++)
        same &= (r[i] == i * 7);
    CHECK(r && same, "heap: realloc preserves data");
    kfree(r);

    /* growth: a block far larger than the initial heap */
    uint64_t free_before = pmm_free_frame_count();
    uint8_t *big = kmalloc(2 * 1024 * 1024);
    CHECK(big != NULL, "heap: grows for a 2 MiB allocation");
    CHECK(pmm_free_frame_count() < free_before, "heap: growth consumed frames");
    big[0] = 1;
    big[2 * 1024 * 1024 - 1] = 2;
    CHECK(heap_check() == 0, "heap: consistent after growth");
    kfree(big);
    CHECK(heap_check() == 0, "heap: consistent after freeing the big block");

    /* many small allocations, freed in a scrambled order */
    void *ptrs[64];
    for (int i = 0; i < 64; i++) {
        ptrs[i] = kmalloc(16 + (i * 37) % 500);
        CHECK(ptrs[i] != NULL, "heap: stress allocation");
    }
    for (int i = 0; i < 64; i += 2)
        kfree(ptrs[i]);
    for (int i = 1; i < 64; i += 2)
        kfree(ptrs[i]);
    CHECK(heap_check() == 0, "heap: consistent after stress");

    heap_get_stats(&s1);
    CHECK(s1.used_bytes == s0.used_bytes && s1.free_block_count == 1,
          "heap: stress leaves no leaks");
    CHECK(kmalloc(0) == NULL, "heap: zero-size allocation returns NULL");
}

int mm_selftest(void)
{
    checks = 0;
    pmm_tests();
    vmm_tests();
    heap_tests();
    return checks;
}
