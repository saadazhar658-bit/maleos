#include "mm/pmm.h"

#include "kernel/printk.h"
#include "kernel/string.h"
#include "mm/mm.h"

/*
 * Physical memory manager: one bit per 4 KiB frame (1 = used, 0 = free).
 * The bitmap is static (256 KiB covers PMM_MAX_PHYS), so it needs no memory
 * allocator to bootstrap.
 */

#define PMM_MAX_FRAMES (PMM_MAX_PHYS / PAGE_SIZE)
#define WORDS (PMM_MAX_FRAMES / 64)

extern char __kernel_phys_start[];
extern char __kernel_phys_end[];

static uint64_t bitmap[WORDS];
static uint64_t frame_limit; /* frames [0, frame_limit) are tracked */
static uint64_t usable_frames;
static uint64_t free_frames;
static uint64_t highest_phys;
static uint64_t hint_word;

static inline bool frame_used(uint64_t f)
{
    return bitmap[f / 64] & (1ULL << (f % 64));
}

static inline void mark_used(uint64_t f)
{
    bitmap[f / 64] |= 1ULL << (f % 64);
}

static inline void mark_free(uint64_t f)
{
    bitmap[f / 64] &= ~(1ULL << (f % 64));
}

/* Frames [first, last) */
static void release_range(uint64_t first, uint64_t last)
{
    for (uint64_t f = first; f < last && f < PMM_MAX_FRAMES; f++)
        mark_free(f);
}

static void reserve_range(uint64_t start, uint64_t end)
{
    uint64_t first = ALIGN_DOWN(start, PAGE_SIZE) / PAGE_SIZE;
    uint64_t last = ALIGN_UP(end, PAGE_SIZE) / PAGE_SIZE;
    for (uint64_t f = first; f < last && f < PMM_MAX_FRAMES; f++)
        mark_used(f);
}

void pmm_init(const struct boot_info *bi)
{
    memset(bitmap, 0xFF, sizeof(bitmap));

    /* 1. Free everything the firmware says is usable RAM. */
    for (size_t i = 0; i < bi->region_count; i++) {
        const struct mem_region *r = &bi->regions[i];
        if (r->type != MEM_AVAILABLE)
            continue;

        uint64_t start = ALIGN_UP(r->base, PAGE_SIZE);
        uint64_t end = ALIGN_DOWN(r->base + r->length, PAGE_SIZE);
        if (end > PMM_MAX_PHYS)
            end = PMM_MAX_PHYS;
        if (end <= start)
            continue;

        release_range(start / PAGE_SIZE, end / PAGE_SIZE);
        if (end > highest_phys)
            highest_phys = end;
    }

    /* 2. Anything non-usable that overlaps wins. */
    for (size_t i = 0; i < bi->region_count; i++) {
        const struct mem_region *r = &bi->regions[i];
        if (r->type != MEM_AVAILABLE)
            reserve_range(r->base, r->base + r->length);
    }

    /* 3. Low memory (BIOS data, VGA, option ROMs), the kernel image, boot info. */
    reserve_range(0, 0x100000);
    reserve_range((uint64_t)__kernel_phys_start, (uint64_t)__kernel_phys_end);
    reserve_range(bi->mbi_phys, bi->mbi_phys + bi->mbi_size);

    frame_limit = highest_phys / PAGE_SIZE;
    for (uint64_t f = 0; f < frame_limit; f++) {
        if (!frame_used(f))
            usable_frames++;
    }
    free_frames = usable_frames;
}

uint64_t pmm_alloc_frame(void)
{
    uint64_t words = (frame_limit + 63) / 64;

    for (uint64_t n = 0; n < words; n++) {
        uint64_t w = (hint_word + n) % words;
        if (bitmap[w] == ~0ULL)
            continue;

        uint64_t bit = (uint64_t)__builtin_ctzll(~bitmap[w]);
        uint64_t f = w * 64 + bit;
        if (f >= frame_limit)
            continue;

        mark_used(f);
        free_frames--;
        hint_word = w;
        return f * PAGE_SIZE;
    }
    return 0;
}

uint64_t pmm_alloc_frames(uint64_t count)
{
    if (count == 0)
        return 0;
    if (count == 1)
        return pmm_alloc_frame();

    uint64_t run = 0;
    for (uint64_t f = 0; f < frame_limit; f++) {
        if (frame_used(f)) {
            run = 0;
            continue;
        }
        if (++run == count) {
            uint64_t first = f - count + 1;
            for (uint64_t i = first; i <= f; i++)
                mark_used(i);
            free_frames -= count;
            return first * PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free_frame(uint64_t phys)
{
    if (phys % PAGE_SIZE || phys / PAGE_SIZE >= frame_limit)
        kpanic("pmm_free_frame: bad address %p", (void *)phys);

    uint64_t f = phys / PAGE_SIZE;
    if (!frame_used(f))
        kpanic("pmm_free_frame: double free of %p", (void *)phys);

    mark_free(f);
    free_frames++;
    if (f / 64 < hint_word)
        hint_word = f / 64;
}

void pmm_free_frames(uint64_t phys, uint64_t count)
{
    for (uint64_t i = 0; i < count; i++)
        pmm_free_frame(phys + i * PAGE_SIZE);
}

uint64_t pmm_total_frames(void)
{
    return usable_frames;
}

uint64_t pmm_free_frame_count(void)
{
    return free_frames;
}

uint64_t pmm_highest_phys(void)
{
    return highest_phys;
}
