#ifndef MALEOS_KERNEL_KSTACK_H
#define MALEOS_KERNEL_KSTACK_H

#include <stdint.h>

#define KSTACK_PAGES 4      /* usable stack: 16 KiB */
#define KSTACK_SLOT_PAGES 8 /* virtual slot: guard page + stack + unmapped slack */
#define KSTACK_MAX 512

/*
 * Allocate a kernel stack. Layout of a slot: page 0 is an unmapped guard page, pages 1..4
 * are the stack, the rest is unmapped. Returns the initial stack pointer (top), or 0.
 */
uint64_t kstack_alloc(int *slot, uint64_t *base);
void kstack_free(int slot);

#endif
