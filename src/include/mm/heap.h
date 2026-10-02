#ifndef MALEOS_MM_HEAP_H
#define MALEOS_MM_HEAP_H

#include <stddef.h>
#include <stdint.h>

struct heap_stats {
    uint64_t mapped_bytes; /* virtual heap size currently backed by frames */
    uint64_t used_bytes;   /* payload bytes handed out */
    uint64_t free_bytes;   /* payload bytes available */
    uint64_t block_count;
    uint64_t free_block_count;
};

void heap_init(void);

void *kmalloc(size_t size); /* 16-byte aligned, NULL on failure or size 0 */
void *kzalloc(size_t size);
void *krealloc(void *ptr, size_t size);
void kfree(void *ptr);

/* Walk every block and verify headers. Returns 0 if the heap is consistent. */
int heap_check(void);
void heap_get_stats(struct heap_stats *out);

#endif
