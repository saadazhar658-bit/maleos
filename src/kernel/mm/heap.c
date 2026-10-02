#include "mm/heap.h"

#include "kernel/printk.h"
#include "kernel/string.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

/*
 * Kernel heap.
 *
 * A contiguous virtual region starting at KHEAP_BASE that grows a few pages at
 * a time (frames from the PMM, mapped RW+NX). It is carved into blocks, each
 * with a 32-byte header; blocks are walked in address order, split on
 * allocation and coalesced with both neighbours on free. Memory just past the
 * end of the heap is unmapped, so running off the end faults.
 */

#define HEAP_LIMIT (256ULL << 20)
#define HEAP_MIN_GROW (16 * PAGE_SIZE)
#define HEAP_MAGIC 0xA110C8ED5AFEB10CULL
#define MIN_SPLIT 32 /* smallest payload worth splitting off */

struct block {
    uint64_t magic;
    uint64_t size;      /* payload bytes */
    uint64_t prev_size; /* payload bytes of the previous block, 0 for the first */
    uint64_t free;
};

#define HDR sizeof(struct block)

static uint64_t heap_end = KHEAP_BASE; /* first unmapped heap address */
static struct block *heap_last;

static inline struct block *first_block(void)
{
    return (struct block *)KHEAP_BASE;
}

static inline struct block *next_block(struct block *b)
{
    struct block *n = (struct block *)((uint8_t *)(b + 1) + b->size);
    return (uint64_t)n >= heap_end ? NULL : n;
}

static inline struct block *prev_block(struct block *b)
{
    if (b == first_block())
        return NULL;
    return (struct block *)((uint8_t *)b - HDR - b->prev_size);
}

static void check_magic(struct block *b, const char *where)
{
    if (b->magic != HEAP_MAGIC)
        kpanic("heap corruption detected in %s at %p", where, b);
}

/* Map `bytes` more of heap; returns false (and changes nothing) on failure. */
static bool map_more(uint64_t bytes)
{
    if (heap_end + bytes > KHEAP_BASE + HEAP_LIMIT)
        return false;

    uint64_t pml4 = vmm_kernel_pml4();
    uint64_t done = 0;
    for (; done < bytes; done += PAGE_SIZE) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame)
            break;
        if (vmm_map(pml4, heap_end + done, frame, VMM_WRITE) != VMM_OK) {
            pmm_free_frame(frame);
            break;
        }
    }

    if (done < bytes) { /* roll back */
        for (uint64_t off = 0; off < done; off += PAGE_SIZE) {
            uint64_t phys;
            if (vmm_translate(pml4, heap_end + off, &phys, NULL)) {
                vmm_unmap(pml4, heap_end + off);
                pmm_free_frame(phys);
            }
        }
        return false;
    }
    return true;
}

/* Grow the heap so that a payload of at least `need` bytes can fit. */
static bool heap_grow(uint64_t need)
{
    bool extend_last = heap_last && heap_last->free;
    uint64_t bytes;

    if (extend_last)
        bytes = need > heap_last->size ? need - heap_last->size : 0;
    else
        bytes = need + HDR;

    bytes = ALIGN_UP(bytes, PAGE_SIZE);
    if (bytes < HEAP_MIN_GROW)
        bytes = HEAP_MIN_GROW;

    if (!map_more(bytes))
        return false;

    if (extend_last) {
        heap_last->size += bytes;
    } else {
        struct block *b = (struct block *)heap_end;
        b->magic = HEAP_MAGIC;
        b->size = bytes - HDR;
        b->prev_size = heap_last ? heap_last->size : 0;
        b->free = 1;
        heap_last = b;
    }
    heap_end += bytes;
    return true;
}

void heap_init(void)
{
    KASSERT(heap_last == NULL);
    if (!heap_grow(HEAP_MIN_GROW - HDR))
        kpanic("heap: cannot create the initial heap");
}

void *kmalloc(size_t size)
{
    if (size == 0 || size > HEAP_LIMIT)
        return NULL;
    uint64_t need = ALIGN_UP((uint64_t)size, 16);

    for (;;) {
        for (struct block *b = first_block(); b; b = next_block(b)) {
            check_magic(b, "kmalloc");
            if (!b->free || b->size < need)
                continue;

            if (b->size >= need + HDR + MIN_SPLIT) {
                struct block *n = (struct block *)((uint8_t *)(b + 1) + need);
                n->magic = HEAP_MAGIC;
                n->size = b->size - need - HDR;
                n->prev_size = need;
                n->free = 1;

                struct block *after = next_block(n);
                if (after)
                    after->prev_size = n->size;
                else
                    heap_last = n;

                b->size = need;
            }
            b->free = 0;
            return b + 1;
        }

        if (!heap_grow(need))
            return NULL;
    }
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    uint64_t addr = (uint64_t)ptr;
    if (addr < KHEAP_BASE + HDR || addr >= heap_end || (addr & 15))
        kpanic("kfree: %p is not a heap pointer", ptr);

    struct block *b = (struct block *)ptr - 1;
    check_magic(b, "kfree");
    if (b->free)
        kpanic("kfree: double free of %p", ptr);

    memset(ptr, 0xDD, b->size); /* poison to expose use-after-free */
    b->free = 1;

    struct block *n = next_block(b);
    if (n && n->free) {
        check_magic(n, "kfree (next)");
        b->size += HDR + n->size;
        struct block *after = next_block(b);
        if (after)
            after->prev_size = b->size;
        if (heap_last == n)
            heap_last = b;
    }

    struct block *p = prev_block(b);
    if (p && p->free) {
        check_magic(p, "kfree (prev)");
        p->size += HDR + b->size;
        struct block *after = next_block(p);
        if (after)
            after->prev_size = p->size;
        if (heap_last == b)
            heap_last = p;
    }
}

void *krealloc(void *ptr, size_t size)
{
    if (!ptr)
        return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }

    struct block *b = (struct block *)ptr - 1;
    check_magic(b, "krealloc");
    if (b->size >= size)
        return ptr;

    void *n = kmalloc(size);
    if (!n)
        return NULL;
    memcpy(n, ptr, b->size);
    kfree(ptr);
    return n;
}

int heap_check(void)
{
    uint64_t covered = 0;
    uint64_t prev_size = 0;
    struct block *prev = NULL;

    for (struct block *b = first_block(); b; b = next_block(b)) {
        if (b->magic != HEAP_MAGIC)
            return -1;
        if (b->prev_size != prev_size)
            return -2;
        if (b->size & 15)
            return -3;
        if (prev && prev->free && b->free)
            return -4; /* neighbours should have been coalesced */
        covered += HDR + b->size;
        prev_size = b->size;
        prev = b;
    }

    if (covered != heap_end - KHEAP_BASE)
        return -5;
    if (prev != heap_last)
        return -6;
    return 0;
}

void heap_get_stats(struct heap_stats *out)
{
    memset(out, 0, sizeof(*out));
    out->mapped_bytes = heap_end - KHEAP_BASE;
    for (struct block *b = first_block(); heap_last && b; b = next_block(b)) {
        out->block_count++;
        if (b->free) {
            out->free_bytes += b->size;
            out->free_block_count++;
        } else {
            out->used_bytes += b->size;
        }
    }
}
